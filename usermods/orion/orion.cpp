#include "wled.h"

#include "core/conductor.h"
#include "core/looks.h"
#include "core/player.h"
#include "core/room.h"
#include "core/wire.h"

// Metadata string format: https://kno.wled.ge/interfaces/json-api/#effect-metadata

static void mode_static(void) {
  SEGMENT.fill(SEGCOLOR(0));
}
#define FX_FALLBACK_STATIC { mode_static(); return; }

/*
 * WLED applies colour gamma (2.8 here) to every pixel and then scales the
 * result by master brightness in the bus. A dim preset therefore has a
 * handful of output levels left, and a slow wave crawls across them one
 * visible step at a time. This effect keeps master brightness at 255 and does
 * the dimming itself, in 16-bit linear light dithered in time to 8-bit output
 * levels.
 *
 * Run it with master brightness 255; anything lower re-quantises after gamma.
 */

#define OA_SHIFT 16
#define OA_ONE   (1L << OA_SHIFT)
#define OA_MAX_WAVES 12

class OrionWave {
  private:
    int32_t  center;
    int32_t  step;
    uint32_t width;
    uint16_t ttl;
    uint16_t age;
    uint16_t alpha;
    uint32_t env;
    uint16_t lin[3];
    bool     alive;

  public:
    void init(uint32_t len, uint32_t color, uint8_t widthSlider) {
      // Linear light: what the gamma stage would have produced for this colour.
      lin[0] = (uint16_t)gamma8(R(color)) << 8;
      lin[1] = (uint16_t)gamma8(G(color)) << 8;
      lin[2] = (uint16_t)gamma8(B(color)) << 8;
      ttl   = hw_random16(1500, 3600);            // 25 s to 60 s at 60 fps
      age   = 0;
      alpha = hw_random16(OA_ONE * 3 / 5, OA_ONE - 1);
      uint32_t minW = (len * OA_ONE) / 12;
      uint32_t maxW = (len * OA_ONE * 45) / 100;
      width = minW + (uint64_t)(maxW - minW) * widthSlider / 255;
      if (width < OA_ONE * 2) width = OA_ONE * 2;
      bool left = hw_random8() & 1;
      center = left ? (int32_t)(len * OA_ONE + width) : -(int32_t)width;
      int32_t base = hw_random16(OA_ONE * 4 / 100, OA_ONE * 12 / 100);
      step = left ? -base : base;
      alive = true;
    }

    void update(uint32_t len, uint8_t speed) {
      int32_t s = (int32_t)(((int64_t)step * (1 + speed)) >> 8);
      if (s == 0) s = step > 0 ? 1 : -1;
      center += s;
      age++;
      if (age >= ttl) { alive = false; env = 0; return; } // retire before the envelope can underflow
      if (step < 0 && center < -(int32_t)width) { alive = false; env = 0; return; }
      if (step > 0 && center > (int32_t)(len * OA_ONE + width)) { alive = false; env = 0; return; }
      uint32_t half = ttl >> 1;
      env = (age < half) ? ((uint32_t)age << OA_SHIFT) / half
                         : ((uint32_t)(ttl - age) << OA_SHIFT) / half;
      if (env >= OA_ONE) env = OA_ONE - 1;
      env = ((uint64_t)env * alpha) >> OA_SHIFT;
    }

    void accumulate(uint32_t i, uint32_t acc[3]) const {
      if (!alive) return;
      int32_t offset = (int32_t)(i << OA_SHIFT) - center;
      if (offset < 0) offset = -offset;
      if ((uint32_t)offset >= width) return;
      uint32_t x  = ((uint64_t)offset << OA_SHIFT) / width;
      uint32_t x2 = ((uint64_t)x * x) >> OA_SHIFT;
      uint32_t f  = OA_ONE - x2;
      f = ((uint64_t)f * f) >> OA_SHIFT;
      f = ((uint64_t)f * env) >> OA_SHIFT;
      for (int c = 0; c < 3; c++) acc[c] += ((uint64_t)lin[c] * f) >> OA_SHIFT;
    }

