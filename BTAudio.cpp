/*
 * BTAudio.cpp
 * Bluetooth A2DP RX (sink) and TX (source) for M5CoreNR
 *
 * Threading model
 * ---------------
 *   Core 1 (loop())      : BTAudio_Start/Stop/Update, BTAudio_SetTxGain
 *   Core 0 (AudioTask)   : BTAudio_PopTo48k(), BTAudio_PushFrom48k()
 *   BT stack tasks       : A2DP sink data callback, A2DP source data callback
 *
 * Two lock free single-producer/single-consumer stereo rings cross the task
 * boundary (both in PSRAM when available):
 *
 *   BT RX : BT task --(sink rate)--> g_RxRing --(resample to 48k)--> AudioTask
 *   BT TX : AudioTask --(resample to sink rate)--> g_TxRing --(memcpy)--> BT task
 *
 * Every conversion stage is driven by a drift servo, because the Bluetooth peer
 * has its own crystal: without it the rings slowly starve or overflow (a click or
 * a gap every few minutes). The servo trims the resampling ratio by up to +/-1 %
 * based on the measured ring fill level.
 *
 * M5CoreNR
 */

#include "Config.h"
#include "BTAudio.h"
#include "AudioPipeline.h"

#include <Arduino.h>
#include <string.h>
#include <math.h>
#include <esp_heap_caps.h>
#include <esp_bt.h>

// Arduino's library auto-detection only sees #include directives that the
// preprocessor actually emits, so the A2DP headers must be included for real
// (never inside a false __has_include() branch, which would deadlock detection).
// Without the library, build with BT_ENABLE 0 in Config.h.
#if BT_ENABLE
  #if !__has_include("BluetoothA2DPSink.h") || !__has_include("BluetoothA2DPSource.h")
    #error "ESP32-A2DP library not found. Install it (https://github.com/pschatzmann/ESP32-A2DP) or set BT_ENABLE 0 in Config.h."
  #endif
  #include "BluetoothA2DPSink.h"
  #include "BluetoothA2DPSource.h"
  #include <vector>
  #define BT_LIB_PRESENT 1
#else
  #define BT_LIB_PRESENT 0
#endif

static_assert((BT_RX_RING_FRAMES & (BT_RX_RING_FRAMES - 1)) == 0, "BT_RX_RING_FRAMES must be a power of two");
static_assert((BT_TX_RING_FRAMES & (BT_TX_RING_FRAMES - 1)) == 0, "BT_TX_RING_FRAMES must be a power of two");
static_assert(BT_SCRATCH_FRAMES >= AUDIO_DMA_BUF_LEN, "BT_SCRATCH_FRAMES must hold one full DSP block");

// ============================================================================
// Lock free stereo PCM ring (single producer / single consumer, PSRAM)
// ============================================================================
struct PcmRing {
    int16_t* buf  = nullptr;
    size_t   cap  = 0;      // frames, power of two
    size_t   w    = 0;      // producer index (frames)
    size_t   r    = 0;      // consumer index (frames)
    uint32_t xrun = 0;      // underruns + overruns

    void Init(int16_t* storage, size_t frames) {
        buf = storage;
        cap = frames;
        w = 0;
        r = 0;
        xrun = 0;
        memset(buf, 0, cap * 2 * sizeof(int16_t));
    }

    size_t Count(void) const {
        const size_t ww = __atomic_load_n(&w, __ATOMIC_ACQUIRE);
        const size_t rr = __atomic_load_n(&r, __ATOMIC_ACQUIRE);
        return (ww - rr) & (cap - 1);
    }

    size_t FreeFrames(void) const { return cap - 1 - Count(); }

    float FillPct(void) const {
        return 100.0f * (float)Count() / (float)(cap - 1);
    }

    void Reset(void) {
        w = 0;
        r = 0;
        memset(buf, 0, cap * 2 * sizeof(int16_t));
    }

    // Producer: append frames, drop the excess on overrun.
    size_t Write(const int16_t* src, size_t frames) {
        if (!buf || frames == 0) return 0;
        const size_t freeFrames = FreeFrames();
        size_t n = (frames < freeFrames) ? frames : freeFrames;
        if (n < frames) __atomic_fetch_add(&xrun, 1, __ATOMIC_RELAXED);

        const size_t ww    = __atomic_load_n(&w, __ATOMIC_RELAXED);
        size_t       first = cap - ww;
        if (first > n) first = n;
        memcpy(&buf[ww * 2], src, first * 2 * sizeof(int16_t));
        if (n > first) memcpy(buf, &src[first * 2], (n - first) * 2 * sizeof(int16_t));
        __atomic_store_n(&w, (ww + n) & (cap - 1), __ATOMIC_RELEASE);
        return n;
    }

