#include "conductor.h"

#include <math.h>
#include <string.h>

#include "looks.h"

namespace orion {

namespace {

constexpr float kNominalFrameMs = 512000.0f / 22050.0f;

// The bands each voice listens to: 43-129 Hz kick, 1.1-3.7 kHz snare, 3.7-9 kHz hat. A voice's
// flux is the rank-th largest band flux in its range: drums are broadband within their range,
// while a bass or synth note lights only one or two bands. Bass notes and their harmonics fill
// 200-1100 Hz, so the snare listens above that.
struct VoiceBands {
  uint8_t first, last, rank;
};
constexpr VoiceBands kVoiceBands[kVoices] = {{0, 1, 2}, {8, 12, 3}, {13, 15, 2}};
constexpr float kRefractoryMs[kVoices] = {110, 80, 50};

// Tempo listens to every voice; phase leans on the kick, because off-beat bass and hats in
// house music are as frequent as the beat but weaker in the low end.
constexpr float kTempoWeight[kVoices] = {1.0f, 1.0f, 0.3f};
constexpr float kPhaseWeight[kVoices] = {1.0f, 1.0f, 0.15f};

// A frame's onset shows up this long after the sound reached the mic (measured, eval_analysis.py).
constexpr float kOnsetDelayMs = 18;

constexpr int kTempoHist = 512;
constexpr int kMaxLag = 264;                           // 8 beats at 80 BPM, in frames
constexpr int kPhaseHist = 256;                        // ~6 s for the beat phase
constexpr float kMinBpm = 80, kBpmStep = 0.5f;
constexpr int kTempoBins = 160;                        // 80..160 BPM: one octave, so no octave flips
constexpr int kTempoEvery = 16;                        // frames between tempo updates
// Lags the comb sums, in beats: the double tempo, the beat, and bar-level repeats.
constexpr float kCombBeats[] = {0.5f, 1, 2, 4, 8};

constexpr float kGrooveRise = 0.35f, kGrooveFall = 0.75f;

// Tempo may move between estimates: Gaussian over bin distance, sigma 2 bins (1 BPM).
constexpr float kTempoMove[7] = {1.0f, 0.8825f, 0.6065f, 0.3247f, 0.1353f, 0.0439f, 0.0111f};

float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// d^n for the per-frame decays, without powf: n is the frames since the last step, usually 1.
float decay(float d, int n) {
  float r = d;
  while (--n > 0) r *= d;
  return r;
}
uint8_t byte01(float v) { return (uint8_t)lroundf(clampf(v, 0, 1) * 255); }
int mod4(int32_t v) { return (int)(((v % kBeatsPerBar) + kBeatsPerBar) % kBeatsPerBar); }

uint32_t xorshift(uint32_t& s) {
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return s;
}

// Per-band levels against the band's own recent average: invariant to room level and mic tilt.
struct Bands {
  float mean[kBands] = {};                             // ~5 s average magnitude
  float slow[kBands] = {};                             // ~30 s average, for envelopes and levels
  float level[kBands] = {};
  float rise[kBands] = {};
  float log[kBands] = {};
  float flux[kBands] = {};
  bool primed = false;
  uint32_t n = 0;

  void update(const float* raw) {
    // A running mean until the window fills, then the EMA: averages primed on room noise must not
    // take 30 s to reach the music, or the first kicks read as huge and every later kick as weak.
    n++;
    const float meanRate = fmaxf(1.0f / 215, 1.0f / n), slowRate = fmaxf(1.0f / 1300, 1.0f / n);
    for (int b = 0; b < kBands; b++) {
      const float v = raw[b] > 0 ? raw[b] : 0;
      if (!primed) mean[b] = slow[b] = level[b] = v + 1e-6f;
      mean[b] += (v - mean[b]) * meanRate;
      slow[b] += (v - slow[b]) * slowRate;
      rise[b] = fmaxf(0, v - level[b]);
      level[b] = v;
      // Linear rise against the band's long-term level: level and tilt invariant, unlike a log
      // flux it keeps a kick's energy ahead of a soft note after a gap, and a quiet breakdown
      // does not turn its leftovers into onsets.
      flux[b] = rise[b] / slow[b];
      log[b] = log2f(1 + 4 * v / mean[b]);
    }
    primed = true;
  }

  // Rise in the kick bands against their long-term level. Unlike the per-band flux it does not
  // rescale when a breakdown empties the bass, so it can tell that the kick is gone.
  float kickFlux() const { return (rise[0] + rise[1]) / (slow[0] + slow[1]); }

  float rankFlux(const VoiceBands& vb) const {
    float top[kBands];
    int n = 0;
    for (int b = vb.first; b <= vb.last; b++) {
      int i = n++;
      for (; i > 0 && top[i - 1] < flux[b]; i--) top[i] = top[i - 1];
      top[i] = flux[b];
    }
    return top[vb.rank - 1];
  }
};

struct VoiceDetector {
  float ref = 0;                                       // flux peak, decays over ~3 s
  float slowRef = 0;                                   // flux peak, decays over ~30 s
  float mean = 0, var = 0;
  float strength = 0, prev = 0;
  double lastOnset = -1e9;

