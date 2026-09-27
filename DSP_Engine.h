/*
 * DSP_Engine.h
 * Real-Time Audio DSP Processing Engine for Cassette Tape Audio
 * M5CoreNR (ESP32 240MHz + Hardware FPU)
 */

#ifndef DSP_ENGINE_H
#define DSP_ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "Config.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- Lifecycle & Processing ---
void DSP_Engine_Init(float sampleRate);
void DSP_Engine_Reset(void);
void DSP_Engine_ProcessStereo(const int16_t* inBuf, int16_t* outBuf, size_t numFrames);

// --- Mode Management ---
void DSP_Engine_SetMode(ProcessingMode mode);
ProcessingMode DSP_Engine_GetMode(void);
const char* DSP_Engine_GetModeName(ProcessingMode mode);

// --- Subsonic Filter Control ---
void DSP_Engine_SetSubsonic(bool enable);
bool DSP_Engine_GetSubsonic(void);

// --- Real-Time Telemetry for Display UI ---
void DSP_Engine_GetVULevels(float* outPeakL_dB, float* outPeakR_dB);
float DSP_Engine_GetDynamicCutoffHz(void);
float DSP_Engine_GetNoiseReduction_dB(void);
void  DSP_Engine_GetDeHissGains(float* outMid_dB, float* outAir_dB);
float DSP_Engine_GetDecoderReduction_dB(void);
float DSP_Engine_GetExciterHarmonicsPercent(void);
float DSP_Engine_GetExciterAirGain_dB(void);
float DSP_Engine_GetStereoWidthPercent(void);

// CPU Utilization Telemetry
void DSP_Engine_SetCPULoad(float loadPercent);
float DSP_Engine_GetCPULoad(void);

#ifdef __cplusplus
}
#endif

#endif // DSP_ENGINE_H
