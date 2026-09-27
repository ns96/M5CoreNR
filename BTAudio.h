/*
 * BTAudio.h
 * Bluetooth A2DP audio support for M5CoreNR
 *
 *  BT RX : A2DP sink. The phone/laptop streams into the DSP chain in place of the
 *          ES8388 Line In ADC, so every DSP mode applies exactly as it does today.
 *  BT TX : A2DP source. The same post-DSP buffer that feeds the headphone DAC is
 *          also resampled, SBC encoded and streamed to a Bluetooth speaker.
 *
 * Design rules:
 *   1. The I2S full-duplex loop keeps running in every BT state. It is the 10.67 ms
 *      pace maker and the ES8388 master clock, so it is never stopped by BT.
 *   2. All sample rate conversion happens at the BT boundary, inside this module.
 *      DSP_Engine and the 48 kHz ES8388 configuration stay untouched.
 *   3. Data crosses between the audio task and the Bluetooth task through lock free
 *      single-producer/single-consumer ring buffers, and every conversion stage is
 *      drift compensated because the BT clock and the I2S APLL are asynchronous.
 *
 * M5CoreNR
 */

#ifndef BT_AUDIO_H
#define BT_AUDIO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "Config.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- Operating mode (exclusive: one A2DP role at a time) ---
typedef enum {
    BT_MODE_OFF = 0,
    BT_MODE_RX,     // A2DP sink   (receive from phone)
    BT_MODE_TX      // A2DP source (send to speaker) - headphone out stays active
} BtAudioMode;

// --- Detailed state for the UI ---
typedef enum {
    BT_STATE_OFF = 0,        // Radio disabled, RF quiet (default boot state)
    BT_STATE_NO_LIB,         // ESP32-A2DP library not installed
    BT_STATE_RX_STARTING,    // Advertising, waiting for a source to connect
    BT_STATE_RX_STREAMING,   // Audio is arriving from the phone
    BT_STATE_TX_SEARCHING,   // Looking for one of the configured peer names
    BT_STATE_TX_STREAMING    // Streaming to the peer
} BtAudioState;

// --- Lifecycle (call BTAudio_Init() lazily; nothing runs until a BT button is used) ---
bool BTAudio_Init(void);
bool BTAudio_StartRx(void);
bool BTAudio_StartTx(void);
void BTAudio_Stop(void);
void BTAudio_Update(void);          // Poll from loop(): link detection, LED/state refresh

// --- Headphone volume mirroring (audio task safe, just stores a float) ---
void BTAudio_SetTxGain(float linear);

// --- Telemetry for the display ---
BtAudioMode  BTAudio_GetMode(void);
BtAudioState BTAudio_GetState(void);
bool         BTAudio_IsLinked(void);       // true while audio is actually flowing
const char*  BTAudio_GetBadgeText(void);   // 5 char header tag: "BT:--","BT:RX","BT:TX"
const char*  BTAudio_GetStatusText(void);  // One status panel line
const char*  BTAudio_GetPeerName(void);    // "" when unknown
float        BTAudio_GetRingFillPct(void); // Active direction ring fill
uint32_t     BTAudio_GetXrunCount(void);   // Underruns + overruns since boot

// --- Audio thread data path (48 kHz domain, called from AudioTask only) ---
// BT RX: pull one block of DSP input, already converted to exactly `frames` @ 48 kHz.
size_t BTAudio_PopTo48k(int16_t* dst, size_t frames);
// BT TX: push one block of post-DSP output (same buffer the headphones receive).
void   BTAudio_PushFrom48k(const int16_t* src, size_t frames);

#ifdef __cplusplus
}
#endif

#endif // BT_AUDIO_H
