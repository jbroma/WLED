#include "wled.h"

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
 * GET /orion/trace?since=N returns up to 120 frames newer than sequence N still
 * in the ring, one per line:
 *   seq ms fftFrame volumeSmth b0..b15 r0..r15
 * b is fftResult (0..255) and r the raw linear channel magnitude from the
 * patched AudioReactive (um_data slot 8), stored as uint16 of r * 8.
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
  for (int b = 0; b < 16; b++) f.raw[b] = hasRaw ? (uint16_t)min(65535.0f, ((const float*)um->u_data[8])[b] * 8.0f) : 0;
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
  const uint32_t end = min(traceSeq, since + 120);
  for (uint32_t seq = since; seq < end; seq++) {
    const TraceFrame& f = traceRing[seq % TRACE_LEN];
    out->printf("%u %u %u %.2f", seq + 1, f.ms, f.fftFrame, f.vol);
    for (int b = 0; b < 16; b++) out->printf(" %u", f.bands[b]);
    for (int b = 0; b < 16; b++) out->printf(" %u", f.raw[b]);
    out->print('\n');
  }
  request->send(out);
}

class OrionUsermod : public Usermod {
  bool routed = false;
 public:
  void setup() override {
    strip.addEffect(255, &mode_orion_aurora, _data_FX_MODE_ORION_AURORA);
  }
  void connected() override {
    if (routed) return;
    server.on(F("/orion/trace"), HTTP_GET, serveTrace);
    routed = true;
  }
  void loop() override { traceAudio(); }
};

static OrionUsermod orion;
REGISTER_USERMOD(orion);
