// The look library. Every look is a pure function of the Moment and one LED: no pixel history, so
// every controller and the simulator draw the same picture for the same Moment.
//
// Fields are sampled in room space: rn (distance from the stage) for rings, u (west to east) for
// sweeps, a (angle around the room centre) for beams. Roles split the work the way a lighting desk
// splits fixtures: the cove carries the movement, the band echoes it, the edge frames and accents,
// and the stage answers the kick and nothing else, so the TV wall never flickers off the beat.
#include "looks.h"

#include <math.h>

namespace orion {

namespace {

constexpr float kBeatsPerPhrase = kBeatsPerBar * kBarsPerPhrase;
// Whole-room flashes start at least this far apart: at most 3 per second (WCAG 2.3.1).
constexpr float kFlashGap = 0.34f;
constexpr Rgbf kBlack = {0, 0, 0};
constexpr Rgbf kWhite = {1, 1, 1};

float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
float fract(float v) { return v - floorf(v); }
float smooth(float e0, float e1, float v) {
  const float t = clamp01((v - e0) / (e1 - e0));
  return t * t * (3 - 2 * t);
}
// 1 at x = 0, falling smoothly to 0 at |x| = 1: a soft spot of light with nothing to compute past it.
float bump(float x) {
  x *= x;
  return x < 1 ? (1 - x) * (1 - x) : 0;
}
// Fast start, slow arrival: motion that leaves with the beat and settles.
float easeOut(float t) {
  t = 1 - clamp01(t);
  return 1 - t * t;
}
// 1 at t <= 0 down to 0 at t = 1. Predicted hits can be slightly early (negative age): full strength.
float decay(float t) {
  t = 1 - clamp01(t);
  return t * t;
}
// sin(x) to within 0.1%. newlib's sinf takes ~15 us on the ESP32 for the large arguments a drift
// reaches, which made Lava cost 12 ms a frame; this wraps to one turn and evaluates a parabola.
float wave(float x) {
  float z = x * 0.15915494f;                           // turns
  z = 2 * (z - floorf(z + 0.5f));                      // -1..1, one turn
  const float y = 4 * z * (1 - fabsf(z));
  return 0.225f * (y * fabsf(y) - y) + y;
}
// Signed distance between two angles in turns, -0.5..0.5.
float turns(float d) { return d - floorf(d + 0.5f); }
float unitHash(uint32_t a, uint32_t b) {
  uint32_t h = a * 0x9E3779B1u ^ b * 0x85EBCA77u;
  h ^= h >> 15;
  h *= 0x2C1B3C6Du;
  h ^= h >> 12;
  return (h & 0xFFFF) / 65536.0f;
}

Rgbf operator+(Rgbf a, Rgbf b) { return {a.r + b.r, a.g + b.g, a.b + b.b}; }
Rgbf operator*(Rgbf c, float k) { return {c.r * k, c.g * k, c.b * k}; }
Rgbf mix(Rgbf a, Rgbf b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
Rgbf brighter(Rgbf a, Rgbf b) { return {fmaxf(a.r, b.r), fmaxf(a.g, b.g), fmaxf(a.b, b.b)}; }

// ---- clocks ----

float spbOf(const Moment& m) { return m.spb > 0.1f ? m.spb : 0.1f; }
float beatsIntoPhrase(const Moment& m) { return m.inPhrase * kBeatsPerPhrase; }

// Free-running seconds for drifts that must not jump when the grid changes, scaled by the Speed
// slider (0.25x at 0, 1x at 0.5, 4x at 1). The double is folded once; the rest stays in float.
float drift(const Moment& m, const Params& p) {
  const float s = (float)(m.seconds - 65536.0 * floor(m.seconds * (1 / 65536.0)));
  return s * (0.25f + p.speed * (0.75f + 3 * p.speed * p.speed));
}

// Where in its bar a hit happened, 0..4 beats.
float hitInBar(const Moment& m, const Hit& h) {
  const float b = m.inBar - h.age / spbOf(m);
  return b - kBeatsPerBar * floorf(b / kBeatsPerBar);
}
bool onDownbeat(const Moment& m, const Hit& h) {
  const float b = hitInBar(m, h);
  return b < 0.25f || b > kBeatsPerBar - 0.25f;
}

// ---- the flash rule: the only place whole-room flashes are timed ----

// Beats between flash slots: `beats` (1, 2 or 4), doubled until slots are at least kFlashGap apart.
float slotBeats(const Moment& m, float beats) {
  while (beats < kBeatsPerBar && beats * spbOf(m) < kFlashGap) beats *= 2;
  return beats;
}
// Seconds since the latest flash slot. Slots sit on the grid every slotBeats(beats), from the bar.
float sinceSlot(const Moment& m, float beats) { return fmodf(m.inBar, slotBeats(m, beats)) * spbOf(m); }
// Whether a hit may flash the whole room (or the stage). With a grid, only hits on a beat slot may;
// without one, only a hit with no hit of its voice in the kFlashGap before it.
bool mayFlash(const Moment& m, const Hit& h) {
  if (m.lock >= 0.5f) {
    const float every = slotBeats(m, 1);
    const float b = hitInBar(m, h);
    return fabsf(b - every * floorf(b / every + 0.5f)) * spbOf(m) < 0.06f;
  }
  for (int i = 0; i < m.hitCount; i++) {
    const Hit& o = m.hits[i];
    if (o.voice == h.voice && o.age > h.age && o.age - h.age < kFlashGap) return false;
  }
  return true;
}

// ---- shared fields ----

// A ring that left the stage `age` seconds ago and crosses the room in `travel` seconds: a crisp
// front in `front`, trailing a tail twice as wide toward the stage in `tail`.
Rgbf ring(float rn, float age, float travel, float width, Rgbf front, Rgbf tail) {
  const float t = age / travel;
  if (t >= 1) return kBlack;
  const float d = rn - 1.15f * easeOut(t);
  if (d > 0.5f * width || d < -2 * width) return kBlack;
  const float k = d > 0 ? bump(d / (0.5f * width)) : bump(d / (2 * width));
  return mix(tail, front, smooth(-2 * width, 0, d)) * (k * (1 - t * t));
}

// Rings from every kick; without kicks the grid still shows: a soft ring on each bar, on each beat
// in a build.
Rgbf kickRings(const Moment& m, const Led& led, float travel, float width, Rgbf front, Rgbf tail) {
  Rgbf c = kBlack;
  bool kicked = false;
  for (int i = 0; i < m.hitCount; i++) {
    const Hit& h = m.hits[i];
    if (h.voice != Voice::Kick) continue;
    kicked = true;
    const bool down = onDownbeat(m, h);
    c = c + ring(led.rn, h.age, travel, down ? 1.5f * width : width, front, tail) * (h.strength * (down ? 1.1f : 0.85f));
  }
  if (!kicked && m.lock > 0) {
    const bool build = m.section == Section::Build;
    const float every = build ? slotBeats(m, 1) : kBeatsPerBar;
    const float amp = build ? 0.35f + 0.55f * m.tension : 0.4f;
    c = ring(led.rn, fmodf(m.inBar, every) * spbOf(m), travel, width, front, tail) * (amp * m.lock);
  }
  return c;
}

// Without a tempo, light swells out from the stage with the bass instead.
float swell(const Moment& m, const Led& led) {
  const float reach = 0.15f + 0.6f * m.low;
  return (1 - m.lock) * (0.15f + 0.45f * m.level) * (1 - smooth(reach - 0.2f, reach + 0.1f, led.rn));
}

// The stage's answer to the kick, in the palette's first colour: kicks on a flash slot land at full;
// others only nudge it, below the size of a flash. The TV wall packs LEDs twice as densely as the
// coves, so its peak stays well under white; white is saved for Impact and the end of a Riser.
Rgbf stageKick(const Moment& m, const Palette& pal) {
  float k = 0;
  for (int i = 0; i < m.hitCount; i++) {
    const Hit& h = m.hits[i];
    if (h.voice == Voice::Kick) k = fmaxf(k, h.strength * decay(h.age / 0.3f) * (mayFlash(m, h) ? 1 : 0.2f));
  }
  return mix(pal.a, pal.accent, 0.3f * k) * (0.18f + 0.07f * m.level + 0.5f * k);
}

// A beam around the room centre at `pos` turns, with a tail three times as long behind it.
float beam(float a, float pos, float dir, float width) {
  const float d = turns(a - pos) * dir;
  return d > 0 ? bump(d / width) : bump(d / (3 * width));
}

// ---- looks ----

// Silence: a dim, slow two-colour drift on the coves only.
Rgbf idle(const Moment& m, const Led& led, const Params& p, const Palette& pal) {
  if (led.role != Role::Cove && led.role != Role::Band) return kBlack;
  const float t = drift(m, p);
  const float hue = 0.5f + 0.5f * wave(6.2832f * (led.a + t / 90));
  const float level = 0.16f + 0.05f * wave(6.2832f * (led.u * 0.5f - t / 23)) + 0.1f * m.level;
  return mix(pal.a, pal.b, hue) * level;
}

// Calm and breakdown: a slow colour field; bass swells push it outward from the stage. No flashes.
Rgbf lava(const Moment& m, const Led& led, const Params& p, const Palette& pal) {
  const float t = drift(m, p);
  const float r = led.r - 0.9f * m.low;
  const float warp = wave(0.7f * led.y + 0.11f * t);
  const float hue = 0.5f + 0.5f * wave(0.55f * led.x + 1.4f * warp + 0.07f * t);
  const float glow = 0.5f + 0.5f * wave(1.1f * r - 0.23f * t + 0.8f * warp);
  const float breathe = 1 + 0.08f * m.lock * wave(6.2832f * m.inBar / kBeatsPerBar + 1.5708f);
  const float level = (0.18f + 0.5f * smooth(0.15f, 1, glow)) * (0.75f + 0.35f * m.level) * breathe;
  return mix(pal.a, pal.b, smooth(0.1f, 0.9f, hue)) * level;
}

// Two beams circle the room on the bar clock: stepped on the beat in a groove (twice as fast in a
// drop), gliding when calm, accelerating through a build; direction flips every phrase. The cove and
// band read as one ring; the edge echoes half a beat late in lighter colours.
Rgbf orbit(const Moment& m, const Led& led, const Params& p, const Palette& pal) {
  if (led.role == Role::Stage) return stageKick(m, pal);
  const float dir = (m.phrase & 1) ? -1.0f : 1.0f;
  const float beats = beatsIntoPhrase(m) - (led.role == Role::Edge ? 0.5f : 0);
  float pos;  // turns; a whole number at every phrase start, so the direction flip never jumps
  switch (m.section) {
    case Section::Silence:
    case Section::Calm:
    case Section::Breakdown: pos = beats / 16; break;
    case Section::Build: pos = beats * beats / (kBeatsPerPhrase * 4); break;
    case Section::Drop: pos = (floorf(beats) + easeOut(fract(beats) * 1.5f)) / 4; break;
    default: pos = (floorf(beats) + easeOut(fract(beats) * 1.5f)) / 8; break;
  }
  pos *= dir;
  const float free = m.lock < 1 ? drift(m, p) * 0.1f : 0;
  const float width = led.role == Role::Edge ? 0.025f : 0.035f;
  const float one = m.lock * beam(led.a, pos, dir, width) + (1 - m.lock) * beam(led.a, free, 1, width);
  const float two = m.lock * beam(led.a, pos + 0.5f, dir, width) + (1 - m.lock) * beam(led.a, free + 0.5f, 1, width);
  const float level = (0.55f + 0.45f * m.level) * (0.75f + 0.25f * m.lock * decay(fract(m.inBar) * 2));
  const bool echo = led.role == Role::Edge;
  const float tint = echo ? 0.5f : 0;
  return (mix(pal.a, pal.accent, tint) * one + mix(pal.b, pal.accent, tint) * two) * (echo ? 0.5f * level : level) +
         pal.b * 0.04f;
}

// Each kick sends a ring from the stage across the room; downbeat rings are wider.
Rgbf shockwave(const Moment& m, const Led& led, const Params& p, const Palette& pal) {
  if (led.role == Role::Stage) return stageKick(m, pal);
  const float travel = fminf(1.15f, fmaxf(0.6f, 1.8f * spbOf(m)));
  return pal.b * (0.04f + 0.08f * m.low) + kickRings(m, led, travel, 0.07f, pal.a, pal.b) + pal.a * swell(m, led);
}

// A soft bar crosses the room along x on the backbeat (the offbeat of every beat in a drop, once a
// bar when calm), flipping direction and colour each bar.
Rgbf sweep(const Moment& m, const Led& led, const Params& p, const Palette& pal) {
  if (led.role == Role::Stage) return stageKick(m, pal);
  float every = 2, offset = 1;
  switch (m.section) {
    case Section::Silence:
    case Section::Calm:
    case Section::Breakdown: every = 4, offset = 0; break;
    case Section::Build: every = m.tension < 0.5f ? 4 : m.tension < 0.85f ? 2 : 1, offset = 0; break;
    case Section::Drop: every = 1, offset = 0.5f; break;
    default: break;
  }
  every = slotBeats(m, every);
  const float fromBar = m.inBar - offset;
  const float since = fromBar - every * floorf(fromBar / every);    // beats since this sweep began
  const bool east = ((m.bar - (fromBar - since < 0 ? 1 : 0)) & 1) == 0;
  auto bar = [&](float pos, bool toEast) {  // pos 0..1 along the way; d > 0 ahead of the bar
    const float d = toEast ? led.u + 0.15f - 1.3f * pos : 1.15f - 1.3f * pos - led.u;
    return d > 0 ? bump(d / 0.06f) : bump(d / 0.25f);
  };
  const float t = since / fminf(every, 1.25f);
  float v = m.lock * bar(easeOut(t), east) * (1 - 0.6f * t * t) * (t < 1);
  if (m.lock < 1) {
    const float f = fract(drift(m, p) / 6);
    v += (1 - m.lock) * bar(f < 0.5f ? 2 * f : 2 - 2 * f, f < 0.5f) * (0.5f + 0.5f * m.level);
  }
  const float fixture = led.role == Role::Edge ? 0.6f : 1;
  return (east ? pal.a : pal.b) * (v * fixture) + (east ? pal.b : pal.a) * 0.05f;
}

// The drum kit split across the fixtures: kicks are wide soft rings from the stage on the coves,
// snares are streaks running along the coves, hats are small sparkles on the edge runs only.
Rgbf layers(const Moment& m, const Led& led, const Params& p, const Palette& pal) {
  if (led.role == Role::Stage) return stageKick(m, pal);
  const bool cove = led.role == Role::Cove || led.role == Role::Band;
  Rgbf c = pal.a * (0.03f + 0.05f * m.level);
  if (cove) {
    const float travel = fminf(1.15f, fmaxf(0.6f, 1.6f * spbOf(m)));
    c = c + kickRings(m, led, travel, 0.15f, pal.a, pal.a) * 0.5f + pal.a * swell(m, led);
  }
  // Snares and hats draw over the kick layer instead of adding to it, so colours never mix to white.
  int snares = 0, hats = 0;
  for (int i = 0; i < m.hitCount; i++) {
    const Hit& h = m.hits[i];
    if (h.voice == Voice::Snare && cove && snares++ < 2) {
      const float t = h.age / 0.6f;
      if (t >= 1) continue;
      const float d = fabsf(turns(led.a - unitHash(h.seed, 1)));
      const float head = 0.06f + 0.2f * easeOut(t * 2);
      const float v = d < head ? smooth(0, head, d) : bump((d - head) / 0.02f);
      c = brighter(c, pal.b * (v * decay(t) * fminf(1, 1.5f * h.strength)));
    } else if (h.voice == Voice::Hat && led.role == Role::Edge && hats++ < 3) {
      // Each hat lights fresh cells and lingers into the next, so the edge as a whole stays level
      // instead of flashing at the hat rate.
      if (unitHash(h.seed, (uint32_t)(led.a * 48)) < 0.1f) c = brighter(c, pal.accent * decay(h.age / 0.32f));
    }
  }
  return c;
}

// Builds: the light pulls in toward the stage as tension rises, brightens and washes out toward
// white, and pulses faster (every bar, two beats, beat) with rings converging on the stage.
Rgbf riser(const Moment& m, const Led& led, const Params& p, const Palette& pal) {
  const float tension = m.section == Section::Build ? m.tension : 0.5f * m.inPhrase;
  const float edge = 1.05f - 0.8f * tension;
  const float inside = 1 - smooth(edge - 0.15f, edge + 0.03f, led.rn);
  const float every = tension < 0.3f ? 4 : tension < 0.6f ? 2 : 1;
  const float since = sinceSlot(m, every);
  const float interval = slotBeats(m, every) * spbOf(m);
  const float pulse = m.lock * decay(since / 0.22f);
  // The converging ring is a second event per interval: only when two still fit the flash budget.
  const float target = edge * (1 - easeOut(since / interval));
  const float converge = interval >= 2 * kFlashGap && led.role != Role::Stage && target > 0.12f
                             ? m.lock * bump((led.rn - target) / 0.07f) : 0;
  const float level = inside * (0.2f + 0.45f * tension + (0.2f + 0.35f * tension) * pulse + 0.6f * converge);
  const Rgbf col = mix(mix(pal.b, pal.a, smooth(0, edge, led.rn)), kWhite, 0.8f * tension * tension);
  return col * (level * (0.8f + 0.2f * m.level));
}

// The drop: one white flash on its first downbeat, then hot bursts filling the room from the stage
// on each kick for four bars, then fast rings for the rest of the phrase.
Rgbf impact(const Moment& m, const Led& led, const Params& p, const Palette& pal) {
  const float spb = spbOf(m);
  const float intoPhrase = beatsIntoPhrase(m);
  Rgbf c = led.role == Role::Stage ? stageKick(m, pal) : pal.a * (0.06f + 0.08f * m.low) + pal.a * swell(m, led);
  bool kicked = false;
  for (int i = 0; i < m.hitCount; i++) {
    const Hit& h = m.hits[i];
    if (h.voice != Voice::Kick) continue;
    kicked = true;
    if (led.role == Role::Stage) continue;
    const float hitBeat = intoPhrase - h.age / spb;
    const Rgbf col = ((int)floorf(hitBeat + 0.25f) & 1) ? pal.b : pal.a;
    if (hitBeat < 4 * kBeatsPerBar && mayFlash(m, h)) {
      const float front = 1.4f * easeOut(h.age / 0.12f);
      const float fill = 1 - smooth(front - 0.4f, front, led.rn);
      c = c + mix(col, pal.accent, bump(led.rn / 0.35f) * 0.7f) * (fill * decay(h.age / 0.5f) * h.strength);
    } else {
      c = c + ring(led.rn, h.age, 0.7f, 0.12f, col, pal.accent) * h.strength;
    }
  }
  if (!kicked && led.role != Role::Stage) c = c + kickRings(m, led, 0.8f, 0.12f, pal.a, pal.b);
  const bool drop = m.section == Section::Drop || m.section == Section::Groove;
  if (drop && m.lock >= 0.5f && intoPhrase < 1) c = brighter(c, kWhite * decay(intoPhrase * spb / 0.3f));
  return c;
}

const uint8_t kSilencePool[] = {0};
const uint8_t kCalmPool[] = {1, 2};
const uint8_t kGroovePool[] = {3, 4, 2, 5};
const uint8_t kBuildPool[] = {6};
const uint8_t kDropPool[] = {7};
const uint8_t kBreakdownPool[] = {1};

}  // namespace

const Look kLooks[] = {
    {"Idle", idle},   {"Lava", lava},     {"Orbit", orbit}, {"Shockwave", shockwave},
    {"Sweep", sweep}, {"Layers", layers}, {"Riser", riser}, {"Impact", impact},
};
const int kLookCount = sizeof(kLooks) / sizeof(kLooks[0]);

const Palette kPalettes[] = {
    {"Magenta Cyan", {1.00f, 0.00f, 0.55f}, {0.00f, 0.75f, 1.00f}, {0.80f, 0.70f, 1.00f}},
    {"Amber Azure", {1.00f, 0.50f, 0.00f}, {0.00f, 0.40f, 1.00f}, {1.00f, 0.85f, 0.60f}},
    {"Violet Lime", {0.55f, 0.00f, 1.00f}, {0.50f, 1.00f, 0.00f}, {0.85f, 1.00f, 0.80f}},
    {"Red Gold", {1.00f, 0.04f, 0.00f}, {1.00f, 0.65f, 0.00f}, {1.00f, 0.90f, 0.70f}},
    {"Deep Blue Ice", {0.05f, 0.10f, 1.00f}, {0.45f, 0.85f, 1.00f}, {0.85f, 0.95f, 1.00f}},
    {"Orange Teal", {1.00f, 0.30f, 0.00f}, {0.00f, 0.85f, 0.65f}, {1.00f, 0.80f, 0.55f}},
    {"Pink Purple", {1.00f, 0.15f, 0.45f}, {0.40f, 0.00f, 1.00f}, {1.00f, 0.70f, 0.90f}},
    {"Green Yellow", {0.00f, 1.00f, 0.25f}, {1.00f, 0.85f, 0.00f}, {0.90f, 1.00f, 0.70f}},
    {"Rose Mint", {1.00f, 0.25f, 0.55f}, {0.20f, 1.00f, 0.60f}, {1.00f, 1.00f, 0.90f}},
};
const int kPaletteCount = sizeof(kPalettes) / sizeof(kPalettes[0]);

const Pool& poolFor(Section s) {
  static const Pool pools[] = {
      {kSilencePool, sizeof kSilencePool}, {kCalmPool, sizeof kCalmPool}, {kGroovePool, sizeof kGroovePool},
      {kBuildPool, sizeof kBuildPool},     {kDropPool, sizeof kDropPool}, {kBreakdownPool, sizeof kBreakdownPool},
  };
  return pools[(int)s];
}

Rgbf render(const Moment& m, const Led& led, const Params& p) {
  auto pick = [](int fixed, uint8_t cued, int count) { return (fixed >= 0 ? fixed : cued) % count; };
  const Palette& pal = kPalettes[pick(p.palette, m.palette, kPaletteCount)];
  Rgbf c = kLooks[pick(p.program, m.look, kLookCount)].fn(m, led, p, pal);
  Rgbf floor = mix(pal.a, pal.b, led.u) * p.floor;
  if (m.xfade < 1) {
    // The new cue lands at full on its beat; the old one fades out underneath, as on a lighting desk.
    const Palette& from = kPalettes[pick(p.palette, m.fromPalette, kPaletteCount)];
    c = brighter(c, kLooks[pick(p.program, m.fromLook, kLookCount)].fn(m, led, p, from) * (1 - m.xfade));
    floor = mix(mix(from.a, from.b, led.u) * p.floor, floor, m.xfade);
  }
  c = brighter(c * (0.35f + 0.65f * p.intensity), floor);
  return {clamp01(c.r), clamp01(c.g), clamp01(c.b)};
}

}  // namespace orion
