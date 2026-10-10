// The controller's LEDs in room space, parsed from /orion-room.json:
//   {"v":1,"role":"cove","box":[x0,y0,x1,y1],"stage":[x,y],"xy":[x,y,...]}   (centimetres)
// written by orion-ha scripts/wled-room-map.py. LED i of the map is strip pixel i.
#pragma once
#include <vector>

#include "types.h"

namespace orion {

class Room {
 public:
  // Parses the map; returns false and leaves the room empty when the text is not a valid map.
  bool parse(const char* json, size_t len);
  size_t size() const { return leds.size(); }
  // Pixels past the map (a segment longer than the map) read as LED 0.
  const Led& at(size_t i) const { return leds[i < leds.size() ? i : 0]; }
  Role role() const { return fixtureRole; }

 private:
  std::vector<Led> leds;
  Role fixtureRole = Role::Cove;
};

}  // namespace orion