    bool stillAlive() const { return alive; }
};

static void mode_orion_aurora(void) {
  const unsigned count = map(SEGMENT.intensity, 0, 255, 1, OA_MAX_WAVES);
  if (!SEGENV.allocateData(sizeof(OrionWave) * OA_MAX_WAVES)) FX_FALLBACK_STATIC;
  OrionWave* waves = reinterpret_cast<OrionWave*>(SEGENV.data);
  const uint32_t len = SEGLEN;

  for (unsigned j = 0; j < count; j++) {
    if (!waves[j].stillAlive()) {
      waves[j].init(len, SEGMENT.color_from_palette(hw_random8(), false, false, hw_random8(0, 3)), SEGMENT.custom2);
    }
    waves[j].update(len, SEGMENT.speed);
  }

  const uint32_t level = 2 + ((uint32_t)SEGMENT.custom1 * 253) / 255;
  // A pixel that switches between off and level 1 is the most visible step
  // there is; a floor turns that edge into a 1-to-2 ramp instead.
  const uint32_t floorLevel = ((uint32_t)SEGMENT.custom3 * 6 + 15) / 31; // custom3 is a 5-bit slider
  const uint32_t span = level > floorLevel ? level - floorLevel : 0;
  uint32_t floorC[3] = {floorLevel, floorLevel, floorLevel};
  {
    const uint32_t bg = SEGCOLOR(1);
    const uint32_t bgl[3] = {gamma8(R(bg)), gamma8(G(bg)), gamma8(B(bg))};
    const uint32_t bgmax = max(bgl[0], max(bgl[1], bgl[2]));
    if (bgmax) for (int c = 0; c < 3; c++) floorC[c] = (bgl[c] * floorLevel + bgmax / 2) / bgmax;
  }
  const bool dither = SEGMENT.check1;
  const uint32_t frame = SEGENV.call;

  for (uint32_t i = 0; i < len; i++) {
    uint32_t acc[3] = {0, 0, 0};
    for (unsigned j = 0; j < count; j++) waves[j].accumulate(i, acc);

    uint8_t out[3];
    // Ordered temporal dither: per-pixel phase so neighbours never blink together.
    const uint32_t threshold = (i * 97 + frame * 181) & 0xFF;
    for (int c = 0; c < 3; c++) {
      uint32_t v = acc[c];
      if (v > 0xFFFF) v = 0xFFFF;
      v = (v * span) / 255;
      uint32_t o = v >> 8;
      if (dither && (v & 0xFF) > threshold) o++;
      else if (!dither) o = (v + 128) >> 8;
      o += floorC[c];
      if (o > 255) o = 255;
      out[c] = gamma8inv((uint8_t)o);          // pre-compensate for gamma32 in show()
    }
    SEGMENT.setPixelColor(i, RGBW32(out[0], out[1], out[2], 0));
  }
}
static const char _data_FX_MODE_ORION_AURORA[] PROGMEM = "Orion Aurora@!,Waves,Level,Width,Floor,Dither;,Floor colour;!;;sx=150,ix=46,c1=40,c2=96,c3=5,o1=0,pal=50";

// The patched AudioReactive keeps its last few raw frames in a ring (um_data slots 8-11): bands,
// a cycle count and the millis() each cycle finished. Reading from the ring, a loop that stalled
// still gets every frame, stamped with when the FFT made it rather than when the loop got to it.
template <class Fn>
static bool eachNewFrame(uint32_t& last, Fn fn) {
  um_data_t* um = nullptr;
  if (!UsermodManager::getUMData(&um, USERMOD_ID_AUDIOREACTIVE) || !um || um->u_size < 12) return false;
  const float* ring = (const float*)um->u_data[8];
  const uint32_t count = *(const volatile uint32_t*)um->u_data[9];
  const uint32_t* ms = (const uint32_t*)um->u_data[10];
  const uint32_t depth = *(const uint32_t*)um->u_data[11];
  // The FFT task may be writing slot `count`; keep one slot of margin behind it.
  uint32_t id = count - last > depth - 2 ? count - (depth - 2) : last;
  for (; id < count; id++) {
    orion::Frame f;
    const uint32_t slot = id % depth;
    memcpy(f.raw, ring + slot * orion::kBands, sizeof f.raw);
    f.id = id + 1;
    f.at = ms[slot];
    f.volume = *(const float*)um->u_data[0];
    fn(f);
  }
  last = count;
  return true;
}

