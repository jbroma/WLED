// No Arduino or WLED headers, so the same code builds natively for the simulator.
//
// Data flow:
//   conductor (LVR1):  Frame --Conductor::step--> Score --wire--> followers
//   every controller:  Score + local clock --Player--> Moment --look()--> Rgbf per Led
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace orion {

using Ms = uint32_t;                                   // a millis() reading; wraps after 49 days
inline int32_t elapsed(Ms later, Ms earlier) { return (int32_t)(later - earlier); }

constexpr int kBands = 16;                             // AudioReactive GEQ channels
constexpr int kBeatsPerBar = 4;
constexpr int kStepsPerBar = 16;                       // groove resolution: sixteenth notes
constexpr int kBarsPerPhrase = 8;

// One AudioReactive FFT cycle on the conductor (~23 ms apart).
struct Frame {
  Ms at;                                               // millis() when the FFT cycle finished
  uint32_t id;                                         // AR fftFrames; consecutive unless a cycle was missed
  float raw[kBands];                                   // linear channel magnitudes before AR gain/smoothing
  float volume;                                        // AR volumeSmth, 0..255
};

enum class Voice : uint8_t { Kick, Snare, Hat };
constexpr int kVoices = 3;

enum class Section : uint8_t {
  Silence,                                             // no music: lights relax
  Calm,                                                // music, low energy (intro, verse, chill track)
  Groove,                                              // steady beat at normal energy
  Build,                                               // tension rising toward a drop
  Drop,                                                // bass returns after a build or breakdown, peak energy
  Breakdown,                                           // beat gone, music continues
};

// The beat line, in conductor milliseconds. Beat n happens at anchorMs + (n - anchorBeat) * periodUs / 1000.
struct Grid {
  Ms anchorMs = 0;
  int32_t anchorBeat = 0;
  uint32_t periodUs = 500000;                          // 120 BPM until a tempo is found
  uint8_t conf = 0;                                    // tempo confidence; >= 128 means locked
  int32_t downbeat = 0;                                // any beat number that starts a bar
  int32_t phraseBeat = 0;                              // any beat number that starts a phrase
};

// A look and palette that take over at a beat.
struct Cue {
  uint8_t look = 0;                                    // index into kLooks
  uint8_t palette = 0;                                 // index into kPalettes
  int32_t atBeat = 0;
};

// A detected onset, in conductor time.
struct Onset {
  uint16_t id;                                         // increments per onset; seeds per-hit random choices
  Voice voice;
  uint8_t strength;                                    // 0..255
  Ms at;
};
constexpr int kOnsets = 8;

// The conductor sends a full Score on every analysis frame, so any received Score replaces the
// previous one.
struct Score {
  uint8_t epoch = 0;                                   // random per conductor boot
  uint16_t seq = 0;
  Ms txMs = 0;                                         // conductor millis() when this Score was made
  Grid grid;
  Section section = Section::Silence;
  uint8_t energy = 0;                                  // long-term loudness 0..255
  uint8_t tension = 0;                                 // build progress 0..255, 0 outside a build
  uint8_t low = 0, mid = 0, high = 0, level = 0;       // envelopes at txMs, 0..255, adaptively normalised
  uint8_t groove[kVoices][kStepsPerBar] = {};          // expected hit strength per sixteenth, 0 = no hit
  Cue cur, next;                                       // next.atBeat > cur.atBeat when a change is pending
  Onset onsets[kOnsets] = {};                          // newest first
  uint8_t onsetCount = 0;
};

// One hit a look can draw: predicted from the groove or detected. Age is seconds since the hit and
// can be slightly negative when a predicted hit is drawn early to cancel output latency.
struct Hit {
  Voice voice;
  float age;
  float strength;                                      // 0..1
  uint16_t seed;                                       // equal on every controller for the same hit
};
constexpr int kHits = 12;

// The instant being drawn, derived once per frame from the latest Score and the local clock.
// Every controller derives the same Moment for the same instant: looks may read nothing else.
struct Moment {
  double seconds;                                      // conductor time in seconds, for slow drifts
  double beat;                                         // continuous beat number on the grid
  float lock;                                          // 0..1: fades grid-driven motion in and out
  float spb;                                           // seconds per beat
  int32_t bar;                                         // bar number, counted from the grid's downbeat
  float inBar;                                         // 0..4 beats into the bar
  int32_t phrase;                                      // phrase number
  float inPhrase;                                      // 0..1 through the phrase
  Section section;
  float energy, tension;                               // 0..1
  float low, mid, high, level;                         // 0..1, fade to 0 when the Score is stale
  uint8_t look, palette;                               // current cue
  uint8_t fromLook, fromPalette;                       // previous cue, for the cross-fade
  float xfade;                                         // 0..1 from previous to current cue
  Hit hits[kHits];                                     // newest first
  uint8_t hitCount;
};

enum class Role : uint8_t { Band, Cove, Edge, Stage };

// One LED in the room, from /orion-room.json. Metres, x right, y down on the floor plan.
struct Led {
  float x, y;
  float u, v;                                          // 0..1 across the shared room box
  float r;                                             // metres from the stage (TV centre)
  float rn;                                            // r / largest r in the room, 0..1
  float a;                                             // angle around the room centre, 0..1, clockwise on the plan
  Role role;
};

struct Rgbf { float r, g, b; };                        // 0..1, perceptual (WLED applies gamma after)

struct Palette {
  const char* name;
  Rgbf a, b, accent;                                   // two colours at once, a third for hits
};

// Preset sliders, mapped to 0..1 by the glue.
struct Params {
  float intensity;                                     // how hard hits land
  float floor;                                         // background level the room never drops below
  float speed;                                         // scales free-running motion
  int program;                                         // -1 = autopilot, else a fixed look index
  int palette;                                         // -1 = autopilot, else a fixed palette index
};

using LookFn = Rgbf (*)(const Moment&, const Led&, const Params&, const Palette&);
struct Look {
  const char* name;
  LookFn fn;
};

}  // namespace orion