    // Consumer: inspect without consuming (so partially used rescans are lossless).
    size_t Peek(int16_t* dst, size_t frames) const {
        if (!buf || frames == 0) return 0;
        const size_t avail = Count();
        const size_t n     = (frames < avail) ? frames : avail;
        if (n == 0) return 0;
        const size_t rr    = __atomic_load_n(&r, __ATOMIC_RELAXED);
        size_t       first = cap - rr;
        if (first > n) first = n;
        memcpy(dst, &buf[rr * 2], first * 2 * sizeof(int16_t));
        if (n > first) memcpy(&dst[first * 2], buf, (n - first) * 2 * sizeof(int16_t));
        return n;
    }

    // Consumer: release `frames` that were previously peeked.
    void Commit(size_t frames) {
        if (frames == 0) return;
        const size_t rr = __atomic_load_n(&r, __ATOMIC_RELAXED);
        __atomic_store_n(&r, (rr + frames) & (cap - 1), __ATOMIC_RELEASE);
    }

    // Consumer: peek + commit + silence padding (used by the A2DP source callback).
    size_t Read(int16_t* dst, size_t frames) {
        const size_t n = Peek(dst, frames);
        Commit(n);
        if (n < frames) {
            __atomic_fetch_add(&xrun, 1, __ATOMIC_RELAXED);
            memset(&dst[n * 2], 0, (frames - n) * 2 * sizeof(int16_t));
        }
        return n;
    }
};

// ============================================================================
// Fractional resampler: 4 point Catmull-Rom cubic, continuously variable ratio
// ============================================================================
class FracResampler {
public:
    void Reset(void) {
        for (int i = 0; i < 4; i++) { m_h[i][0] = 0; m_h[i][1] = 0; }
        m_pos   = 1.0f;
        m_ratio = 1.0f;
        m_primed = false;
    }

    void SetRatio(float r) { m_ratio = r; }
    float GetRatio(void) const { return m_ratio; }

    // Produces up to `dstFrames` output frames from `srcFrames` input frames.
    // Returns the number of output frames produced; `srcUsed` receives the number of
    // input frames that were consumed (the rest must stay in the caller's buffer).
    size_t Process(const int16_t* src, size_t srcFrames,
                   int16_t* dst, size_t dstFrames, size_t* srcUsed) {
        size_t in  = 0;
        size_t out = 0;

        if (!m_primed) {
            if (srcFrames == 0) { if (srcUsed) *srcUsed = 0; return 0; }
            // Prime with the first input frame: no click, just one sample of delay.
            for (int i = 0; i < 4; i++) { m_h[i][0] = src[0]; m_h[i][1] = src[1]; }
            m_pos    = 1.0f;
            m_primed = true;
        }

        while (out < dstFrames) {
            // Slide the interpolation window until the fractional position is < 1.
            while (m_pos >= 1.0f) {
                if (in >= srcFrames) {
                    if (srcUsed) *srcUsed = in;
                    return out;   // out of input; internal state is preserved
                }
                m_h[0][0] = m_h[1][0]; m_h[0][1] = m_h[1][1];
                m_h[1][0] = m_h[2][0]; m_h[1][1] = m_h[2][1];
                m_h[2][0] = m_h[3][0]; m_h[2][1] = m_h[3][1];
                m_h[3][0] = src[in * 2];
                m_h[3][1] = src[in * 2 + 1];
                in++;
                m_pos -= 1.0f;
            }

            dst[out * 2]     = Cubic(m_h[0][0], m_h[1][0], m_h[2][0], m_h[3][0], m_pos);
            dst[out * 2 + 1] = Cubic(m_h[0][1], m_h[1][1], m_h[2][1], m_h[3][1], m_pos);
            out++;
            m_pos += m_ratio;
        }

        if (srcUsed) *srcUsed = in;
        return out;
    }

private:
    static int16_t Cubic(int16_t a, int16_t b, int16_t c, int16_t d, float t) {
        const float fa = (float)a, fb = (float)b, fc = (float)c, fd = (float)d;
        float v = fb + 0.5f * t * ((fc - fa) +
                  t * (2.0f * fa - 5.0f * fb + 4.0f * fc - fd +
                  t * (3.0f * (fb - fc) + fd - fa)));
        if (v > 32767.0f)  v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        return (int16_t)((v >= 0.0f) ? (v + 0.5f) : (v - 0.5f));
    }