/*
 * Trace: the AudioReactive frames this controller saw, for offline analysis.
 * GET /orion/trace?since=N returns up to 80 frames newer than sequence N still
 * in the ring, one per line:
 *   seq ms fftFrame volumeSmth b0..b15 r0..r15 section kickAct lowRel allRel
 * b is fftResult (0..255) and r the raw linear channel magnitude from the
 * patched AudioReactive (um_data slot 8). The ring keeps r as log2(1 + r) in
 * 1/2048 steps: 0.03% precision over the whole range.
 * A since beyond the newest frame returns just the newest sequence number, so
 * a reader can start from "now". The response is built in RAM, hence the cap.
 * The ring is allocated on the first request, so idle controllers pay nothing.
 * Where the FFT runs (the sender) a frame is a new fftFrame count; receivers
 * have no FFT, so there a frame is a change of the GEQ bands.
 */
struct TraceFrame { uint32_t ms, fftFrame; float vol; uint8_t bands[16]; uint16_t raw[16]; uint8_t section; float kickAct, lowRel, allRel; };
// What the conductor concluded on each recent frame, so a trace can be compared with the simulator
// frame by frame.
struct StepNote { uint32_t frame; uint8_t section; float kickAct, lowRel, allRel; };
static StepNote stepNotes[128];
static constexpr unsigned TRACE_LEN = 512;
static TraceFrame* traceRing = nullptr;
static uint32_t traceSeq = 0;

static void traceAudio() {
  if (!traceRing) return;
  um_data_t* um = nullptr;
  if (!UsermodManager::getUMData(&um, USERMOD_ID_AUDIOREACTIVE) || !um) return;
  const uint8_t* bands = (const uint8_t*)um->u_data[2];
  auto record = [&](uint32_t ms, uint32_t frame, const float* raw) {
    TraceFrame& f = traceRing[traceSeq % TRACE_LEN];
    f.ms = ms;
    f.fftFrame = frame;
    f.vol = *(const float*)um->u_data[0];
    memcpy(f.bands, bands, 16);
    for (int b = 0; b < 16; b++) f.raw[b] = raw ? (uint16_t)min(65535.0f, log2f(1.0f + raw[b]) * 2048.0f + 0.5f) : 0;
    const StepNote& n = stepNotes[frame % 128];
    const bool noted = frame && n.frame == frame;
    f.section = noted ? n.section : 255;
    f.kickAct = noted ? n.kickAct : 0;
    f.lowRel = noted ? n.lowRel : 0;
    f.allRel = noted ? n.allRel : 0;
    traceSeq++;
  };
  static uint32_t lastFrame = 0;
  if (eachNewFrame(lastFrame, [&](const orion::Frame& f) { record(f.at, f.id, f.raw); }) && lastFrame) return;
  static uint8_t lastBands[16];
  if (memcmp(bands, lastBands, 16) == 0) return;
  memcpy(lastBands, bands, 16);
  record(millis(), 0, nullptr);
}

