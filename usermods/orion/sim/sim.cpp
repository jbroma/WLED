// Native simulator: runs the Orion engine on recorded or synthetic input and writes what every
// living-room LED shows, for render_plan.py to draw on the floor plan.
//
//   orion-sim --frames F.frames | --scores F.scores  --rooms DIR  --out PREFIX
//             [--fps 30] [--lead MS] [--program N] [--palette N]
//             [--intensity 0..1] [--floor 0..1] [--speed 0..1] [--from S] [--to S]
//
// --frames  lines "ms id volume r0..r15" (raw AR bands): runs the Conductor.
// --scores  lines written by score_text.h: renders those Scores as they are.
// Writes PREFIX.bin (header "ORS1", fps, room count, per room: LED count and a 32-byte name; then
// per frame: uint32 conductor ms and RGB bytes for every LED of every room) and PREFIX.scores.
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include "../core/conductor.h"
#include "../core/looks.h"
#include "../core/player.h"
#include "../core/room.h"
#include "score_text.h"

using namespace orion;

namespace {

struct NamedRoom {
  std::string name;
  Room room;
};

std::vector<NamedRoom> loadRooms(const std::string& dir) {
  std::vector<std::string> files;
  if (DIR* d = opendir(dir.c_str())) {
    while (dirent* e = readdir(d)) {
      std::string n = e->d_name;
      if (n.size() > 5 && n.substr(n.size() - 5) == ".json") files.push_back(n);
    }
    closedir(d);
  }
  std::sort(files.begin(), files.end());
  std::vector<NamedRoom> rooms;
  for (const std::string& f : files) {
    FILE* in = fopen((dir + "/" + f).c_str(), "rb");
    if (!in) continue;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) text.append(buf, n);
    fclose(in);
    NamedRoom r{f.substr(0, f.size() - 5), Room()};
    if (!r.room.parse(text.data(), text.size())) {
      fprintf(stderr, "bad room map %s\n", f.c_str());
      exit(2);
    }
    rooms.push_back(std::move(r));
  }
  return rooms;
}

std::vector<Score> scoresFromFrames(const char* path) {
  FILE* in = fopen(path, "r");
  if (!in) { perror(path); exit(2); }
  Conductor conductor(1);
  std::vector<Score> out;
  Frame f;
  while (true) {
    long long ms, id;
    if (fscanf(in, "%lld %lld %f", &ms, &id, &f.volume) != 3) break;
    for (float& r : f.raw)
      if (fscanf(in, "%f", &r) != 1) break;
    f.at = (Ms)ms;
    f.id = (uint32_t)id;
    out.push_back(conductor.step(f));
  }
  fclose(in);
  return out;
}

std::vector<Score> scoresFromText(const char* path) {
  FILE* in = fopen(path, "r");
  if (!in) { perror(path); exit(2); }
  std::vector<Score> out;
  Score s;
  while (scoreFromText(in, s)) out.push_back(s);
  fclose(in);
  return out;
}

uint8_t byte(float v) { return (uint8_t)lroundf(fminf(1, fmaxf(0, v)) * 255); }

}  // namespace

int main(int argc, char** argv) {
  const char *frames = nullptr, *scoresIn = nullptr, *roomDir = nullptr, *out = nullptr;
  int fps = 30, lead = 0;
  double from = 0, to = 1e9;
  Params p = {0.8f, 0.08f, 0.5f, -1, -1};
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string k = argv[i];
    const char* v = argv[i + 1];
    if (k == "--frames") frames = v;
    else if (k == "--scores") scoresIn = v;
    else if (k == "--rooms") roomDir = v;
    else if (k == "--out") out = v;
    else if (k == "--fps") fps = atoi(v);
    else if (k == "--lead") lead = atoi(v);
    else if (k == "--program") p.program = atoi(v);
    else if (k == "--palette") p.palette = atoi(v);
    else if (k == "--intensity") p.intensity = (float)atof(v);
    else if (k == "--floor") p.floor = (float)atof(v);
    else if (k == "--speed") p.speed = (float)atof(v);
    else if (k == "--from") from = atof(v);
    else if (k == "--to") to = atof(v);
    else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
  }
  if (!(frames || scoresIn) || !roomDir || !out) {
    fprintf(stderr, "usage: orion-sim --frames F | --scores F --rooms DIR --out PREFIX [options]\n");
    return 2;
  }
  const std::vector<NamedRoom> rooms = loadRooms(roomDir);
  const std::vector<Score> scores = frames ? scoresFromFrames(frames) : scoresFromText(scoresIn);
  if (scores.empty() || rooms.empty()) { fprintf(stderr, "no scores or no rooms\n"); return 2; }

  FILE* dump = fopen((std::string(out) + ".scores").c_str(), "w");
  for (const Score& s : scores) fputs(scoreToText(s).c_str(), dump);
  fclose(dump);

  FILE* bin = fopen((std::string(out) + ".bin").c_str(), "wb");
  fwrite("ORS1", 1, 4, bin);
  const uint32_t header[2] = {(uint32_t)fps, (uint32_t)rooms.size()};
  fwrite(header, 4, 2, bin);
  for (const NamedRoom& r : rooms) {
    const uint32_t n = (uint32_t)r.room.size();
    char name[32] = {};
    strncpy(name, r.name.c_str(), sizeof name - 1);
    fwrite(&n, 4, 1, bin);
    fwrite(name, 1, sizeof name, bin);
  }
  const Ms t0 = scores.front().txMs + (Ms)(from * 1000);
  const Ms t1 = (Ms)std::min<double>(scores.back().txMs, scores.front().txMs + to * 1000);
  std::vector<uint8_t> rgb;
  size_t next = 0;
  int written = 0;
  for (double t = t0; t <= t1; t += 1000.0 / fps) {
    const Ms now = (Ms)t;
    while (next + 1 < scores.size() && elapsed(scores[next + 1].txMs, now) <= 0) next++;
    const Moment m = momentAt(scores[next], now + lead);
    fwrite(&now, 4, 1, bin);
    for (const NamedRoom& r : rooms) {
      rgb.resize(r.room.size() * 3);
      for (size_t i = 0; i < r.room.size(); i++) {
        const Rgbf c = render(m, r.room.at(i), p);
        rgb[3 * i] = byte(c.r);
        rgb[3 * i + 1] = byte(c.g);
        rgb[3 * i + 2] = byte(c.b);
      }
      fwrite(rgb.data(), 1, rgb.size(), bin);
    }
    written++;
  }
  fclose(bin);
  fprintf(stderr, "%d frames at %d fps, %zu scores, %zu rooms\n", written, fps, scores.size(), rooms.size());
  return 0;
}