  bool update(float flux, int frames, double t, float refractoryMs) {
    ref = fmaxf(flux, ref * decay(0.9923f, frames));
    slowRef = fmaxf(flux, slowRef * decay(0.9992f, frames));
    prev = strength;
    // Normalising to the recent peak alone would blow a breakdown's leftovers up to full strength.
    const float norm = fmaxf(ref, 0.5f * slowRef);
    strength = norm > 0 ? fminf(1, flux / norm) : 0;
    const float thr = fmaxf(0.3f, mean + 1.5f * sqrtf(var));
    const bool onset = strength > thr && prev <= strength && t - lastOnset >= refractoryMs;
    const float d = strength - mean;
    mean += d * (1.0f / 16);
    var += (d * d - var) * (1.0f / 16);
    if (onset) lastOnset = t;
    return onset;
  }
};

// Tempo from the autocorrelation of the onset function, scored by a comb over one octave and
// smoothed by one forward step of an HMM over tempo bins.
struct Tempo {
  float x[kTempoHist] = {};
  uint32_t n = 0;
  float mean = 0;
  float acf[kMaxLag + 1] = {};
  float belief[kTempoBins];
  float bpm = 120, strength = 0;

  Tempo() {
    for (float& b : belief) b = 1.0f / kTempoBins;
  }

  void push(float onset) {
    mean += (onset - mean) * 0.05f;
    n++;
    const float v = fmaxf(0, onset - mean);
    x[n % kTempoHist] = v;
    for (int l = 0; l <= kMaxLag; l++) acf[l] = 0.995f * acf[l] + v * x[(n - l) % kTempoHist];
  }

  float at(float l) const {
    const int i = (int)l;
    if (i + 1 > kMaxLag) return 0;
    const float f = l - i;
    return acf[i] * (1 - f) + acf[i + 1] * f;
  }

  float comb(float bpm, float frameMs) const {
    const float beat = 60000.0f / (bpm * frameMs);
    float c = 0;
    for (float m : kCombBeats) c += at(m * beat);
    return c;
  }

  void estimate(float frameMs) {
    float score[kTempoBins];
    const float beta = 60000.0f / (120 * frameMs);
    for (int i = 0; i < kTempoBins; i++) {
      const float l = 60000.0f / ((kMinBpm + i * kBpmStep) * frameMs);
      const float prior = l / beta * expf(0.5f - l * l / (2 * beta * beta));
      score[i] = fmaxf(comb(kMinBpm + i * kBpmStep, frameMs), 0) * prior;
    }
    float next[kTempoBins], sum = 0;
    for (int i = 0; i < kTempoBins; i++) {
      float p = 0;
      for (int d = -6; d <= 6; d++) {
        const int j = i + d;
        if (j >= 0 && j < kTempoBins) p += belief[j] * kTempoMove[d < 0 ? -d : d];
      }
      next[i] = (p + 0.02f) * score[i];
      sum += next[i];
    }
    int best = 0;
    for (int i = 0; i < kTempoBins; i++) {
      belief[i] = sum > 0 ? next[i] / sum : 1.0f / kTempoBins;
      if (belief[i] > belief[best]) best = i;
    }
    float off = 0;
    if (best > 0 && best < kTempoBins - 1) {
      const float a = score[best - 1], b = score[best], c = score[best + 1];
      const float den = a - 2 * b + c;
      if (den < 0) off = clampf(0.5f * (a - c) / den, -0.5f, 0.5f);
    }
    bpm = kMinBpm + (best + off) * kBpmStep;
    const float count = sizeof kCombBeats / sizeof kCombBeats[0];
    strength = acf[0] > 0 ? clampf(comb(bpm, frameMs) / (count * acf[0]), 0, 1) : 0;
  }
};

}  // namespace

struct Conductor::State {
  Score score;

  // Timebase: conductor ms since the first frame, unwrapped, as double so days of uptime keep
  // sub-millisecond resolution. Frame stamps jitter by ~10 ms; the beat clock averages that out.
  bool started = false;
  Ms lastAt = 0;
  uint32_t lastId = 0;
  double now = 0;
  float frameMs = kNominalFrameMs;
  float dtAvg = kNominalFrameMs, didAvg = 1;

  Bands bands;
  VoiceDetector voices[kVoices];
  float phaseFn = 0, phaseMean = 0;
  uint16_t onsetId = 0;
  Tempo tempo;
  int sinceTempo = 0;

  float hist[kPhaseHist] = {};
  float kickHist[kPhaseHist] = {};
  Ms histMs[kPhaseHist] = {};
  uint32_t histN = 0;

  // Beat clock: acoustic times of the last and next beat, corrected by each observed beat.
  bool running = false;
  double last = 0, next = 0;
  float period = 500;
  int32_t nextBeat = 1;
  float hitRate = 0, peakAvg = 0.1f, phaseErr = 0;
  int hit = 0, disagree = 0, offPhase = 0;