static void serveTrace(AsyncWebServerRequest* request) {
  if (!traceRing) traceRing = (TraceFrame*)calloc(TRACE_LEN, sizeof(TraceFrame));
  if (!traceRing) { request->send(503, "text/plain", "no memory"); return; }
  uint32_t since = request->hasParam("since") ? strtoul(request->getParam("since")->value().c_str(), nullptr, 10) : 0;
  AsyncResponseStream* out = request->beginResponseStream("text/plain");
  if (since > traceSeq) {
    out->printf("%u\n", traceSeq);
    request->send(out);
    return;
  }
  const uint32_t oldest = traceSeq > TRACE_LEN ? traceSeq - TRACE_LEN : 0;
  if (since < oldest) since = oldest;
  const uint32_t end = min(traceSeq, since + 80);
  for (uint32_t seq = since; seq < end; seq++) {
    const TraceFrame& f = traceRing[seq % TRACE_LEN];
    out->printf("%u %u %u %.2f", seq + 1, f.ms, f.fftFrame, f.vol);
    for (int b = 0; b < 16; b++) out->printf(" %u", f.bands[b]);
    for (int b = 0; b < 16; b++) out->printf(" %.1f", exp2f(f.raw[b] / 2048.0f) - 1.0f);
    out->printf(" %u %.3f %.3f %.3f\n", f.section, f.kickAct, f.lowRel, f.allRel);
  }
  request->send(out);
}

/*
 * Presets must use of=0, mi=false, rev=false, grp=1 so segment pixel i is strip pixel start+i.
 *
 * GET /orion/state reports the link and timing. {"orion":{"lead":N}} on /json/state sets the
 * milliseconds the picture is drawn ahead of the clock (not saved).
 */
namespace {

using orion::elapsed;
using orion::Ms;

struct Stat {
  uint32_t last = 0, max = 0;
  float avg = 0;
  void add(uint32_t v) {
    last = v;
    avg += ((float)v - avg) / 16;
    if (v > max) max = v;
  }
};

struct Engine {
  orion::Room room;
  orion::Player player;
  orion::Score last;
  orion::Moment moment;
  unsigned long momentFor = 0;
  bool momentValid = false;
  int leadMs = 40;
  uint32_t frameUs = 0;
  Stat renderUs, stepUs;
  Ms renderedAt = 0;                                   // last frame Orion Party drew here
} engine;

uint8_t toByte(float v) { return (uint8_t)(v * 255.0f + 0.5f); }   // render() already clamps to 0..1

// Upper edges of the buckets /orion/state counts Score inter-arrival times in; one more bucket for the rest.
const uint8_t kGapEdges[8] = {2, 5, 10, 20, 30, 40, 60, 100};

}  // namespace

static void mode_orion_party(void) {
  const uint32_t t0 = micros();
  if (!engine.momentValid || engine.momentFor != strip.now) {
    engine.renderUs.add(engine.frameUs);
    engine.frameUs = 0;
    engine.moment = engine.player.moment(millis(), engine.leadMs);
    engine.momentFor = strip.now;
    engine.renderedAt = millis();
    engine.momentValid = true;
  }
  if (engine.room.size() == 0) {
    SEGMENT.fill(BLACK);
  } else {
    const orion::Params p = {SEGMENT.intensity / 255.0f, SEGMENT.custom3 / 31.0f, SEGMENT.speed / 255.0f,
                             (int)SEGMENT.custom1 - 1, (int)SEGMENT.custom2 - 1};
    const unsigned len = SEGLEN, start = SEGMENT.start;
    for (unsigned i = 0; i < len; i++) {
      const orion::Rgbf c = orion::render(engine.moment, engine.room.at(start + i), p);
      SEGMENT.setPixelColor(i, RGBW32(toByte(c.r), toByte(c.g), toByte(c.b), 0));
    }
  }
  engine.frameUs += micros() - t0;
}
static const char _data_FX_MODE_ORION_PARTY[] PROGMEM = "Orion Party@Speed,Intensity,Look,Palette,Floor;;;;sx=128,ix=204,c1=0,c2=0,c3=3";

class OrionUsermod : public Usermod {
  struct Subscriber {
    IPAddress ip;
    uint16_t port = 0;
    Ms lastJoin = 0;
    bool used = false;
  };

  bool routed = false;
  bool conductorRole = false;                          // AudioReactive sync mode is send
  orion::Conductor* conductor = nullptr;
  uint32_t lastFrameId = 0;
  orion::Frame lastFrame = {};

