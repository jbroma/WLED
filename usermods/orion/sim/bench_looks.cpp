// Times render() per look on the real room maps over a whole track's Scores.
//
//   bench-looks SCORES ROOMDIR
//
// Prints nanoseconds per render() call for every look pinned, averaged over every LED of every
// controller at 60 frames per second of the track.
#include <stdio.h>

#include <chrono>
#include <string>
#include <vector>

#include "../core/looks.h"
#include "../core/player.h"
#include "../core/room.h"
#include "score_text.h"

using namespace orion;

int main(int argc, char** argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: bench-looks SCORES ROOMDIR\n");
    return 2;
  }
  std::vector<Score> scores;
  FILE* in = fopen(argv[1], "r");
  if (!in) { perror(argv[1]); return 2; }
  for (Score s; scoreFromText(in, s);) scores.push_back(s);
  fclose(in);

  std::vector<Led> leds;
  for (const char* name : {"wled-lvr1", "wled-lvr2", "wled-lvr3", "wled-lvr-tv"}) {
    const std::string path = std::string(argv[2]) + "/" + name + ".json";
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { perror(path.c_str()); return 2; }
    std::string text;
    char buf[4096];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
    fclose(f);
    Room room;
    if (!room.parse(text.data(), text.size())) { fprintf(stderr, "bad map %s\n", name); return 2; }
    for (size_t i = 0; i < room.size(); i++) leds.push_back(room.at(i));
  }

  std::vector<Moment> moments;
  size_t next = 0;
  for (Ms t = scores.front().txMs; t <= scores.back().txMs; t += 17) {
    while (next + 1 < scores.size() && elapsed(scores[next + 1].txMs, t) <= 0) next++;
    moments.push_back(momentAt(scores[next], t));
  }

  printf("%zu moments x %zu LEDs\n", moments.size(), leds.size());
  float sink = 0;
  for (int look = 0; look < kLookCount; look++) {
    const Params p = {0.8f, 0.08f, 0.5f, look, -1};
    const auto t0 = std::chrono::steady_clock::now();
    for (const Moment& m : moments)
      for (const Led& led : leds) {
        const Rgbf c = render(m, led, p);
        sink += c.r + c.g + c.b;
      }
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
    printf("%-10s %6.1f ns per render\n", kLooks[look].name, ns / (moments.size() * leds.size()));
  }
  return sink < 0;
}