    int16_t m_h[4][2] = {{0}};
    float   m_pos     = 1.0f;
    float   m_ratio   = 1.0f;
    bool    m_primed  = false;
};

// ============================================================================
// Module state
// ============================================================================
static bool         g_Inited = false;
static volatile BtAudioMode g_Mode  = BT_MODE_OFF;
static BtAudioState g_State = BT_STATE_OFF;

static PcmRing g_RxRing;                        // BT sink PCM  -> audio task
static PcmRing g_TxRing;                        // DSP output   -> A2DP source
static int16_t* g_RxStorage = nullptr;
static int16_t* g_TxStorage = nullptr;

static FracResampler g_RxRes;                   // audio task only
static FracResampler g_TxRes;                   // audio task only
static volatile bool g_ResetRequest = false;    // loop() -> audio task

static volatile float g_TxGain = 1.0f;          // headphone volume mirror
static float g_RxFade = 0.0f;                   // audio task only
static float g_TxFade = 0.0f;                   // audio task only
static bool  g_RxStarved = false;               // audio task only (gap detection)

static float    g_SinkRate     = 44100.0f;      // negotiated A2DP sink rate
static uint8_t  g_SinkChannels = 2;
static float    g_TxOutRate    = BT_SOURCE_RATE;

static volatile uint32_t g_LastRxDataMs = 0;    // BT task -> loop()
static volatile uint32_t g_LastTxPullMs = 0;    // BT task -> loop()
static uint32_t g_PeerPollMs = 0;
static char     g_PeerName[BT_SOURCE_NAME_LEN + 1] = {0};
static char     g_Status[64] = {0};

// Scratch buffers (one set per task: the audio task and the BT task never share one)
static int16_t g_AudioScratchIn[BT_SCRATCH_FRAMES * 2];
static int16_t g_AudioScratchOut[(BT_SCRATCH_FRAMES + 2) * 2];
static int16_t g_BtScratch[BT_SCRATCH_FRAMES * 2];

// ============================================================================
// Helpers
// ============================================================================
static int16_t* AllocRing(size_t frames, size_t* outFrames) {
    int16_t* p = (int16_t*)heap_caps_malloc(frames * 2 * sizeof(int16_t),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) { *outFrames = frames; return p; }

    const size_t fallback = (frames > BT_RX_RING_FALLBACK) ? BT_RX_RING_FALLBACK : frames;
    p = (int16_t*)heap_caps_malloc(fallback * 2 * sizeof(int16_t),
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (p) { *outFrames = fallback; return p; }
    return nullptr;
}

static float RampTowards(float current, float target, float step) {
    if (current < target) { current += step; if (current > target) current = target; }
    else if (current > target) { current -= step; if (current < target) current = target; }
    return current;
}

// De-click ramp length expressed as a per block increment.
static float FadeStep(size_t frames) {
    const float rampFrames = (BT_FADE_MS / 1000.0f) * SAMPLING_FREQ;
    return (rampFrames > 1.0f) ? ((float)frames / rampFrames) : 1.0f;
}

// Linear gain ramp across one block (used for every start/stop and volume change).
static void ApplyGainRamp(int16_t* buf, size_t frames, float from, float to) {
    if (frames == 0) return;
    if (from == to) {
        if (to >= 0.9999f) return;                       // unity: nothing to do
        for (size_t i = 0; i < frames * 2; i++) {
            buf[i] = (int16_t)((float)buf[i] * to);
        }
        return;
    }
    const float step = (to - from) / (float)frames;
    float g = from;
    for (size_t i = 0; i < frames; i++) {
        buf[i * 2]     = (int16_t)((float)buf[i * 2]     * g);
        buf[i * 2 + 1] = (int16_t)((float)buf[i * 2 + 1] * g);
        g += step;
    }
}

// Drift servo: trim the resampling ratio from the measured ring fill level.
// Increasing the trim consumes source frames faster, which drains the ring.
static float ServoRatio(float baseRatio, float fillPct) {
    float trim = 1.0f + BT_SERVO_KP * (fillPct - BT_RING_TARGET_PCT);
    if (trim > 1.0f + BT_SERVO_MAX_TRIM) trim = 1.0f + BT_SERVO_MAX_TRIM;
    if (trim < 1.0f - BT_SERVO_MAX_TRIM) trim = 1.0f - BT_SERVO_MAX_TRIM;
    return baseRatio * trim;
}

static const char* ShortPeer(const char* name, int maxChars) {
    if (!name || !name[0]) return "-";
    static char shortName[BT_SOURCE_NAME_LEN + 1];
    int n = 0;
    while (name[n] && n < maxChars && n < BT_SOURCE_NAME_LEN) { shortName[n] = name[n]; n++; }
    shortName[n] = 0;
    return shortName;
}

#if BT_LIB_PRESENT
// Peer names for BT TX (an A2DP source cannot browse devices, so they are configured)
static char               g_PeerNames[BT_SOURCE_MAX_PEERS][BT_SOURCE_NAME_LEN + 1];
static int                g_PeerCount = 0;
static std::vector<const char*> g_PeerVector;

static void ParseConfiguredPeers(void) {
    if (g_PeerCount > 0) return;
    const char* s = BT_SOURCE_PEERS;
    while (*s && g_PeerCount < BT_SOURCE_MAX_PEERS) {
        while (*s == ' ' || *s == ',' || *s == '\t') s++;
        if (!*s) break;

        int n = 0;
        while (*s && *s != ',' && n < BT_SOURCE_NAME_LEN) g_PeerNames[g_PeerCount][n++] = *s++;
        while (n > 0 && (g_PeerNames[g_PeerCount][n - 1] == ' ' || g_PeerNames[g_PeerCount][n - 1] == '\t')) n--;
        g_PeerNames[g_PeerCount][n] = 0;
        if (n > 0) g_PeerCount++;

        while (*s && *s != ',') s++;
    }
    Serial.printf("[BT] BT TX target list (%d): ", g_PeerCount);
    for (int i = 0; i < g_PeerCount; i++) Serial.printf("\"%s\" ", g_PeerNames[i]);
    Serial.println();
}

static void BtRadioOff(void) {
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED) {
        esp_bt_controller_disable();
    }
}