  WiFiUDP udp;
  bool udpOpen = false;
  Subscriber subs[orion::kMaxSubscribers];
  Ms nextBeacon = 0, nextJoin = 0;
  uint32_t sent = 0, sendFailures = 0;

  IPAddress conductorIp;
  uint8_t beaconEpoch = 0;
  bool haveConductor = false;
  Ms lastBeacon = 0, lastRx = 0, lastAccept = 0;
  bool haveRx = false;
  uint32_t bad = 0, gapHist[9] = {}, gapMax = 0;

  Ms lastLoop = 0, lastPoll = 0;
  uint32_t loopMax = 0;

  void openSocket() {
    udp.stop();
    udpOpen = udp.beginMulticast(IPAddress(239, 0, 0, 1), orion::kPort);
  }

  void conduct(Ms now) {
    bool stepped = false;
    eachNewFrame(lastFrameId, [&](const orion::Frame& f) {
      lastFrame = f;
      const uint32_t t0 = micros();
      engine.last = conductor->step(f);
      engine.stepUs.add(micros() - t0);
      const orion::Conductor::Debug d = conductor->debug();
      stepNotes[f.id % 128] = {f.id, (uint8_t)engine.last.section, d.kickActivity, d.lowRel, d.allRel};
      stepped = true;
    });
    if (!stepped) return;
    const orion::Score& s = engine.last;
    engine.player.acceptOwn(s);
    if (!udpOpen) return;
    uint8_t buf[orion::kMaxPacket];
    const size_t n = orion::encode(s, buf, sizeof buf);
    for (Subscriber& sub : subs) {
      if (!sub.used) continue;
      if (elapsed(now, sub.lastJoin) > (int32_t)orion::kSubscriberExpiryMs) { sub.used = false; continue; }
      if (udp.beginPacket(sub.ip, sub.port)) {
        udp.write(buf, n);
        if (udp.endPacket()) { sent++; continue; }
      }
      sendFailures++;
    }
  }

  void subscribe(IPAddress ip, uint16_t port, Ms now) {
    Subscriber* slot = nullptr;
    for (Subscriber& s : subs)
      if (s.used && s.ip == ip) { slot = &s; break; }
    if (!slot)
      for (Subscriber& s : subs)
        if (!s.used || elapsed(now, s.lastJoin) > (int32_t)orion::kSubscriberExpiryMs) { slot = &s; break; }
    if (!slot) return;
    slot->ip = ip;
    slot->port = port;
    slot->lastJoin = now;
    slot->used = true;
  }

  void follow(const uint8_t* buf, size_t len, Ms now) {
    orion::Score s;
    if (!orion::decode(buf, len, s)) { bad++; return; }
    if (haveRx) {
      const uint32_t gap = (uint32_t)elapsed(now, lastRx);
      size_t b = 0;
      while (b < 8 && gap >= kGapEdges[b]) b++;
      gapHist[b]++;
      if (gap > gapMax) gapMax = gap;
    }
    haveRx = true;
    lastRx = now;
    const uint32_t before = engine.player.stats(now).received;
    engine.player.accept(s, now);
    bool accepted = engine.player.stats(now).received != before;
    if (!accepted && elapsed(now, lastAccept) > 3000) {
      // Scores keep arriving but none is newer: a conductor that restarted into the same epoch with
      // a lower sequence number. Start over from this one.
      engine.player = orion::Player();
      engine.player.accept(s, now);
      accepted = true;
    }
    if (accepted) {
      lastAccept = now;
      engine.last = s;
    }
  }

