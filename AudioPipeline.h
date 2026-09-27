/*
 * AudioPipeline.h
 * High-Performance I2S DMA Audio Driver and ES8388 Codec Manager
 * M5CoreNR (ESP32 / M5Stack Core2 + Module Audio)
 */

#ifndef AUDIO_PIPELINE_H
#define AUDIO_PIPELINE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "Config.h"

#ifdef __cplusplus
#include "audio_i2c.hpp"
#include "es8388.hpp"
extern "C" {
#endif

// --- Lifecycle ---
bool AudioPipeline_Init(void);
void AudioPipeline_Start(void);
void AudioPipeline_Stop(void);

// --- Input Source (Line In ADC vs Bluetooth RX) ---
// AUDIO_IN_BT only changes what feeds the DSP engine. The I2S full-duplex loop keeps
// running either way because it is the ES8388 master clock and the block pace maker.
typedef enum {
    AUDIO_IN_LINE = 0,  // ES8388 Line In ADC (default)
    AUDIO_IN_BT   = 1   // Decoded Bluetooth A2DP audio
} AudioInputSource;

void AudioPipeline_SetInputSource(AudioInputSource source);
AudioInputSource AudioPipeline_GetInputSource(void);

// --- Bluetooth TX Fan-Out ---
// When enabled, the processed 48 kHz block that feeds the headphone DAC is also
// handed to the Bluetooth A2DP source. Headphone output is never interrupted.
void AudioPipeline_SetBtTxTap(bool enable);
bool AudioPipeline_GetBtTxTap(void);

// --- Volume & Mute Controls ---
void AudioPipeline_SetVolume(uint8_t volume);
uint8_t AudioPipeline_GetVolume(void);
void AudioPipeline_VolumeUp(void);
void AudioPipeline_VolumeDown(void);

void AudioPipeline_SetMute(bool mute);
bool AudioPipeline_GetMute(void);
void AudioPipeline_ToggleMute(void);

// --- Headphone Standard Mode (CTIA vs OMTP) ---
void AudioPipeline_SetHPMode(bool americanCTIA);
bool AudioPipeline_GetHPMode(void);
void AudioPipeline_ToggleHPMode(void);

// --- RGB LED Status Updater ---
void AudioPipeline_UpdateLEDs(void);

#ifdef __cplusplus
}
AudioI2c& AudioPipeline_GetDevice(void);
ES8388& AudioPipeline_GetCodec(void);
#endif

#endif // AUDIO_PIPELINE_H
