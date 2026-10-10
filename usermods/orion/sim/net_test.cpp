// Network model for the Orion party engine: one conductor sends perfect (truth) Scores through the
// real wire codec to three followers over a lossy, jittery link, and every controller keeps its own
// randomly offset, drifting clock. Checks that the four controllers still draw the same picture.
//
//   net-test [--scores F] [--seed N] [--loss 0.2] [--delay-min 5] [--delay-max 80] [--lead 40]
//            [--drift-ppm 30] [--late-boot 7000] [--reboot-at 60000] [--reboot-for 5000]
//
// Time T below is conductor milliseconds of the first epoch (the track's own clock). Each node maps
// it to its local millis() with a random offset and a constant drift. Beacons, joins and Scores
// follow the protocol of core/wire.h: a follower is sent Scores only while its last join is fresh.
// Exit status 0 means every assertion held from kSettleMs after each follower's first Score:
//   beat       conductor and followers are within 10 ms of each other
//   cues       look and palette agree, except for at most 10 ms around a cue switch
//   render     the picture of 3 fixed LEDs matches the conductor's within 2/255 at some instant within
//              10 ms of the same one. At the very same instant it cannot match: a follower's clock runs
//              behind by the link's shortest delay, and a look with a hard attack (a kick flash)
//              changes by more than 2/255 in that time. The same-instant difference is printed as well.
//              With exact clocks the only difference left is that nodes hold Scores of different age,
//              whose envelopes (low, mid, high, level) are not extrapolated. That is limited to 2/255 too.
// --seeds N runs N consecutive seeds and prints the worst case of each measure.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <queue>
#include <random>
#include <string>
#include <vector>

#include "../core/looks.h"
#include "../core/player.h"
#include "../core/wire.h"
#include "score_text.h"

using namespace orion;

