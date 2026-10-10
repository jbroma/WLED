// The look library: what the room shows for a Moment.
#pragma once
#include "types.h"

namespace orion {

extern const Look kLooks[];
extern const int kLookCount;
extern const Palette kPalettes[];
extern const int kPaletteCount;

// Looks the autopilot may cue in a section (indices into kLooks).
struct Pool {
  const uint8_t* looks;
  uint8_t count;
};
const Pool& poolFor(Section s);

// The colour of one LED: resolves the fixed or cued look and palette, cross-fades between cues,
// and applies the floor. Pure, and the only entry point the glue and the simulator call.
Rgbf render(const Moment& m, const Led& led, const Params& p);

}  // namespace orion
