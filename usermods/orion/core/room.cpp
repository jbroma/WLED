#include "room.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace orion {

namespace {

// Finds the key "key" (a quoted string followed by a colon, so "role":"stage" is not the key
// "stage") and returns a pointer to its value.
const char* field(const char* json, const char* end, const char* key) {
  const size_t n = strlen(key);
  for (const char* p = json; p + n + 2 < end; p++) {
    if (*p != '"' || strncmp(p + 1, key, n) != 0 || p[n + 1] != '"') continue;
    const char* q = p + n + 2;
    while (q < end && *q == ' ') q++;
    if (q >= end || *q != ':') continue;
    q++;
    while (q < end && *q == ' ') q++;
    return q;
  }
  return nullptr;
}

// Reads a JSON array of numbers starting at p ('['). Returns the count read, or -1 on bad input.
int numbers(const char* p, const char* end, std::vector<float>& out) {
  if (!p || p >= end || *p != '[') return -1;
  p++;
  out.clear();
  while (p < end) {
    while (p < end && (*p == ' ' || *p == ',' || *p == '\n')) p++;
    if (p < end && *p == ']') return (int)out.size();
    char* next = nullptr;
    const float v = strtof(p, &next);
    if (next == p) return -1;
    out.push_back(v);
    p = next;
  }
  return -1;
}

}  // namespace

bool Room::parse(const char* json, size_t len) {
  leds.clear();
  const char* end = json + len;
  std::vector<float> box, stage, xy;
  if (numbers(field(json, end, "box"), end, box) != 4) return false;
  if (numbers(field(json, end, "stage"), end, stage) != 2) return false;
  const int n = numbers(field(json, end, "xy"), end, xy);
  if (n <= 0 || n % 2) return false;

  const char* role = field(json, end, "role");
  fixtureRole = Role::Cove;
  if (role && *role == '"') {
    if (strncmp(role + 1, "band", 4) == 0) fixtureRole = Role::Band;
    else if (strncmp(role + 1, "edge", 4) == 0) fixtureRole = Role::Edge;
    else if (strncmp(role + 1, "stage", 5) == 0) fixtureRole = Role::Stage;
  }

  const float x0 = box[0] / 100, y0 = box[1] / 100, x1 = box[2] / 100, y1 = box[3] / 100;
  const float sx = stage[0] / 100, sy = stage[1] / 100;
  const float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
  // The farthest corner of the box from the stage normalises r, so rn means the same on every controller.
  float far = 0;
  for (float x : {x0, x1})
    for (float y : {y0, y1}) far = fmaxf(far, hypotf(x - sx, y - sy));

  leds.resize(n / 2);
  for (int i = 0; i < n / 2; i++) {
    Led& led = leds[i];
    led.x = xy[2 * i] / 100;
    led.y = xy[2 * i + 1] / 100;
    led.u = (led.x - x0) / fmaxf(x1 - x0, 0.01f);
    led.v = (led.y - y0) / fmaxf(y1 - y0, 0.01f);
    led.r = hypotf(led.x - sx, led.y - sy);
    led.rn = led.r / fmaxf(far, 0.01f);
    // y grows downward on the plan, so atan2 grows clockwise on screen.
    float a = atan2f(led.y - cy, led.x - cx) / (2 * (float)M_PI);
    led.a = a < 0 ? a + 1 : a;
    led.role = fixtureRole;
  }
  return true;
}

}  // namespace orion