  float spanCur[kBands] = {}, spanNext[kBands] = {}, spanPrev[kBands] = {};
  int spanCurN = 0, spanNextN = 0;
  float changeAvg = 1e-3f;
  float barScore[kBeatsPerBar] = {};
  float kickNear = 0, snareNear = 0;
  int downPhase = 0;
  int32_t phraseBeat = 0;

  float groove[kVoices][kStepsPerBar] = {};
  float kept[kVoices][kStepsPerBar] = {};
  float seen[2][kVoices][kStepsPerBar] = {};
  int32_t seenBar = 0;

  float lowNow = 0, lowFast = 0, lowSlow = 0, lowLong = 0;
  float allShort = 0, allLong = 0, upperSum = 0;
  int upperN = 0;
  float upper[4 * kBeatsPerBar] = {};
  float rise = 0;
  bool ramping = false;
  int snareBeat = 0, snareBeats[kBeatsPerBar] = {};
  float kickRef = 1e-6f, kickBeat = 0, kickAct = 0;
  bool relearn = false;
  uint32_t levelN = 0;
  float kickBeats[kBeatsPerBar] = {};
  bool levelsPrimed = false;

  Section section = Section::Silence;
  float silentMs = 0, musicMs = 0;
  int32_t sectionBeat = 0;
  int32_t enteredBeat = 0;
  int beatOn = 0, beatOff = 0, quietBeats = 0;
  bool beatPresent = false;
  int32_t lastDropBeat = -1000;
  float dropLevel = 0;

  // Envelopes (low, mid, high, level): adaptive floor and peak in log2 units, then a fast-attack
  // release so a hit lands at once and fades over ~150 ms.
  float envPeak[4] = {}, envFloor[4] = {}, env[4] = {};
  bool envPrimed = false;

  uint32_t rng;
  uint8_t recent[2] = {255, 255};
  bool cueSection = false, cueDrop = false;

  explicit State(uint8_t epoch) : rng(0x9E3779B9u ^ (epoch * 0x01000193u)) {
    if (!rng) rng = 1;
  }

  double beatAt(double t) const { return nextBeat + (t - next) / period; }
  // Phase history works in float ms before the current frame: exact over its 6 s, and no
  // soft-float doubles in the per-beat searches on the ESP32.
  float ageOf(uint32_t i) const { return (float)elapsed(lastAt, histMs[i % kPhaseHist]); }

  void pushPhase(float v, float kick, Ms at) {
    histN++;
    hist[histN % kPhaseHist] = v;
    kickHist[histN % kPhaseHist] = kick;
    histMs[histN % kPhaseHist] = at;
  }

  // Phase onset function `age` ms before this frame: the larger of the two samples around it,
  // because interpolating would halve a one-frame kick peak while a smeared off-beat survives.
  float histValue(const float* h, float age) const {
    if (histN < 2) return 0;
    const uint32_t oldest = histN > kPhaseHist - 2 ? histN - (kPhaseHist - 2) : 1;
    const float back = age / frameMs;
    if (back < 0) return h[histN % kPhaseHist];
    if (back > histN - oldest) return 0;
    uint32_t i = histN - (uint32_t)back;
    while (i > oldest && ageOf(i) < age) i--;
    while (i < histN && ageOf(i + 1) >= age) i++;
    if (ageOf(i) < age) return 0;
    if (i == histN) return h[i % kPhaseHist];
    return fmaxf(h[i % kPhaseHist], h[(i + 1) % kPhaseHist]);
  }

  float comb(const float* h, float ago) const {
    float s = 0, w = 1;
    for (int j = 0; j < 6; j++, w *= 0.7f) s += w * histValue(h, ago + j * period - kOnsetDelayMs);
    return s;
  }

  float bestPhase(const float* h, float from, float to, float& score) const {
    float best = to;
    score = -1;
    for (uint32_t k = 0; k < kPhaseHist - 1 && k < histN; k++) {
      const float ago = ageOf(histN - k) + kOnsetDelayMs;
      if (ago < to) continue;
      if (ago > from) break;
      const float s = comb(h, ago);
      if (s > score) {
        score = s;
        best = ago;
      }
    }
    return best;
  }

  void setPhase(double t) {
    const int32_t number = running ? (int32_t)lround(beatAt(t)) : nextBeat;
    last = t;
    next = t + period;
    nextBeat = number + 1;
    running = true;
  }