  void receive(Ms now) {
    uint8_t buf[orion::kMaxPacket];
    for (int n = 0; n < 8; n++) {
      const int len = udp.parsePacket();
      if (len <= 0) return;
      const IPAddress from = udp.remoteIP();
      const uint16_t port = udp.remotePort();
      if (len > (int)sizeof buf) { udp.flush(); continue; }
      udp.read(buf, sizeof buf);
      switch (orion::classify(buf, len)) {
        case orion::Packet::Score:
          if (!conductor) follow(buf, len, now);
          break;
        case orion::Packet::Join:
          if (conductor) subscribe(from, port, now);
          break;
        case orion::Packet::Beacon:
          if (conductor || !orion::decodeBeacon(buf, len, beaconEpoch)) break;
          if (!haveConductor || !(from == conductorIp)) nextJoin = now;
          conductorIp = from;
          haveConductor = true;
          lastBeacon = now;
          break;
        default:
          break;
      }
    }
  }

  void announce(Ms now) {
    uint8_t buf[orion::kMaxPacket];
    if (conductor) {
      if (elapsed(now, nextBeacon) < 0) return;
      nextBeacon = now + orion::kBeaconMs;
      const size_t n = orion::encodeBeacon(conductor->score().epoch, buf, sizeof buf);
      if (udp.beginMulticastPacket()) {
        udp.write(buf, n);
        udp.endPacket();
      }
    } else if (haveConductor && elapsed(now, engine.renderedAt) < 3000 && elapsed(now, nextJoin) >= 0) {
      // Join only while Orion Party draws here: the conductor then stops streaming to this
      // controller once its subscription expires, instead of sending 43 Scores a second all day.
      nextJoin = now + orion::kJoinMs;
      const size_t n = orion::encodeJoin(buf, sizeof buf);
      if (udp.beginPacket(conductorIp, orion::kPort)) {
        udp.write(buf, n);
        udp.endPacket();
      }
    }
  }

  void loadRoom() {
    File f = WLED_FS.open("/orion-room.json", "r");
    if (!f) return;
    const size_t n = f.size();
    char* text = (char*)malloc(n + 1);
    if (text) {
      const size_t got = f.readBytes(text, n);
      engine.room.parse(text, got);
      free(text);
    }
    f.close();
  }

  void writeState(AsyncWebServerRequest* request) {
    const Ms now = millis();
    const orion::Player::Stats st = engine.player.stats(now);
    const orion::Moment m = engine.player.moment(now, 0);
    const orion::Score s = engine.last;
    AsyncResponseStream* out = request->beginResponseStream("application/json");
    out->printf("{\"role\":\"%s\",\"epoch\":%u,\"seq\":%u,\"received\":%u,\"gaps\":%u,\"offsetMs\":%d,\"ageMs\":%u,"
                "\"millis\":%u,\"beat\":%.4f,\"bpm\":%.2f,\"conf\":%u,\"bar\":%d,\"section\":%u,\"look\":%u,\"palette\":%u,"
                "\"leds\":%u,\"lead\":%d,\"frame\":%u,",
                conductor ? "conductor" : "follower", s.epoch, s.seq, st.received, st.gaps, (int)st.offsetMs,
                st.ageMs, now, m.beat, 60.0f / m.spb, s.grid.conf, (int)m.bar, (unsigned)m.section, m.look,
                m.palette, (unsigned)engine.room.size(), engine.leadMs, lastFrameId);
    if (conductor) {
      const orion::Conductor::Debug d = conductor->debug();
      out->printf("\"volume\":%.1f,\"low\":%u,\"mid\":%u,\"high\":%u,\"level\":%u,\"raw\":[", lastFrame.volume, s.low, s.mid,
                  s.high, s.level);
      for (int b = 0; b < orion::kBands; b++) out->printf("%s%.0f", b ? "," : "", lastFrame.raw[b]);
      out->printf("],\"sent\":%u,\"sendFail\":%u,\"analysis\":{\"onset\":%.3f,\"bpm\":%.2f,\"tempoStrength\":%.3f},"
                  "\"subscribers\":[", sent, sendFailures, d.onset, d.bpm, d.tempoStrength);
      bool first = true;
      for (const Subscriber& sub : subs) {
        if (!sub.used) continue;
        out->printf("%s{\"ip\":\"%s\",\"ageMs\":%u}", first ? "" : ",", sub.ip.toString().c_str(), now - sub.lastJoin);
        first = false;
      }
      out->print("],");
    } else {
      if (haveConductor)
        out->printf("\"conductor\":\"%s\",\"beaconEpoch\":%u,\"beaconAgeMs\":%u,", conductorIp.toString().c_str(),
                    beaconEpoch, now - lastBeacon);
      else
        out->print("\"conductor\":null,");
      out->printf("\"bad\":%u,\"gapMaxMs\":%u,\"arrival\":[", bad, gapMax);
      for (int b = 0; b < 9; b++) out->printf("%s%u", b ? "," : "", gapHist[b]);
      out->print("],");
    }
    out->printf("\"stepUs\":{\"last\":%u,\"avg\":%.0f,\"max\":%u},\"renderUs\":{\"last\":%u,\"avg\":%.0f,\"max\":%u},"
                "\"loopMaxMs\":%u}",
                engine.stepUs.last, engine.stepUs.avg, engine.stepUs.max, engine.renderUs.last,
                engine.renderUs.avg, engine.renderUs.max, loopMax);
    request->send(out);
    engine.stepUs.max = engine.renderUs.max = loopMax = 0;
  }

