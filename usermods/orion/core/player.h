#pragma once
#include "types.h"

namespace orion {

// The Moment at conductor time `at`, given the latest Score. Pure: every controller that holds the
// same Score derives the same Moment for the same conductor instant.
Moment momentAt(const Score& s, Ms at);

// Maps this controller's millis() to the conductor's. Each Score carries the conductor's send time;
// the smallest (receive - send) seen over the last 4 s is the truest offset, because network queueing
// only ever adds delay.
class ClockMap {
 public:
  void observe(uint8_t epoch, Ms txConductor, Ms rxLocal);
  Ms toConductor(Ms local) const { return local - (Ms)offset; }
  int32_t offsetMs() const { return offset; }
  bool valid() const { return samples >= 3; }

 private:
  static constexpr int kBuckets = 8;
  static constexpr Ms kBucketMs = 500;
  int32_t best[kBuckets] = {};
  int bucket = 0;
  Ms bucketStart = 0;
  int32_t offset = 0;
  uint32_t samples = 0;
  uint8_t epoch = 0;
};

class Player {
 public:
  // A Score from the network. Newer sequence numbers within an epoch win; a new epoch resets.
  void accept(const Score& s, Ms rxLocal);
  // The conductor's own Score: same clock, no offset.
  void acceptOwn(const Score& s);
  bool hasScore() const { return received > 0; }
  // leadMs draws the music slightly ahead to cancel the output path's latency.
  Moment moment(Ms nowLocal, int leadMs) const;

  struct Stats {
    uint32_t received, gaps;
    int32_t offsetMs;
    uint32_t ageMs;
  };
  Stats stats(Ms nowLocal) const;

 private:
  Score score;
  ClockMap clock;
  bool own = false;
  Ms lastRx = 0;
  uint32_t received = 0, gaps = 0;
};

}  // namespace orion
