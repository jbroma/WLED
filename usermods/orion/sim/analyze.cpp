// Runs the Conductor over a frames file and writes what it decided, for eval_analysis.py.
//
//   orion-analyze IN.frames OUT_PREFIX
//
// IN.frames: lines "ms id volume r0..r15" (ar_frames.py, eval_frames.py, trace_frames.py).
// Writes OUT_PREFIX.scores (score_text.h lines, one per frame) and OUT_PREFIX.debug (one line per
// frame: ms and the Conductor's Debug fields, header first). Prints the time per step to stderr.
#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include "../core/conductor.h"
#include "score_text.h"

using namespace orion;

int main(int argc, char** argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: orion-analyze IN.frames OUT_PREFIX\n");
    return 2;
  }
  FILE* in = fopen(argv[1], "r");
  if (!in) { perror(argv[1]); return 2; }
  const std::string prefix = argv[2];
  FILE* scores = fopen((prefix + ".scores").c_str(), "w");
  FILE* debug = fopen((prefix + ".debug").c_str(), "w");
  if (!scores || !debug) { perror(argv[2]); return 2; }
  fprintf(debug, "ms onset bpm strength kick snare hat phaseErr hit downbeatPhase lowRel allRel kickAct rise\n");

  Conductor conductor(1);
  std::vector<double> times;
  Frame f;
  while (true) {
    long long ms, id;
    if (fscanf(in, "%lld %lld %f", &ms, &id, &f.volume) != 3) break;
    for (float& r : f.raw)
      if (fscanf(in, "%f", &r) != 1) break;
    f.at = (Ms)ms;
    f.id = (uint32_t)id;
    const auto t0 = std::chrono::steady_clock::now();
    const Score& s = conductor.step(f);
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    times.push_back(us);
    fputs(scoreToText(s).c_str(), scores);
    const Conductor::Debug d = conductor.debug();
    fprintf(debug, "%lld %.4f %.2f %.3f %.3f %.3f %.3f %.1f %d %d %.3f %.3f %.3f %.3f\n", ms, d.onset, d.bpm,
            d.tempoStrength, d.voice[0], d.voice[1], d.voice[2], d.phaseErrMs, d.hit, d.downbeatPhase, d.lowRel,
            d.allRel, d.kickActivity, d.rise);
  }
  fclose(in);
  fclose(scores);
  fclose(debug);
  // The first steps pay for cold caches and page faults; report the steady state.
  const size_t warm = times.size() > 200 ? 100 : 0;
  std::vector<double> steady(times.begin() + warm, times.end());
  double total = 0;
  for (double t : steady) total += t;
  std::sort(steady.begin(), steady.end());
  const size_t n = steady.size();
  fprintf(stderr, "%zu frames, %.2f us/step mean, %.1f us p99, %.1f us worst\n", times.size(), n ? total / n : 0,
          n ? steady[n * 99 / 100] : 0, n ? steady.back() : 0);
  return 0;
}
