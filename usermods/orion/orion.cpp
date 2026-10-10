#include "wled.h"

#include "core/conductor.h"
#include "core/looks.h"
#include "core/player.h"
#include "core/room.h"
#include "core/wire.h"

// Orion effects for the Orion homelab controllers (custom_usermods = audioreactive orion).
//
// Metadata string format: https://kno.wled.ge/interfaces/json-api/#effect-metadata

static void mode_static(void) {
  SEGMENT.fill(SEGCOLOR(0));
}
#define FX_FALLBACK_STATIC { mode_static(); return; }

/*
 * Orion Aurora: slow, wide, soft-edged waves rendered in linear light.
 *
 * Why it exists. WLED applies colour gamma (2.8 here) to every pixel and then
 * scales the result by master brightness in the bus. A dim preset therefore
 * has a handful of output levels left, and a slow wave crawls across them one
 * visible step at a time. This effect keeps master brightness at 255 and does
 * the dimming itself:
 *
 *   1. Palette colours are converted to linear light with the gamma table.
 *   2. Waves are summed per pixel in 16-bit linear light.
 *   3. The 16-bit value is dithered in time to an 8-bit *output* level
 *      (adjacent levels alternate with a per-pixel phase, so the eye sees the
 *      average).
 *   4. The output level is run through the inverse gamma table so that the
 *      gamma stage in strip.show() lands exactly on it.
 *
 * Sliders: Speed, Waves (count), Level (dim ceiling), Width (wave width).
 * Option 1: Dither on/off (A/B for the proof of concept).
 * Run it with master brightness 255; anything lower re-quantises after gamma.
 */

#define OA_SHIFT 16
#define OA_ONE   (1L << OA_SHIFT)
#define OA_MAX_WAVES 12