namespace {

constexpr int kFollowers = 3;
constexpr int64_t kSettleMs = 3000;
constexpr double kBeatLimitMs = 10, kCueLimitMs = 10;
constexpr int kRenderLimit = 2;                        // of 255
constexpr int kShiftMs = 10;                           // the picture may be this far off in time and still count as the same

struct Config {
  std::string scores = "../../../.sim/bin/edm128.cues.scores";   // from sim/; `make net-test` writes it
  uint64_t seed = 1;
  double loss = 0.2;
  int delayMin = 5, delayMax = 80, lead = 40;
  double driftPpm = 30;
  int64_t lateBoot = 7000, rebootAt = 60000, rebootFor = 5000;
};

Ms wrap(int64_t v) { return (Ms)(uint64_t)v; }

// The same Score as the conductor would send in another epoch: its own clock, sequence and numbering.
Score reissue(Score s, uint8_t epoch, uint16_t seq, int64_t shift) {
  s.epoch = epoch;
  s.seq = seq;
  s.txMs = wrap((int64_t)s.txMs + shift);
  s.grid.anchorMs = wrap((int64_t)s.grid.anchorMs + shift);
  for (int i = 0; i < s.onsetCount; i++) s.onsets[i].at = wrap((int64_t)s.onsets[i].at + shift);
  return s;
}

struct InFlight {
  int64_t due;
  uint64_t order;
  int to;                                              // 0 = conductor, 1..kFollowers = follower
  int from;
  std::vector<uint8_t> bytes;
  bool operator<(const InFlight& o) const { return due != o.due ? due > o.due : order > o.order; }
};

struct Follower {
  Player player;
  Score held;
  int64_t bootT = 0;
  uint32_t offset = 0;
  double ppm = 0;
  bool knows = false;
  int64_t nextJoin = 0;
  int epoch = -1;                                      // epoch of the newest accepted Score
  int64_t firstScoreT = -1, lastBadT = -1;
  double worstSettleMs = 0, worstJoinMs = 0;
};

struct Led3 {
  Led led[3] = {{1.5f, 6.97f, 0.05f, 0.5f, 0.4f, 0.05f, 0.5f, Role::Stage},
                {4.5f, 5.0f, 0.5f, 0.5f, 3.5f, 0.45f, 0.25f, Role::Cove},
                {8.0f, 9.4f, 1.0f, 1.0f, 7.5f, 1.0f, 0.75f, Role::Edge}};
};

int byte(float v) { return (int)lroundf(fminf(1, fmaxf(0, v)) * 255); }

struct Pixels {
  int c[9];
};
Pixels draw(const Moment& m) {
  static const Led3 leds;
  static const Params p = {0.8f, 0.08f, 0.5f, -1, -1};
  Pixels px;
  for (int i = 0; i < 3; i++) {
    const Rgbf c = render(m, leds.led[i], p);
    px.c[3 * i] = byte(c.r);
    px.c[3 * i + 1] = byte(c.g);
    px.c[3 * i + 2] = byte(c.b);
  }
  return px;
}
int pixelDiff(const Pixels& a, const Pixels& b) {
  int d = 0;
  for (int i = 0; i < 9; i++) d = std::max(d, abs(a.c[i] - b.c[i]));
  return d;
}

// Smallest difference between `ref` and what a follower draws at any instant within kShiftMs of `now`.
// `guess` is the shift that aligns the beat, tried first.
int shiftedDiff(const Pixels& ref, const Player& p, Ms now, int lead, int guess) {
  auto at = [&](int shift) { return pixelDiff(ref, draw(p.moment(now + (Ms)(int32_t)shift, lead))); };
  int best = 255;
  for (int d : {0, 1, -1, 2, -2}) {
    best = std::min(best, at(std::max(-kShiftMs, std::min(kShiftMs, guess + d))));
    if (best <= kRenderLimit) return best;
  }
  for (int shift = -kShiftMs; shift <= kShiftMs; shift++) best = std::min(best, at(shift));
  return best;
}

bool loadScores(const std::string& path, std::vector<Score>& out) {
  FILE* in = fopen(path.c_str(), "r");
  if (!in) { perror(path.c_str()); return false; }
  Score s;
  while (scoreFromText(in, s)) out.push_back(s);
  fclose(in);
  return !out.empty();
}

int fails = 0;
void check(bool ok, const char* what) {
  if (!ok) { printf("FAIL  %s\n", what); fails++; }
}

// The codec on every truth Score, then the packets it must refuse.
void checkWire(const std::vector<Score>& scores) {
  uint8_t buf[kMaxPacket];
  size_t minBytes = 1u << 30, maxBytes = 0;
  double total = 0;
  int mismatches = 0;
  for (const Score& s : scores) {
    const size_t n = encode(s, buf, sizeof buf);
    Score back;
    if (n == 0 || !decode(buf, n, back) || scoreToText(back) != scoreToText(s)) mismatches++;
    minBytes = std::min(minBytes, n);
    maxBytes = std::max(maxBytes, n);
    total += n;
  }
  check(mismatches == 0, "wire: a truth Score did not survive encode then decode");
  printf("wire: %zu Scores round-trip exactly, %zu to %zu bytes, mean %.1f\n", scores.size(), minBytes, maxBytes,
         total / scores.size());

  Score s = scores[scores.size() / 2];
  s.onsetCount = kOnsets;
  const size_t full = encode(s, buf, sizeof buf);
  check(full > 0 && full <= kMaxPacket, "wire: a Score with all onsets must fit kMaxPacket");
  check(encode(s, buf, full - 1) == 0, "wire: encode into a short buffer must return 0");
  Score out;
  check(decode(buf, full, out), "wire: full Score decodes");
  std::vector<uint8_t> p(buf, buf + full);
  auto rejects = [&](const std::vector<uint8_t>& bytes, const char* what) {
    Score probe;
    check(!decode(bytes.data(), bytes.size(), probe), what);
  };
  std::vector<uint8_t> q = p;
  q[0] = 'X';
  rejects(q, "wire: wrong magic must be rejected");
  q = p;
  q[4] = 2;
  rejects(q, "wire: wrong version must be rejected");
  rejects(std::vector<uint8_t>(p.begin(), p.end() - 1), "wire: truncated packet must be rejected");
  q = p;
  q.push_back(0);
  rejects(q, "wire: trailing byte must be rejected");
  rejects(std::vector<uint8_t>(), "wire: empty packet must be rejected");
  Score bad = s;
  bad.grid.periodUs = 0;
  size_t n = encode(bad, buf, sizeof buf);
  rejects(std::vector<uint8_t>(buf, buf + n), "wire: zero beat period must be rejected");
  bad = s;
  bad.section = (Section)9;
  n = encode(bad, buf, sizeof buf);
  rejects(std::vector<uint8_t>(buf, buf + n), "wire: unknown section must be rejected");
  bad = s;
  bad.onsets[3].voice = (Voice)7;
  n = encode(bad, buf, sizeof buf);
  rejects(std::vector<uint8_t>(buf, buf + n), "wire: unknown voice must be rejected");
  bad = s;
  bad.onsetCount = 200;
  n = encode(bad, buf, sizeof buf);
  rejects(std::vector<uint8_t>(buf, buf + n), "wire: onset count above kOnsets must be rejected");

  uint8_t epoch = 0;
  n = encodeBeacon(77, buf, sizeof buf);
  check(n == 5 && classify(buf, n) == Packet::Beacon && decodeBeacon(buf, n, epoch) && epoch == 77, "wire: beacon round-trip");
  check(!decodeBeacon(buf, 4, epoch), "wire: short beacon must be rejected");
  n = encodeJoin(buf, sizeof buf);
  check(n == 4 && classify(buf, n) == Packet::Join, "wire: join classifies as Join");
  n = encode(s, buf, sizeof buf);
  check(classify(buf, n) == Packet::Score, "wire: Score classifies as Score");
  check(classify(buf, 3) == Packet::Unknown, "wire: 3 bytes classify as Unknown");
}

struct Result {
  double beatCF = 0, beatFF = 0, cue = 0, settle = 0;
  int shifted = 0, ideal = 0;
  std::vector<const char*> failures;
};

Result simulate(const Config& cfg, const std::vector<Score>& truth, bool verbose) {
  Result result;
  std::mt19937_64 rng(cfg.seed);
  auto uniform = [&]() { return std::uniform_real_distribution<double>(0, 1)(rng); };
  auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };

