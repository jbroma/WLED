#include "looks.h"

#include <math.h>

namespace orion {

namespace {

float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

Rgbf scale(Rgbf c, float k) { return {c.r * k, c.g * k, c.b * k}; }
Rgbf mix(Rgbf a, Rgbf b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }

// Placeholder until the look library lands: the palette's first colour, brighter on each beat.
Rgbf lookBeat(const Moment& m, const Led&, const Params& p, const Palette& pal) {
  const float phase = (float)(m.beat - floor(m.beat));
  const float pulse = expf(-phase * 4) * m.lock;
  return scale(pal.a, p.floor + (1 - p.floor) * pulse);
}

const uint8_t kAll[] = {0};

}  // namespace

const Look kLooks[] = {
    {"Beat", lookBeat},
};
const int kLookCount = sizeof(kLooks) / sizeof(kLooks[0]);

const Palette kPalettes[] = {
    {"Magenta Cyan", {1.0f, 0.0f, 0.6f}, {0.0f, 0.8f, 1.0f}, {1.0f, 1.0f, 1.0f}},
};
const int kPaletteCount = sizeof(kPalettes) / sizeof(kPalettes[0]);

const Pool& poolFor(Section) {
  static const Pool all = {kAll, sizeof(kAll)};
  return all;
}

Rgbf render(const Moment& m, const Led& led, const Params& p) {
  auto pick = [](int fixed, uint8_t cued, int count) { return (fixed >= 0 ? fixed : cued) % count; };
  const Look& to = kLooks[pick(p.program, m.look, kLookCount)];
  const Palette& toPal = kPalettes[pick(p.palette, m.palette, kPaletteCount)];
  Rgbf c = to.fn(m, led, p, toPal);
  if (m.xfade < 1) {
    const Look& from = kLooks[pick(p.program, m.fromLook, kLookCount)];
    const Palette& fromPal = kPalettes[pick(p.palette, m.fromPalette, kPaletteCount)];
    c = mix(from.fn(m, led, p, fromPal), c, m.xfade);
  }
  return {clamp01(c.r), clamp01(c.g), clamp01(c.b)};
}

}  // namespace orion
