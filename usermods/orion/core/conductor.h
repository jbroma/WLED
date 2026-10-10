// The conductor runs on the controller that owns the microphone (WLED-LVR1). It turns
// AudioReactive frames into the Score every controller renders from.
#pragma once
#include "types.h"

namespace orion {

class Conductor {
 public:
  explicit Conductor(uint8_t epoch);
  ~Conductor();
  Conductor(const Conductor&) = delete;
  Conductor& operator=(const Conductor&) = delete;

  // One FFT frame in, the updated Score out: txMs = f.at, seq incremented on every call.
  // Deterministic: the same frames give the same Scores on the device and in the simulator.
  const Score& step(const Frame& f);
  const Score& score() const;

  // Values for /orion/state and the simulator's reports.
  struct Debug {
    float onset;                                       // onset detection function, this frame
    float bpm;                                         // current tempo estimate
    float tempoStrength;                               // how clearly the tempo stands out, 0..1
    float voice[kVoices];                              // per-voice onset strength this frame, 0..1
    float phaseErrMs;                                  // last beat: observed onset minus predicted beat
    int hit;                                           // last beat found an onset near the prediction
    int downbeatPhase;                                 // beat number mod 4 that starts a bar
    float lowRel, allRel;                              // bass and overall level against the long-term, log2
    float kickActivity;                                // kick presence against its groove level, 0..1+
    float rise;                                        // mid/high level slope, log2 per bar
  };
  Debug debug() const;

 private:
  struct State;
  State* st;
};

}  // namespace orion