  const int64_t t0 = truth.front().txMs;
  const int64_t tEnd = (int64_t)truth.back().txMs + 1000;
  const int64_t rebootT = t0 + cfg.rebootAt, resumeT = rebootT + cfg.rebootFor;

  // Cue switches in conductor time, for the look and palette check: the beat a Score announces a
  // different look or palette for, as a time on the track clock, minus the lead every node draws ahead.
  std::vector<int64_t> switches;
  for (const Score& s : truth) {
    if (s.next.atBeat <= s.cur.atBeat || (s.next.look == s.cur.look && s.next.palette == s.cur.palette)) continue;
    const int64_t t = (int64_t)s.grid.anchorMs +
                      llround((s.next.atBeat - s.grid.anchorBeat) * (double)s.grid.periodUs / 1000.0) - cfg.lead;
    if (switches.empty() || switches.back() != t) switches.push_back(t);
  }

  Follower f[kFollowers + 1];                          // index 0 unused: nodes are numbered like InFlight::to
  const double ppm[kFollowers + 1] = {0, cfg.driftPpm, -cfg.driftPpm, cfg.driftPpm / 2};
  f[1].offset = (uint32_t)rng();
  f[2].offset = wrap(-70000);                          // local millis() wraps 70 s into the run
  f[3].offset = (uint32_t)rng();
  f[1].bootT = t0 + pick(0, 800);
  f[2].bootT = t0 + pick(0, 800);
  f[3].bootT = t0 + cfg.lateBoot + pick(0, 999);
  for (int i = 1; i <= kFollowers; i++) {
    f[i].ppm = ppm[i];
    f[i].nextJoin = f[i].bootT;
  }
  auto local = [&](int i, int64_t T) { return wrap((int64_t)f[i].offset + llround((double)(T - t0) * (1 + f[i].ppm * 1e-6))); };

