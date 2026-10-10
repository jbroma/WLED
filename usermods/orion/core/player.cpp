#include "player.h"

#include <limits.h>
#include <math.h>

namespace orion {

namespace {

float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

float smooth(float edge0, float edge1, float v) {
  const float t = clamp01((v - edge0) / (edge1 - edge0));
  return t * t * (3 - 2 * t);
}

uint16_t hash3(int32_t a, int32_t b, int32_t c) {
  uint32_t h = (uint32_t)a * 0x9E3779B1u ^ (uint32_t)b * 0x85EBCA77u ^ (uint32_t)c * 0xC2B2AE3Du;
  h ^= h >> 15;
  h *= 0x2C1B3C6Du;
  h ^= h >> 12;
  return (uint16_t)h;
}

// How long each voice stays visible after it hits, in seconds.
constexpr float kHitLife[kVoices] = {1.2f, 0.8f, 0.35f};
constexpr float kSameHit = 0.07f;
constexpr uint8_t kGrooveFloor = 64;

void addHit(Moment& m, const Hit& h) {
  if (m.hitCount < kHits) {
    m.hits[m.hitCount++] = h;
    return;
  }
  int oldest = 0;
  for (int i = 1; i < kHits; i++)
    if (m.hits[i].age > m.hits[oldest].age) oldest = i;
  if (h.age < m.hits[oldest].age) m.hits[oldest] = h;
}

}  // namespace

Moment momentAt(const Score& s, Ms at) {
  Moment m = {};
  const float stale = elapsed(at, s.txMs) / 1000.0f;
  const Grid& g = s.grid;
  const double spb = g.periodUs / 1e6;
  m.seconds = at / 1000.0;
  m.spb = (float)spb;
  m.beat = g.anchorBeat + elapsed(at, g.anchorMs) / 1000.0 / spb;
  m.lock = smooth(100, 180, g.conf) * clamp01(1 - (stale - 2) / 6);

  const double fromDownbeat = m.beat - g.downbeat;
  m.bar = (int32_t)floor(fromDownbeat / kBeatsPerBar);
  m.inBar = (float)(fromDownbeat - (double)m.bar * kBeatsPerBar);
  const double fromPhrase = (m.beat - g.phraseBeat) / (kBeatsPerBar * kBarsPerPhrase);
  m.phrase = (int32_t)floor(fromPhrase);
  m.inPhrase = (float)(fromPhrase - m.phrase);

  m.section = s.section;
  m.energy = s.energy / 255.0f;
  m.tension = s.tension / 255.0f;
  // Envelopes are instantaneous: fade them out quickly when Scores stop arriving.
  const float fresh = clamp01(1 - (stale - 0.25f) / 0.75f);
  m.low = s.low / 255.0f * fresh;
  m.mid = s.mid / 255.0f * fresh;
  m.high = s.high / 255.0f * fresh;
  m.level = s.level / 255.0f * fresh;

  m.look = m.fromLook = s.cur.look;
  m.palette = m.fromPalette = s.cur.palette;
  m.xfade = 1;
  if (s.next.atBeat > s.cur.atBeat && m.beat >= s.next.atBeat) {
    m.look = s.next.look;
    m.palette = s.next.palette;
    m.xfade = clamp01((float)(m.beat - s.next.atBeat));
  }

  if (m.lock > 0.3f) {
    for (int v = 0; v < kVoices; v++) {
      for (int bar = m.bar - 1; bar <= m.bar; bar++) {
        for (int step = 0; step < kStepsPerBar; step++) {
          const uint8_t level = s.groove[v][step];
          if (level < kGrooveFloor) continue;
          const double hitBeat = g.downbeat + (double)bar * kBeatsPerBar + step / 4.0;
          const float age = (float)((m.beat - hitBeat) * spb);
          if (age < -0.05f || age > kHitLife[v]) continue;
          addHit(m, {(Voice)v, age, level / 255.0f * m.lock, hash3(bar, step, v)});
        }
      }
    }
  }
  const int predicted = m.hitCount;
  for (int i = 0; i < s.onsetCount && i < kOnsets; i++) {
    const Onset& o = s.onsets[i];
    const float age = elapsed(at, o.at) / 1000.0f;
    if (age < 0 || age > kHitLife[(int)o.voice]) continue;
    bool known = false;
    for (int h = 0; h < predicted && !known; h++)
      known = m.hits[h].voice == o.voice && fabsf(m.hits[h].age - age) < kSameHit;
    if (!known) addHit(m, {o.voice, age, o.strength / 255.0f, o.id});
  }
  for (int i = 1; i < m.hitCount; i++)
    for (int j = i; j > 0 && m.hits[j].age < m.hits[j - 1].age; j--) {
      const Hit t = m.hits[j];
      m.hits[j] = m.hits[j - 1];
      m.hits[j - 1] = t;
    }
  return m;
}

void ClockMap::observe(uint8_t e, Ms tx, Ms rx) {
  const int32_t d = elapsed(rx, tx);
  if (samples == 0 || e != epoch || abs(d - offset) > 200) {
    epoch = e;
    for (int32_t& b : best) b = d;
    offset = d;
    bucket = 0;
    bucketStart = rx;
    samples = 1;
    return;
  }
  samples++;
  if (elapsed(rx, bucketStart) >= (int32_t)kBucketMs) {
    bucket = (bucket + 1) % kBuckets;
    best[bucket] = INT32_MAX;
    bucketStart = rx;
  }
  if (d < best[bucket]) best[bucket] = d;
  int32_t target = INT32_MAX;
  for (int32_t b : best)
    if (b < target) target = b;
  // Slew 1 ms per Score so a bucket expiring never jumps the picture.
  offset += target > offset ? 1 : target < offset ? -1 : 0;
}

void Player::accept(const Score& s, Ms rx) {
  const bool newer = received == 0 || s.epoch != score.epoch || (int16_t)(s.seq - score.seq) > 0;
  if (!newer) return;
  if (received && s.epoch == score.epoch) gaps += (uint16_t)(s.seq - score.seq - 1);
  clock.observe(s.epoch, s.txMs, rx);
  score = s;
  own = false;
  lastRx = rx;
  received++;
}

void Player::acceptOwn(const Score& s) {
  score = s;
  own = true;
  lastRx = s.txMs;
  received++;
}

Moment Player::moment(Ms now, int leadMs) const {
  const Ms at = (own ? now : clock.toConductor(now)) + leadMs;
  return momentAt(score, at);
}

Player::Stats Player::stats(Ms now) const {
  return {received, gaps, own ? 0 : clock.offsetMs(), (uint32_t)elapsed(now, lastRx)};
}

}  // namespace orion