// ---------------------------------------------------------------------------
// A2DP sink: route the decoded PCM into the DSP chain instead of the codec.
// Overriding audio_data_callback() bypasses the library's output stage entirely,
// so the A2DP sink never touches our I2S bus.
// ---------------------------------------------------------------------------
static void BTAudio_OnSinkPcm(const uint8_t* data, uint32_t len);

class NrA2dpSink : public BluetoothA2DPSink {
protected:
    void audio_data_callback(const uint8_t* data, uint32_t len) override {
        BTAudio_OnSinkPcm(data, len);
    }
};

static NrA2dpSink*            g_Sink   = nullptr;
static BluetoothA2DPSource*   g_Source = nullptr;

static void BTAudio_OnSinkRate(uint16_t rate) {
    if (rate >= 8000 && rate <= 48000) {
        g_SinkRate = (float)rate;
        Serial.printf("[BT] A2DP sink stream: %u Hz\n", (unsigned)rate);
    }
}

// BT task: one decoded A2DP packet -> RX ring (no conversion, no blocking work).
static void BTAudio_OnSinkPcm(const uint8_t* data, uint32_t len) {
    if (!g_RxStorage || g_Mode != BT_MODE_RX || len == 0) return;

    uint8_t ch = 2;
    if (g_Sink) {
        const uint16_t c = g_Sink->channels();
        if (c == 1 || c == 2) ch = (uint8_t)c;
        const uint16_t rate = g_Sink->sample_rate();
        if (rate >= 8000 && rate <= 48000) g_SinkRate = (float)rate;
    }
    g_SinkChannels = ch;

    const int16_t* pcm    = (const int16_t*)data;
    const size_t   frames = len / (sizeof(int16_t) * ch);
    if (frames == 0) return;

    if (ch == 2) {
        g_RxRing.Write(pcm, frames);
    } else {
        // Mono stream: duplicate into stereo in chunks.
        size_t done = 0;
        while (done < frames) {
            size_t n = frames - done;
            if (n > BT_SCRATCH_FRAMES) n = BT_SCRATCH_FRAMES;
            for (size_t i = 0; i < n; i++) {
                const int16_t s = pcm[(done + i) * ch];
                g_BtScratch[i * 2]     = s;
                g_BtScratch[i * 2 + 1] = s;
            }
            g_RxRing.Write(g_BtScratch, n);
            done += n;
        }
    }

    g_LastRxDataMs = millis();
}

// BT task: pull the next A2DP frame block. Pure ring read, returns frame count.
static int32_t BTAudio_OnSourceData(Frame* frames, int32_t frameCount) {
    if (!frames || frameCount <= 0) return 0;

    g_LastTxPullMs = millis();

    if (!g_TxStorage || g_Mode != BT_MODE_TX) {
        memset(frames, 0, (size_t)frameCount * sizeof(Frame));
        return frameCount;
    }

    // Read() pads with silence and counts an underrun if the ring ran dry.
    g_TxRing.Read((int16_t*)frames, (size_t)frameCount);
    return frameCount;
}
#endif  // BT_LIB_PRESENT