  // The conductor's clock reads wrap(T + shift): epoch 1 wraps 30 s into the run.
  int64_t shift = -(t0 + 30000);
  uint8_t epoch = 1;
  size_t next = 0, seqBase = 0;
  bool down = false;
  Player conductor;
  Score conductorHeld;
  int64_t epochStartT = t0, nextBeacon = t0;
  int64_t lastJoin[kFollowers + 1];
  for (int64_t& j : lastJoin) j = INT64_MIN / 2;

  std::priority_queue<InFlight> wire;
  uint64_t order = 0;
  uint64_t sent = 0, lost = 0, delivered = 0, dropped = 0, scoresSent = 0, scoresLost = 0;
  auto deliver = [&](int64_t T, int from, int to, const uint8_t* bytes, size_t n, bool isScore) {
    sent++;
    if (isScore) scoresSent++;
    if (uniform() < cfg.loss) { lost++; if (isScore) scoresLost++; return; }
    wire.push({T + pick(cfg.delayMin, cfg.delayMax), order++, to, from, std::vector<uint8_t>(bytes, bytes + n)});
  };

  auto closeEpisode = [&](Follower& x) {
    if (x.firstScoreT >= 0 && x.lastBadT >= x.firstScoreT)
      x.worstSettleMs = std::max<double>(x.worstSettleMs, (double)(x.lastBadT - x.firstScoreT));
    x.lastBadT = -1;
  };

  double worstBeatCF = 0, worstBeatFF = 0, worstCue = 0;
  int worstRender = 0, worstShifted = 0, worstRenderIdeal = 0;
  int64_t worstBeatAt = 0, worstRenderAt = 0;
  uint64_t pairs = 0, pairsOver = 0;
  int64_t disagreeStart = -1;
  std::vector<double> disagreements;
  std::string disagreeNodes;
  uint64_t samples = 0;
  int unexplained = 0;

  struct Sample {
    bool valid = false;
    Moment m, ideal;
    Pixels px, pxIdeal;
  };

