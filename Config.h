/*
 * Config.h
 * Global Configuration for M5CoreNR (Cassette Tape Real-Time Audio DSP)
 * M5Stack Core2 + M5 Module Audio (ES8388 Codec)
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// --- Firmware Metadata ---
#define APP_FIRMWARE_NAME       "M5CoreNR"
#define APP_FIRMWARE_VERSION    "v1.1.0"

// --- Display Configuration (320x240 LCD) ---
#define SCREEN_WIDTH            320
#define SCREEN_HEIGHT           240
#define HEADER_HEIGHT           24
#define VU_METER_HEIGHT         50
#define STATUS_PANEL_HEIGHT     58
#define MODE_BUTTONS_HEIGHT     76
#define GUIDE_BAR_HEIGHT        20

// --- Audio Sampling Rates & DMA Buffers ---
#define SAMPLING_FREQ           48000.0f  // 48 kHz native codec rate (FULL BANDWIDTH, NO DECIMATION)
#define AUDIO_DMA_BUF_LEN       512       // 512 stereo frames per DMA chunk (stable, zero underflow)
#define AUDIO_DMA_BUF_COUNT     8         // DMA ping-pong buffer count

// Latency in milliseconds: (512 / 48000) * 1000 = 10.6 ms
#define AUDIO_ROUNDTRIP_LATENCY_MS  (((float)AUDIO_DMA_BUF_LEN / SAMPLING_FREQ) * 1000.0f)

// --- Volume & Gain Settings ---
#define DEFAULT_HEADPHONE_VOL   90        // 90% = 0 dB (100% untouched 1:1 unity passthrough)
#define VOLUME_STEP             5         // Step per button press

// --- Hardware Pinout (M5Stack Core2 + Module Audio) ---
// I2C Control Pins (Wire1 on I2C_NUM_1)
#define SYS_I2C_SDA_PIN         21
#define SYS_I2C_SCL_PIN         22

// I2S Bus Pins
#define SYS_I2S_MCLK_PIN        0         // GPIO 0 Master Clock to ES8388
#define SYS_I2S_SCLK_PIN        19        // GPIO 19 BCLK (Bit Clock)
#define SYS_I2S_LRCK_PIN        27        // GPIO 27 WS/LRCK (Word Select)
#define SYS_I2S_DOUT_PIN        2         // GPIO 2 DOUT (To Headphone DAC)
#define SYS_I2S_DIN_PIN         34        // GPIO 34 DIN (From Line-in ADC)

// Alias mappings
#define I2S_PIN_BCK             SYS_I2S_SCLK_PIN
#define I2S_PIN_WS              SYS_I2S_LRCK_PIN
#define I2S_PIN_DOUT            SYS_I2S_DOUT_PIN
#define I2S_PIN_DIN             SYS_I2S_DIN_PIN

// --- Bluetooth A2DP (BT RX / BT TX) ---
// BT RX = A2DP Sink  : a phone/laptop streams INTO the DSP chain, replacing Line In.
// BT TX = A2DP Source: the same post-DSP audio that feeds the headphones is also
//                      SBC encoded and streamed to a Bluetooth speaker/headphone.
// Requires the "ESP32-A2DP" library (pschatzmann, Apache-2.0):
//   https://github.com/pschatzmann/ESP32-A2DP
// Set BT_ENABLE to 0 if the library is not installed - the firmware then builds
// without Bluetooth and the BT page reports "BT LIB MISSING".
#define BT_ENABLE               1
#define BT_SINK_NAME            "M5CoreNR"  // Advertised name in BT RX mode
// BT TX targets: comma separated A2DP sink names, first reachable device wins.
// NOTE: A2DP sources cannot browse devices, so the target must be listed here.
#define BT_SOURCE_PEERS         "M5CoreNR-BT,M5 Speaker"
#define BT_SOURCE_MAX_PEERS     4
#define BT_SOURCE_NAME_LEN      32
// A2DP source stream rate: the ESP32-A2DP source sends 44.1 kHz stereo SBC.
#define BT_SOURCE_RATE          44100.0f

// Stereo PCM ring buffers (allocated in PSRAM; both sizes must be powers of two)
#define BT_RX_RING_FRAMES       16384   // ~371 ms @ 44.1 kHz (BT task -> audio task)
#define BT_TX_RING_FRAMES       16384   // ~341 ms @ 48 kHz   (audio task -> BT task)
#define BT_RX_RING_FALLBACK     4096    // Used if PSRAM allocation is unavailable
#define BT_TX_RING_FALLBACK     4096
#define BT_SCRATCH_FRAMES       768     // Resampler chunk scratch (static, internal RAM)

// Asynchronous drift servo: BT clocks and our APLL are not synchronous, so the
// resampling ratio is trimmed slightly, driven by the ring fill level.
#define BT_RING_TARGET_PCT      35.0f   // Servo set point (ring fill, %)
#define BT_RING_MIN_PCT         12.0f   // Below this the stream fades out
#define BT_RING_MAX_PCT         88.0f   // Above this the stream fades out
#define BT_SERVO_KP             0.0006f // Ratio trim per % of fill error
#define BT_SERVO_MAX_TRIM       0.010f  // +/- 1 % ratio authority (~10000 ppm)
#define BT_FADE_MS              15      // De-click ramp length on link start/stop

// Headphone volume mirroring onto the BT TX stream.
// The ES8388 DAC volume law is steps = (V * 33 + 50) / 100, clamped to 33.
#define BT_TX_MIRROR_VOLUME     1
#define BT_VOL_DB_PER_STEP      0.5f    // ES8388 DAC volume step size in dB
#define BT_VOL_STEPS_MAX        33      // ES8388 DAC volume register maximum
#define BT_VOL_FLOOR_DB         96.0f   // Force silence at volume == 0

// --- Processing Modes ---
typedef enum {
    MODE_BYPASS = 0,    // Clean 1:1 bit-perfect passthrough (Raw Tape)
    MODE_DNR,           // 3-Band Split Spectral Expander (De-Hiss)
    MODE_DEHISS = MODE_DNR, // Semantic alias for 3-Band Split De-Hiss
    MODE_EXCITER,       // Psychoacoustic Harmonic Exciter (Synthesized Treble & Air)
    MODE_DOLBY_B,       // Dolby B Playback Decoder (-10 dB Sliding Band)
    MODE_DOLBY_C,       // Dolby C Playback Decoder (-20 dB Dual-Stage + Spectral Skew)
    MODE_DBX            // DBX Type II Tape Decoder (1:2 Downward RMS Expander)
} ProcessingMode;

#endif // CONFIG_H