// ============================================================================
// Public API
// ============================================================================
bool BTAudio_Init(void) {
    if (g_Inited) return true;

    size_t rxCap = 0, txCap = 0;
    g_RxStorage = AllocRing(BT_RX_RING_FRAMES, &rxCap);
    g_TxStorage = AllocRing(BT_TX_RING_FRAMES, &txCap);

    if (!g_RxStorage || !g_TxStorage) {
        Serial.println("[BT] FATAL: ring buffer allocation failed");
        if (g_RxStorage) { free(g_RxStorage); g_RxStorage = nullptr; }
        if (g_TxStorage) { free(g_TxStorage); g_TxStorage = nullptr; }
        g_State = BT_STATE_NO_LIB;
        return false;
    }

    g_RxRing.Init(g_RxStorage, rxCap);
    g_TxRing.Init(g_TxStorage, txCap);
    g_RxRes.Reset();
    g_TxRes.Reset();
    g_RxFade = 0.0f;
    g_TxFade = 0.0f;
    g_Inited = true;

    Serial.printf("[BT] Rings ready: RX %u, TX %u frames (%s)\n",
                  (unsigned)rxCap, (unsigned)txCap,
                  (rxCap == BT_RX_RING_FRAMES) ? "PSRAM" : "internal RAM");

#if BT_LIB_PRESENT
    ParseConfiguredPeers();
    g_State = BT_STATE_OFF;
#else
    Serial.println("[BT] ESP32-A2DP library not installed - BT page shows 'BT LIB MISSING'.");
    Serial.println("[BT] Install: Library Manager -> \"ESP32-A2DP\" (pschatzmann), then reflash.");
    g_State = BT_STATE_NO_LIB;
#endif
    return true;
}

bool BTAudio_StartRx(void) {
    if (!BTAudio_Init()) return false;

#if BT_LIB_PRESENT
    if (g_Mode == BT_MODE_RX) return true;                  // already in RX

    // Stop whatever role is running first (one A2DP role at a time).
    if (g_Mode == BT_MODE_TX && g_Source) { g_Source->end(false); delay(100); }

    if (!g_Sink) {
        g_Sink = new NrA2dpSink();
        if (!g_Sink) { g_State = BT_STATE_NO_LIB; return false; }
        g_Sink->set_sample_rate_callback(BTAudio_OnSinkRate);
        g_Sink->set_auto_reconnect(true, 3);
    }

    g_RxRing.Reset();
    g_RxFade = 0.0f;
    g_TxFade = 0.0f;
    g_LastRxDataMs = 0;
    g_PeerName[0] = 0;
    g_ResetRequest = true;

    g_Mode  = BT_MODE_RX;
    g_State = BT_STATE_RX_STARTING;
    AudioPipeline_SetBtTxTap(false);
    AudioPipeline_SetInputSource(AUDIO_IN_BT);

    Serial.printf("[BT] A2DP sink starting as \"%s\" - pair from your phone.\n", BT_SINK_NAME);
    g_Sink->start(BT_SINK_NAME);
    return true;
#else
    g_State = BT_STATE_NO_LIB;
    return false;
#endif
}

bool BTAudio_StartTx(void) {
    if (!BTAudio_Init()) return false;

#if BT_LIB_PRESENT
    if (g_PeerCount == 0) {
        Serial.println("[BT] BT TX: no peer names configured in Config.h (BT_SOURCE_PEERS)");
        return false;
    }
    if (g_Mode == BT_MODE_TX) return true;                  // already in TX

    if (g_Mode == BT_MODE_RX && g_Sink) { g_Sink->end(false); delay(100); }

    if (!g_Source) {
        g_Source = new BluetoothA2DPSource();
        if (!g_Source) { g_State = BT_STATE_NO_LIB; return false; }
        g_Source->set_data_callback_in_frames(BTAudio_OnSourceData);
        g_Source->set_local_name(BT_SINK_NAME);
        g_Source->set_auto_reconnect(true, 3);
    }

    g_PeerVector.clear();
    for (int i = 0; i < g_PeerCount; i++) g_PeerVector.push_back(g_PeerNames[i]);

    g_TxRing.Reset();
    g_RxFade = 0.0f;
    g_TxFade = 0.0f;
    g_LastTxPullMs = 0;
    g_ResetRequest = true;
    strncpy(g_PeerName, g_PeerNames[0], sizeof(g_PeerName) - 1);
    g_PeerName[sizeof(g_PeerName) - 1] = 0;

    g_Mode  = BT_MODE_TX;
    g_State = BT_STATE_TX_SEARCHING;
    AudioPipeline_SetInputSource(AUDIO_IN_LINE);   // BT TX sends Line In audio
    AudioPipeline_SetBtTxTap(true);                // the headphone buffer is fanned out

    Serial.printf("[BT] A2DP source searching for target \"%s\"...\n", g_PeerNames[0]);
    g_Source->start(g_PeerVector);
    return true;
#else
    g_State = BT_STATE_NO_LIB;
    return false;
#endif
}