  for (int64_t T = t0; T <= tEnd; T++) {
    // 1. Packets that arrive now.
    while (!wire.empty() && wire.top().due <= T) {
      InFlight p = wire.top();
      wire.pop();
      if (p.to == 0) {
        if (!down && classify(p.bytes.data(), p.bytes.size()) == Packet::Join) lastJoin[p.from] = T;
        continue;
      }
      Follower& x = f[p.to];
      switch (classify(p.bytes.data(), p.bytes.size())) {
        case Packet::Beacon: {
          uint8_t e;
          if (decodeBeacon(p.bytes.data(), p.bytes.size(), e)) x.knows = true;
          break;
        }
        case Packet::Score: {
          Score s;
          if (!decode(p.bytes.data(), p.bytes.size(), s)) { result.failures.push_back("a Score that left the conductor did not decode"); break; }
          delivered++;
          const uint32_t before = x.player.stats(local(p.to, T)).received;
          x.player.accept(s, local(p.to, T));
          if (x.player.stats(local(p.to, T)).received == before) { dropped++; break; }
          x.held = s;
          if (x.epoch != s.epoch) {
            closeEpisode(x);
            x.epoch = s.epoch;
            x.firstScoreT = T;
            x.worstJoinMs = std::max<double>(x.worstJoinMs, (double)(T - std::max<int64_t>(x.bootT, epochStartT)));
          }
          break;
        }
        default:
          break;
      }
    }

    // 2. The conductor: reboot, beacon, Score.
    if (!down && T == rebootT) {
      down = true;
      for (int64_t& j : lastJoin) j = INT64_MIN / 2;
    }
    if (down && T == resumeT) {
      down = false;
      epoch++;
      shift = (int64_t)(uint32_t)rng() - T;            // a fresh boot: millis() restarts, here at a random reading
      seqBase = next;
      conductor = Player();
      epochStartT = nextBeacon = T;
    }
    if (down) {
      while (next < truth.size() && (int64_t)truth[next].txMs < T) next++;
    } else {
      uint8_t buf[kMaxPacket];
      if (T >= nextBeacon) {
        nextBeacon += kBeaconMs;
        const size_t n = encodeBeacon(epoch, buf, sizeof buf);
        for (int i = 1; i <= kFollowers; i++)
          if (T >= f[i].bootT) deliver(T, 0, i, buf, n, false);
      }
      while (next < truth.size() && (int64_t)truth[next].txMs <= T) {
        const Score s = reissue(truth[next], epoch, (uint16_t)(next - seqBase), shift);
        next++;
        conductor.acceptOwn(s);
        conductorHeld = s;
        const size_t n = encode(s, buf, sizeof buf);
        if (n == 0) result.failures.push_back("encode failed");
        for (int i = 1; i <= kFollowers; i++)
          if (T - lastJoin[i] <= (int64_t)kSubscriberExpiryMs) deliver(T, 0, i, buf, n, true);
      }
    }

    // 3. Followers that know the conductor join every kJoinMs.
    for (int i = 1; i <= kFollowers; i++) {
      if (T < f[i].bootT || !f[i].knows || T < f[i].nextJoin) continue;
      f[i].nextJoin = T + kJoinMs;
      uint8_t buf[8];
      const size_t n = encodeJoin(buf, sizeof buf);
      deliver(T, i, 0, buf, n, false);
    }

    // 4. Compare what everyone would draw at this instant.
    if (down) { disagreeStart = -1; continue; }
    Sample node[kFollowers + 1];
    const int64_t conductorNow = (int64_t)wrap(T + shift);
    node[0].valid = true;
    node[0].m = conductor.moment(wrap(T + shift), cfg.lead);
    node[0].ideal = momentAt(conductorHeld, wrap(conductorNow + cfg.lead));
    for (int i = 1; i <= kFollowers; i++) {
      if (f[i].epoch != epoch || f[i].firstScoreT < 0) continue;
      const Moment m = f[i].player.moment(local(i, T), cfg.lead);
      const double beatErr = fabs(m.beat - node[0].m.beat) * m.spb * 1000;
      if (beatErr > kBeatLimitMs) f[i].lastBadT = T;
      if (T < f[i].firstScoreT + kSettleMs) continue;
      node[i].valid = true;
      node[i].m = m;
      node[i].ideal = momentAt(f[i].held, wrap(conductorNow + cfg.lead));
    }
    int count = 0;
    for (Sample& n : node) {
      if (!n.valid) continue;
      count++;
      n.px = draw(n.m);
      n.pxIdeal = draw(n.ideal);
    }
    if (count < 2) { disagreeStart = -1; continue; }
    samples++;
    bool cuesAgree = true;
    for (int a = 0; a <= kFollowers; a++) {
      if (!node[a].valid) continue;
      for (int b = a + 1; b <= kFollowers; b++) {
        if (!node[b].valid) continue;
        const double beatMs = fabs(node[a].m.beat - node[b].m.beat) * node[a].m.spb * 1000;
        if (a == 0 && beatMs > worstBeatCF) { worstBeatCF = beatMs; worstBeatAt = T - t0; }
        if (a != 0) worstBeatFF = std::max(worstBeatFF, beatMs);
        const int px = pixelDiff(node[a].px, node[b].px);
        if (px > worstRender) { worstRender = px; worstRenderAt = T - t0; }
        if (a == 0) {
          pairs++;
          if (px > kRenderLimit) {
            pairsOver++;
            const int guess = (int)lround((node[0].m.beat - node[b].m.beat) * node[0].m.spb * 1000);
            worstShifted = std::max(worstShifted, shiftedDiff(node[0].px, f[b].player, local(b, T), cfg.lead, guess));
          }
        }
        worstRenderIdeal = std::max(worstRenderIdeal, pixelDiff(node[a].pxIdeal, node[b].pxIdeal));
        if (node[a].m.look != node[b].m.look || node[a].m.palette != node[b].m.palette) cuesAgree = false;
      }
    }
    if (!cuesAgree) {
      if (disagreeStart < 0) {
        disagreeStart = T;
        disagreeNodes.clear();
        char text[64];
        for (int a = 0; a <= kFollowers; a++) {
          if (!node[a].valid) continue;
          snprintf(text, sizeof text, " node%d look %u palette %u", a, node[a].m.look, node[a].m.palette);
          disagreeNodes += text;
        }
      }
    } else if (disagreeStart >= 0) {
      const double ms = (double)(T - disagreeStart);
      disagreements.push_back(ms);
      worstCue = std::max(worstCue, ms);
      bool near = false;
      for (int64_t sw : switches) near |= llabs(disagreeStart - sw) <= (int64_t)kCueLimitMs;
      if (!near) unexplained++;
      if (!near || ms > kCueLimitMs)
        printf("cue disagreement from +%.3f s for %.0f ms, %s a switch:%s\n", (disagreeStart - t0) / 1000.0, ms,
               near ? "at" : "away from", disagreeNodes.c_str());
      disagreeStart = -1;
    }
  }
  for (int i = 1; i <= kFollowers; i++) closeEpisode(f[i]);

