#pragma once
#include <stdio.h>

#include <string>

#include "../core/types.h"

namespace orion {

inline std::string scoreToText(const Score& s) {
  std::string out;
  char buf[64];
  auto put = [&](long long v) { snprintf(buf, sizeof buf, "%lld ", v); out += buf; };
  put(s.txMs); put(s.epoch); put(s.seq);
  put(s.grid.anchorMs); put(s.grid.anchorBeat); put(s.grid.periodUs); put(s.grid.conf);
  put(s.grid.downbeat); put(s.grid.phraseBeat);
  put((int)s.section); put(s.energy); put(s.tension);
  put(s.low); put(s.mid); put(s.high); put(s.level);
  put(s.cur.look); put(s.cur.palette); put(s.cur.atBeat);
  put(s.next.look); put(s.next.palette); put(s.next.atBeat);
  for (int v = 0; v < kVoices; v++)
    for (int k = 0; k < kStepsPerBar; k++) put(s.groove[v][k]);
  put(s.onsetCount);
  for (int i = 0; i < s.onsetCount; i++) {
    put(s.onsets[i].id); put((int)s.onsets[i].voice); put(s.onsets[i].strength); put(s.onsets[i].at);
  }
  out.back() = '\n';
  return out;
}

// Returns false at end of input or on a malformed line.
inline bool scoreFromText(FILE* in, Score& s) {
  long long v[22];
  for (long long& x : v)
    if (fscanf(in, "%lld", &x) != 1) return false;
  s = Score();
  s.txMs = (Ms)v[0]; s.epoch = (uint8_t)v[1]; s.seq = (uint16_t)v[2];
  s.grid.anchorMs = (Ms)v[3]; s.grid.anchorBeat = (int32_t)v[4]; s.grid.periodUs = (uint32_t)v[5];
  s.grid.conf = (uint8_t)v[6]; s.grid.downbeat = (int32_t)v[7]; s.grid.phraseBeat = (int32_t)v[8];
  s.section = (Section)v[9]; s.energy = (uint8_t)v[10]; s.tension = (uint8_t)v[11];
  s.low = (uint8_t)v[12]; s.mid = (uint8_t)v[13]; s.high = (uint8_t)v[14]; s.level = (uint8_t)v[15];
  s.cur = {(uint8_t)v[16], (uint8_t)v[17], (int32_t)v[18]};
  s.next = {(uint8_t)v[19], (uint8_t)v[20], (int32_t)v[21]};
  for (int i = 0; i < kVoices; i++)
    for (int k = 0; k < kStepsPerBar; k++) {
      long long g;
      if (fscanf(in, "%lld", &g) != 1) return false;
      s.groove[i][k] = (uint8_t)g;
    }
  long long n;
  if (fscanf(in, "%lld", &n) != 1 || n < 0 || n > kOnsets) return false;
  s.onsetCount = (uint8_t)n;
  for (int i = 0; i < n; i++) {
    long long id, voice, strength, at;
    if (fscanf(in, "%lld %lld %lld %lld", &id, &voice, &strength, &at) != 4) return false;
    s.onsets[i] = {(uint16_t)id, (Voice)voice, (uint8_t)strength, (Ms)at};
  }
  return true;
}

}  // namespace orion