void BTAudio_Stop(void) {
    const BtAudioMode previous = g_Mode;

    if (previous == BT_MODE_OFF && g_State == BT_STATE_OFF) return;   // already off

    g_Mode  = BT_MODE_OFF;
    g_RxFade = 0.0f;
    g_TxFade = 0.0f;
    g_LastRxDataMs = 0;
    g_LastTxPullMs = 0;

    AudioPipeline_SetBtTxTap(false);
    AudioPipeline_SetInputSource(AUDIO_IN_LINE);

#if BT_LIB_PRESENT
    if (previous == BT_MODE_RX && g_Sink) { g_Sink->end(false); delay(80); }
    if (previous == BT_MODE_TX && g_Source) { g_Source->end(false); delay(80); }
    BtRadioOff();   // restore the RF quiet baseline
#endif

    // The profile is down, so it is safe to clear the rings now.
    if (g_Inited) {
        g_RxRing.Reset();
        g_TxRing.Reset();
    }
    g_ResetRequest = true;
    g_State = BT_LIB_PRESENT ? BT_STATE_OFF : BT_STATE_NO_LIB;

    Serial.println("[BT] Off (radio disabled, Line In restored).");
}

#if BT_LIB_PRESENT
// Refresh the peer name from the A2DP sink at most once per second.
static void PollPeerName(uint32_t now) {
    if ((uint32_t)(now - g_PeerPollMs) <= 1000) return;
    g_PeerPollMs = now;
    if (!g_Sink) return;
    const char* name = g_Sink->get_peer_name();
    if (name && name[0] && strcmp(name, g_PeerName) != 0) {
        strncpy(g_PeerName, name, sizeof(g_PeerName) - 1);
        g_PeerName[sizeof(g_PeerName) - 1] = 0;
        Serial.printf("[BT] Peer: %s\n", g_PeerName);
    }
}

// Print one line per link state transition, so the serial monitor tells the story.
static void LogStateChange(void) {
    static BtAudioState last = BT_STATE_OFF;
    if (g_State == last) return;

    switch (g_State) {
        case BT_STATE_RX_CONNECTED:
            Serial.println("[BT] A2DP sink CONNECTED - press play on the phone.");
            break;
        case BT_STATE_RX_STREAMING:
            Serial.println("[BT] A2DP sink streaming.");
            break;
        case BT_STATE_RX_STARTING:
            if (last == BT_STATE_RX_CONNECTED || last == BT_STATE_RX_STREAMING) {
                Serial.println("[BT] A2DP sink link lost - advertising again.");
            }
            break;
        case BT_STATE_TX_CONNECTED:
            Serial.println("[BT] A2DP source CONNECTED - waiting for audio.");
            break;
        case BT_STATE_TX_STREAMING:
            Serial.println("[BT] A2DP source streaming.");
            break;
        case BT_STATE_TX_SEARCHING:
            if (last == BT_STATE_TX_CONNECTED || last == BT_STATE_TX_STREAMING) {
                Serial.println("[BT] A2DP source link lost - searching again.");
            }
            break;
        default:
            break;
    }
    last = g_State;
}
#endif  // BT_LIB_PRESENT