  void step(const Frame& f);
  void intake(const Frame& f, int& frames);
  void detectOnsets(int frames);
  void observeOnset(Voice v, float strength, double t);
  void updateTempo();
  void trackBeats();
  void evaluateBeat();
  void closeBeatSpan(int32_t number);
  void shiftDownbeat(int phase);
  void commitBars();
  void updateLevels(const Frame& f);
  void updateEnvelopes(const Frame& f, int frames, bool silent);
  void onBeat(int32_t number);
  void detectDrop(double t);
  void enter(Section s, int32_t beat);
  uint8_t pickLook(Section s);
  void autopilot();
  void writeScore(const Frame& f);
};

void Conductor::State::intake(const Frame& f, int& frames) {
  frames = 1;
  if (started) {
    const int32_t dt = elapsed(f.at, lastAt);
    const uint32_t did = f.id - lastId;
    if (did >= 1 && did <= 16) frames = (int)did;
    else frames = (int)clampf(roundf(dt / frameMs), 1, 16);
    // Mean frame period as a ratio of averages: a frame noticed late after a main-loop stall is
    // followed by one noticed early, and only the sums cancel that out.
    if (did >= 1 && did <= 16 && dt > 0 && dt < 1000) {
      dtAvg += (dt - dtAvg) * (1.0f / 500);
      didAvg += (did - didAvg) * (1.0f / 500);
      frameMs = clampf(dtAvg / didAvg, 15, 35);
    }
    now += dt;
  }
  started = true;
  lastAt = f.at;
  lastId = f.id;
}

void Conductor::State::detectOnsets(int frames) {
  const float toNext = (float)(now - next) - kOnsetDelayMs;
  float tempoFn = 0;
  phaseFn = 0;
  for (int v = 0; v < kVoices; v++) {
    const float flux = bands.rankFlux(kVoiceBands[v]);
    if (v == (int)Voice::Kick) {
      const float rise = bands.kickFlux();
      kickRef = fmaxf(rise, kickRef * decay(0.9998f, frames));
      kickBeat = fmaxf(kickBeat, rise);
    }
    if (voices[v].update(flux, frames, now, kRefractoryMs[v])) {
      const double t = now - kOnsetDelayMs;
      observeOnset((Voice)v, voices[v].strength, t);
      if (v == (int)Voice::Kick) detectDrop(t);
    }
    if (fabsf(toNext) < period / 8) {
      if (v == (int)Voice::Kick) kickNear = fmaxf(kickNear, voices[v].strength);
      if (v == (int)Voice::Snare) snareNear = fmaxf(snareNear, voices[v].strength);
    }
    tempoFn += kTempoWeight[v] * voices[v].strength;
    phaseFn += kPhaseWeight[v] * voices[v].strength;
  }
  // Missed frames enter the histories as silence, so lags stay in frames.
  for (int k = 1; k < frames; k++) {
    tempo.push(0);
    pushPhase(0, 0, lastAt - (Ms)lroundf((frames - k) * frameMs));
  }
  tempo.push(tempoFn);
  phaseMean += (phaseFn - phaseMean) * 0.05f;
  pushPhase(fmaxf(0, phaseFn - phaseMean), voices[(int)Voice::Kick].strength, lastAt);
}

void Conductor::State::observeOnset(Voice v, float strength, double t) {
  Score& s = score;
  const int n = s.onsetCount < kOnsets ? s.onsetCount : kOnsets - 1;
  memmove(&s.onsets[1], &s.onsets[0], n * sizeof(Onset));
  s.onsets[0] = {onsetId++, v, byte01(strength), lastAt - (Ms)lround(now - t)};
  s.onsetCount = n + 1;

  if (v == Voice::Snare) snareBeat++;
  if (!running) return;
  const double steps = (beatAt(t) - downPhase) * 4;
  const int32_t stepNo = (int32_t)floor(steps + 0.5);
  const int32_t bar = (int32_t)floor(stepNo / (double)kStepsPerBar);
  const int bank = bar - seenBar;
  if (bank < 0 || bank > 1) return;
  float& cell = seen[bank][(int)v][stepNo - bar * kStepsPerBar];
  cell = fmaxf(cell, strength);
}

// Folds each finished bar into the groove: steps that keep hitting rise slowly, missed steps
// fade fast, because followers replay the groove blind and a false hit is worse than a late one.
void Conductor::State::commitBars() {
  const double steps = (beatAt(now - kOnsetDelayMs) - downPhase) * 4 - 2;
  const int32_t bar = (int32_t)floor(steps / kStepsPerBar);
  if (bar - seenBar > 4) seenBar = bar - 1;
  while (seenBar < bar) {
    const bool locked = score.grid.conf >= 128;
    for (int v = 0; v < kVoices; v++)
      for (int k = 0; k < kStepsPerBar; k++) {
        float& g = groove[v][k];
        const float o = locked ? seen[0][v][k] : 0;
        g += (o - g) * (o > g ? kGrooveRise : kGrooveFall);
      }
    memcpy(seen[0], seen[1], sizeof seen[0]);
    memset(seen[1], 0, sizeof seen[1]);
    seenBar++;
  }
}

void Conductor::State::updateTempo() {
  // Wait for ~4.5 s of music: a first guess from two seconds is often a third off.
  if (++sinceTempo < kTempoEvery || tempo.n < 12 * kTempoEvery) return;
  sinceTempo = 0;
  tempo.estimate(frameMs);
  if (tempo.strength < 0.1f) return;
  const float est = 60000.0f / tempo.bpm;
  float score;
  if (!running) {
    period = est;
    setPhase(now - bestPhase(hist, kOnsetDelayMs + period, kOnsetDelayMs, score));
    return;
  }
  // The beat clock's period wanders with every noisy phase measurement; the autocorrelation
  // integrates seconds of music, so it holds the period near the estimate.
  if (fabsf(period / est - 1) < 0.04f) period += (est - period) * 0.2f;
  disagree = fabsf(period / est - 1) > 0.04f ? disagree + 1 : 0;
  if (disagree >= 3) {
    disagree = 0;
    period = est;
    setPhase(now - bestPhase(hist, kOnsetDelayMs + period, kOnsetDelayMs, score));
  }
}

void Conductor::State::closeBeatSpan(int32_t number) {
  if (spanCurN > 0) {
    float change = 0;
    for (int b = 0; b < kBands; b++) {
      const float m = spanCur[b] / spanCurN;
      change += fabsf(m - spanPrev[b]);
      spanPrev[b] = m;
    }
    changeAvg += (change - changeAvg) * 0.1f;
    barScore[mod4(number)] += 0.5f * change / (changeAvg + 1e-6f);
  }
  memcpy(spanCur, spanNext, sizeof spanCur);
  spanCurN = spanNextN;
  memset(spanNext, 0, sizeof spanNext);
  spanNextN = 0;
}

void Conductor::State::shiftDownbeat(int phase) {
  int shift = phase - downPhase;
  if (shift == 0) return;
  auto rotate = [shift](float(*g)[kStepsPerBar]) {
    for (int v = 0; v < kVoices; v++) {
      float rot[kStepsPerBar];
      for (int k = 0; k < kStepsPerBar; k++) rot[k] = g[v][(k + shift * 4 + 4 * kStepsPerBar) % kStepsPerBar];
      memcpy(g[v], rot, sizeof rot);
    }
  };
  rotate(groove);
  rotate(kept);
  if (shift > 2) shift -= kBeatsPerBar;
  if (shift < -1) shift += kBeatsPerBar;
  phraseBeat += shift;
  downPhase = phase;
}

void Conductor::State::evaluateBeat() {
  const float nextAgo = (float)(now - next);
  auto offset = [&](uint32_t i) { return nextAgo - ageOf(i) - kOnsetDelayMs; };
  const float reach = 0.3f * period;
  float bestV = 0, bestW = 0, rival = 0;
  uint32_t best = 0;
  for (uint32_t k = 0; k < kPhaseHist - 2 && k + 1 < histN; k++) {
    const uint32_t i = histN - k;
    const float off = offset(i);
    if (off > reach) continue;
    if (off < -reach) break;
    const float d = off / (0.25f * period);
    const float w = hist[i % kPhaseHist] * expf(-0.5f * d * d);
    if (w > bestW) {
      bestW = w;
      bestV = hist[i % kPhaseHist];
      best = i;
    }
  }
  // A second peak nearly as strong (a roll, a busy off-beat) makes the evidence ambiguous.
  for (uint32_t k = 0; best && k < kPhaseHist - 2 && k + 1 < histN; k++) {
    const uint32_t i = histN - k;
    const float off = offset(i);
    if (off > reach) continue;
    if (off < -reach) break;
    if (i + 1 < best || i > best + 1) rival = fmaxf(rival, hist[i % kPhaseHist]);
  }
  // A clear onset near the prediction confirms the beat; only an unambiguous one moves the clock.
  hit = 0;
  bool clear = false;
  if (best && bestV > fmaxf(0.03f, 0.3f * peakAvg)) {
    float off = offset(best);
    if (best < histN && best + kPhaseHist > histN + 2) {
      const float a = hist[(best - 1) % kPhaseHist], b = hist[best % kPhaseHist], c = hist[(best + 1) % kPhaseHist];
      const float den = a - 2 * b + c;
      if (den < 0) off += clampf(0.5f * (a - c) / den, -0.5f, 0.5f) * frameMs;
    }
    clear = rival < 0.7f * bestV;
    hit = clear || fabsf(off) < 0.12f * period;
    if (clear) {
      phaseErr = off;
      peakAvg += (bestV - peakAvg) * 0.1f;
    }
  }
  const float r = clear ? phaseErr : 0;
  const int32_t number = nextBeat;
  // Bar position evidence: a kick says "one" (or three), a snare says two or four, and the
  // spectral change (closeBeatSpan) says where chords and patterns turn over.
  for (float& sc : barScore) sc *= 0.96f;
  barScore[mod4(number)] += kickNear - snareNear;
  barScore[mod4(number + 2)] += 0.5f * kickNear - snareNear;
  barScore[mod4(number + 1)] += snareNear;
  barScore[mod4(number + 3)] += snareNear;
  kickNear = snareNear = 0;
  last = next + 0.3f * r;
  period = clampf(period + 0.05f * r, 60000.0f / 165, 60000.0f / 75);
  next = last + period;
  nextBeat++;
  hitRate += (hit - hitRate) * 0.15f;

  // The local search only follows the phase it has; check it against the best phase in the whole
  // beat, which catches a lock onto off-beats taken before the kick or snare came in. A kick that
  // clearly prefers one phase (four on the floor) decides; otherwise all voices do.
  const float lastAgo = (float)(now - last);
  float here = comb(kickHist, lastAgo), there;
  float alt = bestPhase(kickHist, lastAgo + 0.7f * period, lastAgo - 0.3f * period, there);
  if (!(there > 1.5f * here && fabsf(alt - lastAgo) > 0.15f * period)) {
    here = comb(hist, lastAgo);
    alt = bestPhase(hist, lastAgo + 0.7f * period, lastAgo - 0.3f * period, there);
  }
  offPhase = there > 1.2f * here && fabsf(alt - lastAgo) > 0.15f * period ? offPhase + 1 : 0;
  if (offPhase >= 4) {
    offPhase = 0;
    setPhase(now - alt);
  }
  onBeat(number);
}

void Conductor::State::trackBeats() {
  const bool after = now - kOnsetDelayMs >= next;
  float* span = after ? spanNext : spanCur;
  for (int b = 0; b < kBands; b++) span[b] += bands.log[b];
  (after ? spanNextN : spanCurN)++;

  while (now - kOnsetDelayMs >= next + 0.3 * period) {
    closeBeatSpan(nextBeat - 1);
    evaluateBeat();
  }
  int bestPhase = downPhase;
  for (int p = 0; p < kBeatsPerBar; p++)
    if (barScore[p] > barScore[bestPhase] + 1.0f) bestPhase = p;
  if (bestPhase != downPhase) shiftDownbeat(bestPhase);
  commitBars();
}

void Conductor::State::updateLevels(const Frame& f) {
  float low = 0, all = 0, up = 0;
  for (int b = 0; b < kBands; b++) {
    const float v = f.raw[b] > 0 ? f.raw[b] : 0;
    all += v;
    if (b < 2) low += v;
    if (b >= 4) up += v / bands.slow[b];
  }
  const float l = log2f(low + 1e-6f), a = log2f(all + 1e-6f);
  if (!levelsPrimed) {
    lowFast = lowSlow = lowLong = l;
    allShort = allLong = a;
    levelsPrimed = true;
    levelN = 0;
  }
  levelN++;
  lowNow = l;
  lowFast += (l - lowFast) * 0.3f;
  lowSlow += (l - lowSlow) * 0.02f;
  // The bass level of grooves: held through breakdowns, so a drop can be measured against it.
  if (beatPresent) lowLong += (l - lowLong) * (1.0f / 1300);
  allShort += (a - allShort) * 0.01f;
  allLong += (a - allLong) * fmaxf(1.0f / 2600, 1.0f / levelN);
  upperSum += log2f(up / 12 + 1e-6f);
  upperN++;
}

void Conductor::State::updateEnvelopes(const Frame& f, int frames, bool silent) {
  constexpr uint8_t kFirst[4] = {0, 3, 13, 0}, kLast[4] = {2, 12, 15, 15};
  const float dt = frames * frameMs / 1000;
  for (int g = 0; g < 4; g++) {
    float out = 0;
    if (!silent && bands.primed) {
      float sum = 0;
      for (int b = kFirst[g]; b <= kLast[g]; b++) sum += (f.raw[b] > 0 ? f.raw[b] : 0) / bands.slow[b];
      const float y = log2f(sum / (kLast[g] - kFirst[g] + 1) + 1e-3f);
      if (!envPrimed) envPeak[g] = envFloor[g] = y;
      envPeak[g] = fmaxf(y, envPeak[g] - 0.5f * dt);
      envFloor[g] = fminf(y, envFloor[g] + 0.5f * dt);
      const float span = fmaxf(envPeak[g] - envFloor[g], 1.5f);
      out = clampf((y - (envPeak[g] - span)) / span, 0, 1);
    }
    env[g] = fmaxf(out, env[g] - dt / 0.15f);
  }
  if (!silent && bands.primed) envPrimed = true;
}

void Conductor::State::onBeat(int32_t number) {
  // Kick presence: the strongest kick in the last bar against the long-term kick peak. The
  // adaptive onset strength rescales to whatever a breakdown leaves, so it cannot tell.
  for (int i = kBeatsPerBar - 1; i > 0; i--) kickBeats[i] = kickBeats[i - 1];
  kickBeats[0] = kickBeat;
  kickBeat = 0;
  float strongest = 0;
  for (float k : kickBeats) strongest = fmaxf(strongest, k);
  kickAct = strongest / kickRef;

  // A build ramps: the upper bands climb bar after bar (a step, like a pad coming in, does not),
  // or a snare roll runs at two or more hits per beat.
  for (int i = 4 * kBeatsPerBar - 1; i > 0; i--) upper[i] = upper[i - 1];
  // Per-beat means, not a smoothed level: smoothing would turn a step (a pad coming in) into a ramp.
  upper[0] = upperN ? upperSum / upperN : upper[1];
  upperSum = 0;
  upperN = 0;
  float bars[4] = {};
  for (int i = 0; i < 4 * kBeatsPerBar; i++) bars[i / kBeatsPerBar] += upper[i] / kBeatsPerBar;
  rise = bars[0] - bars[1];
  ramping = bars[0] - bars[1] > 0.05f && bars[1] - bars[2] > 0.05f && bars[0] - bars[3] > 0.4f;
  for (int i = kBeatsPerBar - 1; i > 0; i--) snareBeats[i] = snareBeats[i - 1];
  snareBeats[0] = snareBeat;
  snareBeat = 0;
  int snares = 0;
  for (int n : snareBeats) snares += n;
  const bool rolling = snares >= 6;

  if (kickAct > 0.35f) beatOn++, beatOff = 0;
  else beatOff++, beatOn = 0;
  if (!beatPresent && beatOn >= 4) beatPresent = true;
  if (beatPresent && beatOff >= 4) beatPresent = false;
  quietBeats = ramping || rolling ? 0 : quietBeats + 1;

  // Builds are noticed a few bars in; date them from where the climb started.
  const int32_t buildStart = number - (ramping ? 3 * kBeatsPerBar : kBeatsPerBar);
  const int32_t inSection = number - sectionBeat;
  // The ramp window must lie inside the section, or the step into it reads as a climb.
  ramping = ramping && inSection >= 3 * kBeatsPerBar;
  switch (section) {
    case Section::Silence: break;
    case Section::Calm:
    case Section::Breakdown:
      if (beatPresent) enter(Section::Groove, number);
      else if ((ramping || rolling) && inSection >= 2 * kBeatsPerBar) enter(Section::Build, buildStart);
      break;
    case Section::Groove:
      // With the kick still going, only a roll that also climbs is a build.
      if (!beatPresent) enter(Section::Breakdown, number);
      else if (ramping && rolling) enter(Section::Build, buildStart);
      break;
    case Section::Drop:
      if (!beatPresent) enter(Section::Breakdown, number);
      else if (allShort < dropLevel - 0.8f || inSection >= 64) enter(Section::Groove, number);
      break;
    case Section::Build:
      if (beatPresent && !ramping) enter(Section::Groove, number);
      else if (quietBeats >= 8) enter(Section::Breakdown, number);
      break;
  }
}

void Conductor::State::detectDrop(double t) {
  if (section != Section::Build && section != Section::Breakdown) return;
  if (!running || nextBeat - enteredBeat < 8 || nextBeat - lastDropBeat < 64) return;
  // A full kick (against the long-term kick peak) bringing the bass back at least 3x: snare-roll
  // leakage into the kick bands measured up to 0.6 of the peak and 2x the bass level.
  if (bands.kickFlux() < 0.75f * kickRef || lowNow - lowSlow < 1.5f || lowNow < lowLong - 1.0f) return;
  // The drop lands on a downbeat and starts a phrase: re-anchor the clock, bar and phrase on it.
  const int32_t beat = (int32_t)lround(beatAt(t));
  barScore[mod4(beat)] += 3;
  int best = mod4(beat);
  for (int p = 0; p < kBeatsPerBar; p++)
    if (barScore[p] > barScore[best]) best = p;
  shiftDownbeat(best);
  last = t;
  next = t + period;
  nextBeat = beat + 1;
  int delta = best - mod4(beat);
  if (delta > 1) delta -= kBeatsPerBar;
  if (delta < -2) delta += kBeatsPerBar;
  phraseBeat = beat + delta;
  lastDropBeat = beat;
  dropLevel = allShort;
  beatPresent = true;
  beatOff = 0;
  enter(Section::Drop, beat);
  cueDrop = true;
}

void Conductor::State::enter(Section s, int32_t beat) {
  if (s == section) return;
  // The kick leaves at once in a breakdown, and the drop usually brings the old groove back.
  const bool wasGrooving = section == Section::Groove || section == Section::Drop;
  if (s == Section::Breakdown || s == Section::Build) {
    if (wasGrooving) memcpy(kept, groove, sizeof kept);
    memset(groove[(int)Voice::Kick], 0, sizeof groove[0]);
  }
  if (s == Section::Drop) memcpy(groove, kept, sizeof kept);
  if (s == Section::Silence) memset(groove, 0, sizeof groove);
  section = s;
  sectionBeat = beat;
  enteredBeat = nextBeat;
  cueSection = true;
}

uint8_t Conductor::State::pickLook(Section s) {
  const Pool& pool = poolFor(s);
  if (pool.count == 0) return 0;
  uint8_t look = pool.looks[xorshift(rng) % pool.count];
  for (int tries = 0; tries < 4 && pool.count > 2 && (look == recent[0] || look == recent[1]); tries++)
    look = pool.looks[xorshift(rng) % pool.count];
  recent[1] = recent[0];
  recent[0] = look;
  return look;
}

// Look and palette changes, dated on the beat grid so every controller switches together.
void Conductor::State::autopilot() {
  Score& s = score;
  const Grid& g = s.grid;
  const double beatNow = g.anchorBeat + elapsed(lastAt, g.anchorMs) * 1000.0 / g.periodUs;
  if (s.next.atBeat > s.cur.atBeat && beatNow >= s.next.atBeat + 1) s.cur = s.next;
  const bool pending = s.next.atBeat > s.cur.atBeat;
  // Earliest bar start at least 1.5 beats ahead, so followers hear about it in time.
  const int32_t soon = (int32_t)ceil(beatNow + 1.5);
  const int32_t bar = soon + mod4(g.downbeat - soon);
  int32_t at = 0;
  if ((cueDrop || cueSection) && (!pending || s.next.atBeat >= soon)) {
    at = bar;
  } else if (!pending) {
    const int32_t phraseLen = kBeatsPerBar * kBarsPerPhrase;
    const int32_t phrase = soon + (int32_t)(((g.phraseBeat - soon) % phraseLen + phraseLen) % phraseLen);
    if (phrase - beatNow <= 4) at = phrase;
  }
  if (!at) return;
  cueDrop = cueSection = false;
  uint8_t palette = s.cur.palette;
  if (kPaletteCount > 1)
    while (palette == s.cur.palette) palette = (uint8_t)(xorshift(rng) % kPaletteCount);
  s.next.look = pickLook(section);
  s.next.palette = palette;
  s.next.atBeat = at;
}

void Conductor::State::step(const Frame& f) {
  int frames = 1;
  intake(f, frames);
  float sum = 0;
  for (float r : f.raw) sum += r > 0 ? r : 0;
  // The AR noise gate sends exact zeros; quieter than 1/64 of the long-term level is silence too.
  const bool silent = sum <= 0 || (levelsPrimed && log2f(sum) < allLong - 6);
  silentMs = silent ? silentMs + frames * frameMs : 0;
  musicMs = silent ? 0 : musicMs + frames * frameMs;

  // Music after 3 s of silence is a new start: re-learn every level reference from it, so room noise
  // heard before it (or a different track's loudness) does not skew kick presence and levels.
  if (silentMs >= 3000) relearn = true;
  if (!silent && relearn) {
    bands = Bands();
    levelsPrimed = false;
    kickRef = 1e-6f;
    relearn = false;
  }
  // Silence leaves the tempo autocorrelation as it was, so a track that pauses (or a breakdown
  // quiet enough to close the AR noise gate) picks its tempo up again at once.
  if (silent) {
    for (VoiceDetector& v : voices) v.strength = 0;
    for (int k = 0; k < frames; k++) pushPhase(0, 0, lastAt - (Ms)lroundf((frames - 1 - k) * frameMs));
  } else {
    bands.update(f.raw);
    updateLevels(f);
    detectOnsets(frames);
  }
  updateEnvelopes(f, frames, silent);
  updateTempo();
  if (running) trackBeats();

  if (silentMs >= 1000) {
    enter(Section::Silence, nextBeat);
    hitRate *= 0.97f;
  } else if (section == Section::Silence && musicMs >= 300) {
    enter(Section::Calm, nextBeat);
  }
  writeScore(f);
  autopilot();
}

void Conductor::State::writeScore(const Frame& f) {
  Score& s = score;
  s.seq++;
  s.txMs = f.at;
  Grid& g = s.grid;
  if (running) {
    g.anchorMs = f.at + (Ms)(int32_t)llround(last - now);
    g.anchorBeat = nextBeat - 1;
    g.periodUs = (uint32_t)lroundf(period * 1000);
  } else {
    g.anchorMs = f.at;
    g.anchorBeat = 0;
    g.periodUs = 500000;
  }
  g.conf = byte01(clampf((hitRate - 0.3f) / 0.5f, 0, 1) * clampf(tempo.strength / 0.25f, 0, 1));
  g.downbeat = downPhase;
  g.phraseBeat = phraseBeat;
  s.section = section;
  s.tension = section == Section::Build ? byte01((nextBeat - sectionBeat) / 32.0f) : 0;
  s.energy = section == Section::Silence ? 0 : byte01(0.5f + 0.4f * (allShort - allLong));
  s.low = byte01(env[0]);
  s.mid = byte01(env[1]);
  s.high = byte01(env[2]);
  s.level = byte01(env[3]);
  for (int v = 0; v < kVoices; v++)
    for (int k = 0; k < kStepsPerBar; k++) s.groove[v][k] = byte01(groove[v][k]);
}

Conductor::Conductor(uint8_t epoch) : st(new State(epoch)) { st->score.epoch = epoch; }
Conductor::~Conductor() { delete st; }

const Score& Conductor::step(const Frame& f) {
  st->step(f);
  return st->score;
}

const Score& Conductor::score() const { return st->score; }

Conductor::Debug Conductor::debug() const {
  const State& s = *st;
  return {s.phaseFn,
          s.tempo.bpm,
          s.tempo.strength,
          {s.voices[0].strength, s.voices[1].strength, s.voices[2].strength},
          s.phaseErr,
          s.hit,
          s.downPhase,
          s.lowFast - s.lowLong,
          s.allShort - s.allLong,
          s.kickAct,
          s.rise};
}

}  // namespace orion
