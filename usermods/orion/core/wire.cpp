#include "wire.h"

#include <string.h>

namespace orion {

namespace {

constexpr char kScoreMagic[4] = {'O', 'R', 'N', '1'};
constexpr char kBeaconMagic[4] = {'O', 'R', 'N', 'B'};
constexpr char kJoinMagic[4] = {'O', 'R', 'N', 'J'};
constexpr uint8_t kVersion = 1;

// The two ends of the layout. Writer and Reader expose the same calls, so layout() below states
// the byte order once and the two directions cannot drift apart. Running past the end sets `bad`.
struct Writer {
  uint8_t* p;
  uint8_t* end;
  bool bad;
  Writer(uint8_t* out, size_t cap) : p(out), end(out + cap), bad(false) {}
  void u8(uint8_t& v) {
    if (p == end) { bad = true; return; }
    *p++ = v;
  }
  void u16(uint16_t& v) { uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; for (uint8_t& x : b) u8(x); }
  void u32(uint32_t& v) {
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    for (uint8_t& x : b) u8(x);
  }
  void i32(int32_t& v) { u32(reinterpret_cast<uint32_t&>(v)); }
  void tag(const char (&magic)[4], uint8_t version) {
    for (char c : magic) { uint8_t b = (uint8_t)c; u8(b); }
    uint8_t v = version;
    u8(v);
  }
};

struct Reader {
  const uint8_t* p;
  const uint8_t* end;
  bool bad;
  Reader(const uint8_t* in, size_t len) : p(in), end(in + len), bad(false) {}
  void u8(uint8_t& v) {
    if (p == end) { bad = true; v = 0; return; }
    v = *p++;
  }
  void u16(uint16_t& v) { uint8_t lo, hi; u8(lo); u8(hi); v = (uint16_t)(lo | hi << 8); }
  void u32(uint32_t& v) {
    uint8_t b[4];
    for (uint8_t& x : b) u8(x);
    v = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
  }
  void i32(int32_t& v) { u32(reinterpret_cast<uint32_t&>(v)); }
  void tag(const char (&magic)[4], uint8_t version) {
    for (char c : magic) { uint8_t b; u8(b); if (b != (uint8_t)c) bad = true; }
    uint8_t v;
    u8(v);
    if (v != version) bad = true;
  }
};

template <class IO>
void cue(IO& io, Cue& c) {
  io.u8(c.look);
  io.u8(c.palette);
  io.i32(c.atBeat);
}

// Every field of Score, in wire order: 101 bytes plus 8 per onset.
template <class IO>
void layout(IO& io, Score& s) {
  io.tag(kScoreMagic, kVersion);
  io.u8(s.epoch);
  io.u16(s.seq);
  io.u32(s.txMs);
  io.u32(s.grid.anchorMs);
  io.i32(s.grid.anchorBeat);
  io.u32(s.grid.periodUs);
  io.u8(s.grid.conf);
  io.i32(s.grid.downbeat);
  io.i32(s.grid.phraseBeat);
  io.u8(reinterpret_cast<uint8_t&>(s.section));
  io.u8(s.energy);
  io.u8(s.tension);
  io.u8(s.low);
  io.u8(s.mid);
  io.u8(s.high);
  io.u8(s.level);
  for (auto& row : s.groove)
    for (uint8_t& g : row) io.u8(g);
  cue(io, s.cur);
  cue(io, s.next);
  io.u8(s.onsetCount);
  for (int i = 0; i < s.onsetCount && i < kOnsets; i++) {
    Onset& o = s.onsets[i];
    io.u16(o.id);
    io.u8(reinterpret_cast<uint8_t&>(o.voice));
    io.u8(o.strength);
    io.u32(o.at);
  }
}

bool hasMagic(const uint8_t* in, size_t len, const char (&magic)[4]) { return len >= 4 && memcmp(in, magic, 4) == 0; }

}  // namespace

Packet classify(const uint8_t* in, size_t len) {
  if (hasMagic(in, len, kScoreMagic)) return Packet::Score;
  if (hasMagic(in, len, kBeaconMagic)) return Packet::Beacon;
  if (hasMagic(in, len, kJoinMagic)) return Packet::Join;
  return Packet::Unknown;
}

size_t encode(const Score& s, uint8_t* out, size_t cap) {
  Score copy = s;
  Writer w(out, cap);
  layout(w, copy);
  return w.bad ? 0 : (size_t)(w.p - out);
}

bool decode(const uint8_t* in, size_t len, Score& out) {
  Score s;
  Reader r(in, len);
  layout(r, s);
  if (r.bad || r.p != r.end) return false;             // wrong magic or version, too short, or trailing bytes
  if (s.onsetCount > kOnsets || s.grid.periodUs == 0) return false;
  if ((uint8_t)s.section > (uint8_t)Section::Breakdown) return false;
  for (int i = 0; i < s.onsetCount; i++)
    if ((uint8_t)s.onsets[i].voice >= kVoices) return false;
  out = s;
  return true;
}

size_t encodeBeacon(uint8_t epoch, uint8_t* out, size_t cap) {
  if (cap < 5) return 0;
  memcpy(out, kBeaconMagic, 4);
  out[4] = epoch;
  return 5;
}

bool decodeBeacon(const uint8_t* in, size_t len, uint8_t& epoch) {
  if (len != 5 || !hasMagic(in, len, kBeaconMagic)) return false;
  epoch = in[4];
  return true;
}

size_t encodeJoin(uint8_t* out, size_t cap) {
  if (cap < 4) return 0;
  memcpy(out, kJoinMagic, 4);
  return 4;
}

}  // namespace orion