void BTAudio_Update(void) {
    if (!g_Inited) { return; }

#if BT_LIB_PRESENT
    const uint32_t now = millis();

    // A2DP link state straight from the library (paired/connected, independent of audio)
    bool connected = false;
    if (g_Mode == BT_MODE_RX && g_Sink)        connected = g_Sink->is_connected();
    else if (g_Mode == BT_MODE_TX && g_Source) connected = g_Source->is_connected();

    switch (g_Mode) {
        case BT_MODE_RX: {
            const bool streaming = g_LastRxDataMs && (uint32_t)(now - g_LastRxDataMs) < 1500;
            g_State = streaming ? BT_STATE_RX_STREAMING
                    : connected ? BT_STATE_RX_CONNECTED
                                : BT_STATE_RX_STARTING;
            if (connected) PollPeerName(now);
            break;
        }

        case BT_MODE_TX: {
            const bool pulling = g_LastTxPullMs && (uint32_t)(now - g_LastTxPullMs) < 3000;
            const bool alive   = g_Source && g_Source->is_active(6000);
            g_State = (pulling && alive) ? BT_STATE_TX_STREAMING
                    : connected          ? BT_STATE_TX_CONNECTED
                                         : BT_STATE_TX_SEARCHING;
            break;
        }

        default:
            g_State = BT_STATE_OFF;
            break;
    }

    LogStateChange();
#endif
}

void BTAudio_SetTxGain(float linear) {
    if (linear < 0.0f) linear = 0.0f;
    if (linear > 1.0f) linear = 1.0f;   // never boost the BT stream
    g_TxGain = linear;
}

BtAudioMode  BTAudio_GetMode(void)  { return g_Mode; }
BtAudioState BTAudio_GetState(void) { return g_State; }

bool BTAudio_IsLinked(void) {
    return (g_State == BT_STATE_RX_STREAMING) || (g_State == BT_STATE_TX_STREAMING);
}

int BTAudio_GetLinkLevel(void) {
    switch (g_State) {
        case BT_STATE_RX_STREAMING:
        case BT_STATE_TX_STREAMING: return 2;   // audio flowing
        case BT_STATE_RX_CONNECTED:
        case BT_STATE_TX_CONNECTED: return 1;   // paired, no audio yet
        default:                    return 0;   // not connected
    }
}

const char* BTAudio_GetBadgeText(void) {
    if (g_State == BT_STATE_NO_LIB) return "BT:!!";
    switch (g_Mode) {
        case BT_MODE_RX: return "BT:RX";
        case BT_MODE_TX: return "BT:TX";
        default:         return "BT:--";
    }
}

const char* BTAudio_GetStatusText(void) {
    switch (g_State) {
        case BT_STATE_NO_LIB:
            snprintf(g_Status, sizeof(g_Status), "BT LIB MISSING - INSTALL ESP32-A2DP");
            break;
        case BT_STATE_RX_STARTING:
            snprintf(g_Status, sizeof(g_Status), "BT RX: PAIR \"%s\" ON YOUR PHONE", BT_SINK_NAME);
            break;
        case BT_STATE_RX_CONNECTED:
            snprintf(g_Status, sizeof(g_Status), "BT RX: %s CONNECTED (IDLE)", ShortPeer(g_PeerName, 12));
            break;
        case BT_STATE_RX_STREAMING:
            snprintf(g_Status, sizeof(g_Status), "BT RX: %s  RING %3.0f%%  XRUN %lu",
                     ShortPeer(g_PeerName, 12), (double)g_RxRing.FillPct(),
                     (unsigned long)(g_RxRing.xrun + g_TxRing.xrun));
            break;
        case BT_STATE_TX_SEARCHING:
            snprintf(g_Status, sizeof(g_Status), "BT TX: SEARCHING \"%s\"", ShortPeer(g_PeerName, 18));
            break;
        case BT_STATE_TX_CONNECTED:
            snprintf(g_Status, sizeof(g_Status), "BT TX: %s CONNECTED (IDLE)", ShortPeer(g_PeerName, 12));
            break;
        case BT_STATE_TX_STREAMING:
            snprintf(g_Status, sizeof(g_Status), "BT TX: %s  RING %3.0f%%  XRUN %lu",
                     ShortPeer(g_PeerName, 12), (double)g_TxRing.FillPct(),
                     (unsigned long)(g_RxRing.xrun + g_TxRing.xrun));
            break;
        default:
            snprintf(g_Status, sizeof(g_Status), "BT OFF - RADIO DISABLED");
            break;
    }
    return g_Status;
}

const char* BTAudio_GetPeerName(void) { return g_PeerName; }

float BTAudio_GetRingFillPct(void) {
    if (!g_Inited) return 0.0f;
    if (g_Mode == BT_MODE_TX) return g_TxRing.FillPct();
    return g_RxRing.FillPct();
}

uint32_t BTAudio_GetXrunCount(void) {
    if (!g_Inited) return 0;
    return g_RxRing.xrun + g_TxRing.xrun;
}