 public:
  void setup() override {
    strip.addEffect(255, &mode_orion_aurora, _data_FX_MODE_ORION_AURORA);
    strip.addEffect(255, &mode_orion_party, _data_FX_MODE_ORION_PARTY);
    loadRoom();
  }

  // readFromConfig() gets the whole "um" object of cfg.json (cfg.cpp passes doc["um"] to every
  // usermod), so another usermod's settings can be read here, whatever order they load in.
  bool readFromConfig(JsonObject& root) override {
    conductorRole = ((root["AudioReactive"]["sync"]["mode"] | 0) & 1) != 0;
    return true;
  }

  void readFromJsonState(JsonObject& root) override {
    JsonObject o = root["orion"];
    if (!o.isNull() && o["lead"].is<int>()) engine.leadMs = constrain(o["lead"].as<int>(), 0, 500);
  }

  void connected() override {
    openSocket();
    if (routed) return;
    server.on(F("/orion/trace"), HTTP_GET, serveTrace);
    server.on(F("/orion/state"), HTTP_GET, [this](AsyncWebServerRequest* request) { writeState(request); });
    routed = true;
  }

  void onUpdateBegin(bool init) override {
    if (init) {
      udp.stop();
      udpOpen = false;
    } else if (WiFi.status() == WL_CONNECTED) {
      openSocket();
    }
  }

  void addToJsonInfo(JsonObject& root) override {
    JsonObject user = root["u"];
    if (user.isNull()) user = root.createNestedObject("u");
    JsonArray row = user.createNestedArray(F("Orion"));
    char text[48];
    if (conductor) {
      int followers = 0;
      for (const Subscriber& sub : subs) followers += sub.used;
      snprintf(text, sizeof text, "conductor, %d followers", followers);
    } else if (engine.player.hasScore()) {
      snprintf(text, sizeof text, "follower, Score %u ms old", engine.player.stats(millis()).ageMs);
    } else {
      snprintf(text, sizeof text, "follower, no conductor");
    }
    row.add(String(text));
  }

  void loop() override {
    const Ms now = millis();
    if (conductorRole != (conductor != nullptr)) {
      delete conductor;
      conductor = conductorRole ? new orion::Conductor(hw_random8()) : nullptr;
    }
    if (conductor) conduct(now);
    traceAudio();                                      // after the conductor, so its notes are in
    if (now == lastPoll) return;                       // the UDP socket needs no polling faster than 1 kHz
    if (elapsed(now, lastLoop) > (int32_t)loopMax) loopMax = elapsed(now, lastLoop);
    lastLoop = lastPoll = now;
    if (udpOpen) {
      receive(now);
      announce(now);
    }
  }
};

static OrionUsermod orionUsermod;
REGISTER_USERMOD(orionUsermod);