  double settle = 0;
  for (int i = 1; i <= kFollowers; i++) settle = std::max(settle, f[i].worstSettleMs);
  result.beatCF = worstBeatCF;
  result.beatFF = worstBeatFF;
  result.cue = worstCue;
  result.settle = settle;
  result.shifted = worstShifted;
  result.ideal = worstRenderIdeal;
  if (samples == 0) result.failures.push_back("nothing was compared");
  if (worstBeatCF > kBeatLimitMs || worstBeatFF > kBeatLimitMs) result.failures.push_back("beat difference above the limit");
  if (worstCue > kCueLimitMs || unexplained) result.failures.push_back("look or palette disagreed away from a cue switch, or for too long");
  if (worstShifted > kRenderLimit) result.failures.push_back("render differs from the conductor's at every instant within the time shift");
  if (worstRenderIdeal > kRenderLimit) result.failures.push_back("render differs between nodes that share a clock");
  if (!verbose) return result;

  printf("network: loss %.0f%%, delay %d-%d ms, drift +-%.0f ppm, lead %d ms, seed %llu\n", cfg.loss * 100, cfg.delayMin,
         cfg.delayMax, cfg.driftPpm, cfg.lead, (unsigned long long)cfg.seed);
  printf("packets: %llu sent, %llu lost; Scores %llu sent, %llu lost (%.1f%%), %llu delivered, %llu stale or reordered\n",
         (unsigned long long)sent, (unsigned long long)lost, (unsigned long long)scoresSent,
         (unsigned long long)scoresLost, scoresSent ? 100.0 * scoresLost / scoresSent : 0.0,
         (unsigned long long)delivered, (unsigned long long)dropped);
  printf("conductor: epoch 1 until +%.1f s, down %.1f s, epoch %d after; clock wraps in epoch 1\n", cfg.rebootAt / 1000.0,
         cfg.rebootFor / 1000.0, epoch);
  for (int i = 1; i <= kFollowers; i++) {
    const Player::Stats st = f[i].player.stats(local(i, tEnd));
    printf("follower %d: boot +%.1f s, slowest join %.1f s after boot or restart, settled within %.2f s of the first Score, "
           "%u Scores accepted, %u gaps, clock %+.0f ppm\n",
           i, (f[i].bootT - t0) / 1000.0, f[i].worstJoinMs / 1000.0, f[i].worstSettleMs / 1000.0, st.received, st.gaps,
           f[i].ppm);
  }
  printf("compared %llu instants (1 ms apart), each from %lld ms after a follower's first Score\n",
         (unsigned long long)samples, (long long)kSettleMs);
  printf("beat difference: conductor to follower %.2f ms (worst at +%.1f s), follower to follower %.2f ms (limit %.0f)\n",
         worstBeatCF, worstBeatAt / 1000.0, worstBeatFF, kBeatLimitMs);
  printf("cue switches: %zu announced, %zu disagreement intervals, worst %.0f ms (limit %.0f), %d not at a switch\n",
         switches.size(), disagreements.size(), worstCue, kCueLimitMs, unexplained);
  printf("render of 3 fixed LEDs, conductor to follower, limit %d/255:\n", kRenderLimit);
  printf("  same instant: worst %d/255 at +%.1f s, above the limit for %.1f%% of instants\n", worstRender,
         worstRenderAt / 1000.0, pairs ? 100.0 * pairsOver / pairs : 0.0);
  printf("  within +-%d ms of that instant: worst %d/255\n", kShiftMs, worstShifted);
  printf("  exact clocks, each node's own newest Score: worst %d/255 (all pairs)\n", worstRenderIdeal);
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  int seeds = 1;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string k = argv[i];
    const char* v = argv[i + 1];
    if (k == "--scores") cfg.scores = v;
    else if (k == "--seed") cfg.seed = strtoull(v, nullptr, 10);
    else if (k == "--seeds") seeds = atoi(v);
    else if (k == "--loss") cfg.loss = atof(v);
    else if (k == "--delay-min") cfg.delayMin = atoi(v);
    else if (k == "--delay-max") cfg.delayMax = atoi(v);
    else if (k == "--lead") cfg.lead = atoi(v);
    else if (k == "--drift-ppm") cfg.driftPpm = atof(v);
    else if (k == "--late-boot") cfg.lateBoot = atoll(v);
    else if (k == "--reboot-at") cfg.rebootAt = atoll(v);
    else if (k == "--reboot-for") cfg.rebootFor = atoll(v);
    else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
  }
  std::vector<Score> truth;
  if (!loadScores(cfg.scores, truth)) return 2;
  printf("scores: %s (%zu Scores, %.1f s)\n", cfg.scores.c_str(), truth.size(),
         elapsed(truth.back().txMs, truth.front().txMs) / 1000.0);
  checkWire(truth);

  Result worst;
  int failing = 0;
  for (int n = 0; n < seeds; n++) {
    Config c = cfg;
    c.seed = cfg.seed + n;
    const Result r = simulate(c, truth, seeds == 1);
    worst.beatCF = std::max(worst.beatCF, r.beatCF);
    worst.beatFF = std::max(worst.beatFF, r.beatFF);
    worst.cue = std::max(worst.cue, r.cue);
    worst.settle = std::max(worst.settle, r.settle);
    worst.shifted = std::max(worst.shifted, r.shifted);
    worst.ideal = std::max(worst.ideal, r.ideal);
    if (!r.failures.empty()) failing++;
    if (seeds > 1 && !r.failures.empty())
      printf("seed %llu: %s (beat %.2f/%.2f ms, cue %.0f ms)\n", (unsigned long long)c.seed, r.failures[0], r.beatCF,
             r.beatFF, r.cue);
    for (const char* f : r.failures) check(false, f);
  }
  if (seeds > 1)
    printf("%d seeds: %d failing; worst beat difference %.2f ms conductor to follower, %.2f ms follower to follower; "
           "worst cue disagreement %.0f ms; slowest settling %.2f s; worst shifted render %d/255, exact-clock render %d/255\n",
           seeds, failing, worst.beatCF, worst.beatFF, worst.cue, worst.settle / 1000.0, worst.shifted, worst.ideal);
  printf("%s\n", fails ? "FAIL" : "PASS");
  return fails ? 1 : 0;
}