// ============================================================================
// Audio task data path (48 kHz)
// ============================================================================
size_t BTAudio_PopTo48k(int16_t* dst, size_t frames) {
    if (!dst || frames == 0) return 0;

#if BT_LIB_PRESENT
    if (g_ResetRequest) {
        g_ResetRequest = false;
        g_RxRes.Reset();
        g_TxRes.Reset();
    }

    const bool active = g_Inited && (g_Mode == BT_MODE_RX);
    size_t produced = 0;

    if (active) {
        // If the previous block starved, restart the interpolator instead of
        // interpolating across the gap (which would produce a step artifact).
        if (g_RxStarved) {
            g_RxStarved = false;
            g_RxRes.Reset();
        }

        // Source frames per 48 kHz output frame (nominally 44100/48000 = 0.91875)
        const float baseRatio = g_SinkRate / SAMPLING_FREQ;
        g_RxRes.SetRatio(ServoRatio(baseRatio, g_RxRing.FillPct()));

        while (produced < frames) {
            const size_t remaining = frames - produced;
            size_t needIn = (size_t)((float)remaining * g_RxRes.GetRatio()) + 8;
            if (needIn > BT_SCRATCH_FRAMES) needIn = BT_SCRATCH_FRAMES;

            const size_t avail = g_RxRing.Peek(g_AudioScratchIn, needIn);
            if (avail == 0) break;

            size_t used = 0;
            const size_t n = g_RxRes.Process(g_AudioScratchIn, avail,
                                            &dst[produced * 2], remaining, &used);
            g_RxRing.Commit(used);
            produced += n;
            if (n == 0 && used == 0) break;   // stall guard
        }
    }

    if (produced < frames) {
        g_RxStarved = true;
        memset(&dst[produced * 2], 0, (frames - produced) * 2 * sizeof(int16_t));
    }

    // Open the de-click ramp only once the ring holds enough prefill to stay fed.
    const float target = (active && g_RxRing.FillPct() >= BT_RING_MIN_PCT) ? 1.0f : 0.0f;
    const float previous = g_RxFade;
    g_RxFade = RampTowards(g_RxFade, target, FadeStep(frames));
    ApplyGainRamp(dst, frames, previous, g_RxFade);

    return produced;
#else
    memset(dst, 0, frames * 2 * sizeof(int16_t));
    return 0;
#endif
}

void BTAudio_PushFrom48k(const int16_t* src, size_t frames) {
    if (!src || frames == 0) return;

#if BT_LIB_PRESENT
    if (g_ResetRequest) {
        g_ResetRequest = false;
        g_RxRes.Reset();
        g_TxRes.Reset();
    }

    // BT TX is off (or being torn down): stop feeding the ring immediately.
    if (!g_Inited || g_Mode != BT_MODE_TX) {
        g_TxFade = 0.0f;
        return;
    }

    // Buffer level control: while searching, hold the ring at the servo set point so
    // audio starts promptly; once the peer streams, allow it to rise to the maximum so
    // Bluetooth bursts are absorbed. Staying inside these limits also keeps the overrun
    // counter meaningful (it only counts real streaming hiccups).
    const float fillCap = BTAudio_IsLinked() ? BT_RING_MAX_PCT : BT_RING_TARGET_PCT;
    if (g_TxRing.FillPct() >= fillCap) return;

    // De-click ramp plus headphone volume mirroring in a single pass.
    const float previous = g_TxFade;
    g_TxFade = RampTowards(g_TxFade, g_TxGain, FadeStep(frames));

    size_t n = frames;
    if (n > BT_SCRATCH_FRAMES) n = BT_SCRATCH_FRAMES;

    memcpy(g_AudioScratchIn, src, n * 2 * sizeof(int16_t));
    ApplyGainRamp(g_AudioScratchIn, n, previous, g_TxFade);

    // 48 kHz frames per A2DP frame (nominally 48000/44100 = 1.08844)
    const float baseRatio = SAMPLING_FREQ / g_TxOutRate;
    g_TxRes.SetRatio(ServoRatio(baseRatio, g_TxRing.FillPct()));

    // The (+2) output capacity guarantees the whole block is consumed, so no input
    // frame is ever dropped here.
    size_t used = 0;
    const size_t produced = g_TxRes.Process(g_AudioScratchIn, n, g_AudioScratchOut,
                                           BT_SCRATCH_FRAMES + 2, &used);
    if (produced > 0) {
        g_TxRing.Write(g_AudioScratchOut, produced);
    }
    if (used < n) {
        // Should not happen: counts as an overrun if the scratch sizing is changed.
        __atomic_fetch_add(&g_TxRing.xrun, 1, __ATOMIC_RELAXED);
    }
#else
    (void)src;
    (void)frames;
#endif
}