class OrionWave {
  private:
    int32_t  center;        // 16.16 pixel position
    int32_t  step;          // 16.16 pixels per frame, signed
    uint32_t width;         // 16.16 half-width in pixels
    uint16_t ttl;           // frames
    uint16_t age;           // frames
    uint16_t alpha;         // 0..OA_ONE-1 peak alpha
    uint32_t env;           // cached age envelope 0..OA_ONE-1
    uint16_t lin[3];        // linear-light colour, 16 bit per channel
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
      // Half-width: 8% .. 45% of the run, scaled by the Width slider.
      uint32_t minW = (len * OA_ONE) / 12;
      uint32_t maxW = (len * OA_ONE * 45) / 100;
      width = minW + (uint64_t)(maxW - minW) * widthSlider / 255;
      if (width < OA_ONE * 2) width = OA_ONE * 2;
      bool left = hw_random8() & 1;
      // Start just outside the run and drift across it.
      center = left ? (int32_t)(len * OA_ONE + width) : -(int32_t)width;
      // Base speed 0.04 .. 0.12 px/frame before the Speed slider scales it.
      int32_t base = hw_random16(OA_ONE * 4 / 100, OA_ONE * 12 / 100);
      step = left ? -base : base;
      alive = true;
    }

    void update(uint32_t len, uint8_t speed) {
      // Speed slider 0..255 -> 1/256 .. 1 of base speed. At 128 a wave moves
      // 1.2 to 3.6 px/s, so 80 pixels take 22 to 66 seconds.
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

    // Adds this wave's linear-light contribution to acc[3] for pixel i.
    void accumulate(uint32_t i, uint32_t acc[3]) const {
      if (!alive) return;
      int32_t offset = (int32_t)(i << OA_SHIFT) - center;
      if (offset < 0) offset = -offset;
      if ((uint32_t)offset >= width) return;
      // x in 0..1 (16.16); falloff (1 - x^2)^2: flat top, no hard edge.
      uint32_t x  = ((uint64_t)offset << OA_SHIFT) / width;
      uint32_t x2 = ((uint64_t)x * x) >> OA_SHIFT;
      uint32_t f  = OA_ONE - x2;
      f = ((uint64_t)f * f) >> OA_SHIFT;
      f = ((uint64_t)f * env) >> OA_SHIFT;      // 0..OA_ONE-1
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

  // Level slider: ceiling of the linear output, 0..255 -> output level 2..255.
  const uint32_t level = 2 + ((uint32_t)SEGMENT.custom1 * 253) / 255;
  // Floor slider: output level every pixel never drops below (0..6). A pixel
  // that switches between off and level 1 is the most visible step there is;
  // a floor turns that edge into a 1-to-2 ramp instead.
  const uint32_t floorLevel = ((uint32_t)SEGMENT.custom3 * 6 + 15) / 31; // custom3 is a 5-bit slider
  const uint32_t span = level > floorLevel ? level - floorLevel : 0;
  // The floor takes its hue from colour slot 2 (the background colour): the
  // brightest channel of that colour sits at floorLevel, the others scale
  // down with it. With no background colour the floor is a neutral grey.
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
      v = (v * span) / 255;                    // 16-bit linear above the floor
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

/*
 * Trace: the AudioReactive frames this controller saw, for offline analysis.
 * GET /orion/trace?since=N returns up to 80 frames newer than sequence N still
 * in the ring, one per line:
 *   seq ms fftFrame volumeSmth b0..b15 r0..r15
 * b is fftResult (0..255) and r the raw linear channel magnitude from the
 * patched AudioReactive (um_data slot 8). The ring keeps r as log2(1 + r) in
 * 1/2048 steps: 0.03% precision over the whole range.
 * A since beyond the newest frame returns just the newest sequence number, so
 * a reader can start from "now". The response is built in RAM, hence the cap.
 * The ring is allocated on the first request, so idle controllers pay nothing.
 * Where the FFT runs (the sender) a frame is a new fftFrame count; receivers
 * have no FFT, so there a frame is a change of the GEQ bands.
 */
struct TraceFrame { uint32_t ms, fftFrame; float vol; uint8_t bands[16]; uint16_t raw[16]; };
static constexpr unsigned TRACE_LEN = 512;
static TraceFrame* traceRing = nullptr;
static uint32_t traceSeq = 0;

static void traceAudio() {
  if (!traceRing) return;
  um_data_t* um = nullptr;
  if (!UsermodManager::getUMData(&um, USERMOD_ID_AUDIOREACTIVE) || !um) return;
  const uint8_t* bands = (const uint8_t*)um->u_data[2];
  const bool hasRaw = um->u_size >= 10;
  const uint32_t fftFrame = hasRaw ? *(const uint32_t*)um->u_data[9] : 0;
  static uint32_t lastFrame = 0;
  static uint8_t lastBands[16];
  if (fftFrame) {
    if (fftFrame == lastFrame) return;
    lastFrame = fftFrame;
  } else {
    if (memcmp(bands, lastBands, 16) == 0) return;
    memcpy(lastBands, bands, 16);
  }
  TraceFrame& f = traceRing[traceSeq % TRACE_LEN];
  f.ms = millis();
  f.fftFrame = fftFrame;
  f.vol = *(const float*)um->u_data[0];
  memcpy(f.bands, bands, 16);
  for (int b = 0; b < 16; b++)
    f.raw[b] = hasRaw ? (uint16_t)min(65535.0f, log2f(1.0f + ((const float*)um->u_data[8])[b]) * 2048.0f + 0.5f) : 0;
  traceSeq++;
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
    out->print('\n');
  }
  request->send(out);
}

/*
 * Orion Party: the music-reactive look of the living room.
 *
 * The controller whose AudioReactive sync mode is send is the conductor. It turns each FFT frame into
 * a Score (core/conductor.h) and unicasts it to every follower that joined (core/wire.h). Every
 * controller, the conductor included, derives one Moment per rendered frame from the newest Score and
 * its clock (core/player.h) and colours its own LEDs from /orion-room.json (core/looks.h).
 *
 * Sliders: Speed sx, Intensity ix, Look c1 (0 = autopilot, k = look k-1), Palette c2 (same), Floor c3.
 * Presets must use of=0, mi=false, rev=false, grp=1 so segment pixel i is strip pixel start+i.
 *
 * GET /orion/state reports the link and timing. {"orion":{"lead":N}} on /json/state sets the
 * milliseconds the picture is drawn ahead of the clock (not saved).
 */
namespace {

using orion::elapsed;
using orion::Ms;

struct Stat {
  uint32_t last = 0, max = 0;                          // max since /orion/state was last read
  float avg = 0;
  void add(uint32_t v) {
    last = v;
    avg += ((float)v - avg) / 16;
    if (v > max) max = v;
  }
};

// Everything the effect and the usermod share.
struct Engine {
  orion::Room room;
  orion::Player player;
  orion::Score last;                                   // newest Score, for /orion/state only
  orion::Moment moment;                                // computed once per rendered frame
  unsigned long momentFor = 0;                         // strip.now the Moment was computed for
  bool momentValid = false;
  int leadMs = 40;
  uint32_t frameUs = 0;                                // render time of the frame in progress, all segments
  Stat renderUs, stepUs;
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
  orion::Frame lastFrame = {};                         // newest conductor input, for /orion/state only

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
  uint32_t loopMax = 0;                                // longest time between loop() calls since /orion/state was read

  void openSocket() {
    udp.stop();
    udpOpen = udp.beginMulticast(IPAddress(239, 0, 0, 1), orion::kPort);
  }

  // The newest AudioReactive FFT frame, or false when there is none since the last call. The FFT task
  // writes the bands and then bumps the counter, so a counter that did not move while copying means
  // the copy is of that frame.
  bool readFrame(orion::Frame& f, Ms now) {
    um_data_t* um = nullptr;
    if (!UsermodManager::getUMData(&um, USERMOD_ID_AUDIOREACTIVE) || !um || um->u_size < 10) return false;
    const volatile uint32_t* counter = (const volatile uint32_t*)um->u_data[9];
    for (int attempt = 0; attempt < 3; attempt++) {
      const uint32_t id = *counter;
      if (id == lastFrameId) return false;
      memcpy(f.raw, um->u_data[8], sizeof f.raw);
      if (*counter != id) continue;
      f.id = lastFrameId = id;
      f.at = now;
      f.volume = *(const float*)um->u_data[0];
      return true;
    }
    return false;
  }

  void conduct(Ms now) {
    orion::Frame f;
    if (!readFrame(f, now)) return;
    lastFrame = f;
    const uint32_t t0 = micros();
    const orion::Score& s = conductor->step(f);
    engine.stepUs.add(micros() - t0);
    engine.player.acceptOwn(s);
    engine.last = s;
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
    } else if (haveConductor && elapsed(now, nextJoin) >= 0) {
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
    traceAudio();
    const Ms now = millis();
    if (conductorRole != (conductor != nullptr)) {
      delete conductor;
      conductor = conductorRole ? new orion::Conductor(hw_random8()) : nullptr;
    }
    if (conductor) conduct(now);
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
