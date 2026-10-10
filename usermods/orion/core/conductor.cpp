#include "conductor.h"

#include <math.h>

namespace orion {

// Placeholder until the analyzer lands: envelopes only, no tempo.
struct Conductor::State {
  Score score;
  float low = 0;
};

Conductor::Conductor(uint8_t epoch) : st(new State) { st->score.epoch = epoch; }
Conductor::~Conductor() { delete st; }

const Score& Conductor::step(const Frame& f) {
  Score& s = st->score;
  s.seq++;
  s.txMs = f.at;
  const float low = log1pf(f.raw[0] + f.raw[1] + f.raw[2]);
  st->low = fmaxf(low, st->low * 0.9f);
  s.low = (uint8_t)fminf(255, st->low * 20);
  s.section = f.volume > 1 ? Section::Groove : Section::Silence;
  return s;
}

const Score& Conductor::score() const { return st->score; }

Conductor::Debug Conductor::debug() const { return {0, 0, 0}; }

}  // namespace orion
