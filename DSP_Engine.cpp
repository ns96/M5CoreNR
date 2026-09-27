/*
 * DSP_Engine.cpp
 * Real-Time Audio DSP Processing Engine for Cassette Tape Audio
 * M5CoreNR (ESP32 240MHz + Hardware FPU)
 */

#include "DSP_Engine.h"
#include <math.h>
#include <string.h>

// ============================================================================
// DSP Tuning Constants
// ============================================================================

// --- Envelope Attack/Release Time Constants (seconds) ---
// These are converted to per-block coefficients by
// UpdateBlockEnvelopeCoefficients(), so envelope behavior is independent of the
// I2S DMA block size (AUDIO_DMA_BUF_LEN). The values below are the exact
// equivalents of the original per-block constants at the default 512-frame
// block period (10.667 ms at 48 kHz), so the current tuning is preserved.
#define DSP_TC_DEHISS_ATT_S        0.0178f  // 17.8 ms  (was per-block 0.45)
#define DSP_TC_DEHISS_REL_S        0.261f   // 261 ms   (was per-block 0.04)
#define DSP_TC_DEHISS_GAIN_SLEW_S  0.0371f  // 37.1 ms  (was per-block 0.25)
#define DSP_TC_EXCITER_PRES_ATT_S  0.528f   // 528 ms   (was per-block 0.020)
#define DSP_TC_EXCITER_PRES_REL_S  5.33f    // 5.33 s   (was per-block 0.002)
#define DSP_TC_EXCITER_AIR_ATT_S   0.0178f  // 17.8 ms  (was per-block 0.45)
#define DSP_TC_EXCITER_AIR_REL_S   0.208f   // 208 ms   (was per-block 0.05)
#define DSP_TC_EXCITER_AIR_SLEW_S  0.0371f  // 37.1 ms  (was per-block 0.25)
#define DSP_TC_EXCITER_GAIN_SLEW_S 0.0371f  // 37.1 ms  (was per-block 0.25)
#define DSP_TC_DOLBYB_ATT_S        0.0154f  // 15.4 ms  (was per-block 0.50)
#define DSP_TC_DOLBYB_REL_S        0.128f   // 128 ms   (was per-block 0.08)
#define DSP_TC_DOLBYC_ATT_S        0.0154f  // 15.4 ms  (was per-block 0.50)
#define DSP_TC_DOLBYC_REL_S        0.128f   // 128 ms   (was per-block 0.08)
#define DSP_TC_DBX_ATT_S           0.0134f  // 13.4 ms  (was per-block 0.55)
#define DSP_TC_DBX_REL_S           0.128f   // 128 ms   (was per-block 0.08)
#define DSP_TC_DBX_GAIN_SLEW_S     0.0371f  // 37.1 ms  (was per-block 0.25)
#define DSP_TC_DC_OFFSET_S         0.0417f  // 41.7 ms  (per-sample DC tracker)

// --- DBX Type II Expander Tuning ---
#define DBX_REF_AMPLITUDE          0.25f    // 0 VU reference amplitude (unchanged)
#define DBX_THRESHOLD_DB           0.0f     // Expansion threshold, dB relative to 0 VU
#define DBX_GAIN_SLOPE             1.0f     // (ratio - 1) for a 1:2 downward expander
#define DBX_MAX_CUT_DB             30.0f    // Maximum downward expansion depth

// --- DSP Structures ---

// Biquad Filter (Direct Form II Transposed) for higher frequencies
struct Biquad {
    float b0, b1, b2, a1, a2;
    float s1, s2;

    void reset() {
        s1 = 0.0f;
        s2 = 0.0f;
    }

    inline float process(float in) {
        float out = b0 * in + s1;
        s1 = b1 * in - a1 * out + s2;
        s2 = b2 * in - a2 * out;
        if (fabsf(s1) < 1e-15f) s1 = 0.0f;
        if (fabsf(s2) < 1e-15f) s2 = 0.0f;
        return out;
    }

    void setBPF(float f0, float Q, float Fs) {
        float w0 = 2.0f * (float)M_PI * f0 / Fs;
        float alpha = sinf(w0) / (2.0f * Q);
        float cosw0 = cosf(w0);
        float a0 = 1.0f + alpha;

        b0 = alpha / a0;
        b1 = 0.0f;
        b2 = (-alpha) / a0;
        a1 = (-2.0f * cosw0) / a0;
        a2 = (1.0f - alpha) / a0;
    }

    void setHPF(float f0, float Q, float Fs) {
        float w0 = 2.0f * (float)M_PI * f0 / Fs;
        float alpha = sinf(w0) / (2.0f * Q);
        float cosw0 = cosf(w0);
        float a0 = 1.0f + alpha;

        b0 = ((1.0f + cosw0) / 2.0f) / a0;
        b1 = (-(1.0f + cosw0)) / a0;
        b2 = ((1.0f + cosw0) / 2.0f) / a0;
        a1 = (-2.0f * cosw0) / a0;
        a2 = (1.0f - alpha) / a0;
    }

    void setLPF(float f0, float Q, float Fs) {
        float w0 = 2.0f * (float)M_PI * f0 / Fs;
        float alpha = sinf(w0) / (2.0f * Q);
        float cosw0 = cosf(w0);
        float a0 = 1.0f + alpha;

        b0 = ((1.0f - cosw0) / 2.0f) / a0;
        b1 = (1.0f - cosw0) / a0;
        b2 = ((1.0f - cosw0) / 2.0f) / a0;
        a1 = (-2.0f * cosw0) / a0;
        a2 = (1.0f - alpha) / a0;
    }

    void setAllpass(float f0, float Q, float Fs) {
        float w0 = 2.0f * (float)M_PI * f0 / Fs;
        float alpha = sinf(w0) / (2.0f * Q);
        float cosw0 = cosf(w0);
        float a0 = 1.0f + alpha;

        b0 = (1.0f - alpha) / a0;
        b1 = (-2.0f * cosw0) / a0;
        b2 = (1.0f + alpha) / a0;
        a1 = (-2.0f * cosw0) / a0;
        a2 = (1.0f - alpha) / a0;
    }

    void setHighShelf(float f0, float gainDB, float Q, float Fs) {
        float A = powf(10.0f, gainDB / 40.0f);
        float w0 = 2.0f * (float)M_PI * f0 / Fs;
        float alpha = sinf(w0) / (2.0f * Q);
        float cosw0 = cosf(w0);
        float twoSqrtAAlpha = 2.0f * sqrtf(A) * alpha;

        float a0 = (A + 1.0f) - (A - 1.0f) * cosw0 + twoSqrtAAlpha;
        b0 = (A * ((A + 1.0f) + (A - 1.0f) * cosw0 + twoSqrtAAlpha)) / a0;
        b1 = (-2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosw0)) / a0;
        b2 = (A * ((A + 1.0f) + (A - 1.0f) * cosw0 - twoSqrtAAlpha)) / a0;
        a1 = (2.0f * ((A - 1.0f) - (A + 1.0f) * cosw0)) / a0;
        a2 = ((A + 1.0f) - (A - 1.0f) * cosw0 - twoSqrtAAlpha) / a0;
    }

    void setLowShelf(float f0, float gainDB, float Q, float Fs) {
        float A = powf(10.0f, gainDB / 40.0f);
        float w0 = 2.0f * (float)M_PI * f0 / Fs;
        float alpha = sinf(w0) / (2.0f * Q);
        float cosw0 = cosf(w0);
        float twoSqrtAAlpha = 2.0f * sqrtf(A) * alpha;

        float a0 = (A + 1.0f) + (A - 1.0f) * cosw0 + twoSqrtAAlpha;
        b0 = (A * ((A + 1.0f) - (A - 1.0f) * cosw0 + twoSqrtAAlpha)) / a0;
        b1 = (2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosw0)) / a0;
        b2 = (A * ((A + 1.0f) - (A - 1.0f) * cosw0 - twoSqrtAAlpha)) / a0;
        a1 = (-2.0f * ((A - 1.0f) + (A + 1.0f) * cosw0)) / a0;
        a2 = ((A + 1.0f) + (A - 1.0f) * cosw0 - twoSqrtAAlpha) / a0;
    }
};

// Linkwitz-Riley 4th-Order (LR4) Cascaded Butterworth Crossover Filters
struct LinkwitzRiley4_LP {
    Biquad lp1, lp2;
    void init(float f0, float Fs) {
        lp1.setLPF(f0, 0.70710678f, Fs);
        lp2.setLPF(f0, 0.70710678f, Fs);
        reset();
    }
    void reset() {
        lp1.reset();
        lp2.reset();
    }
    inline float process(float in) {
        return lp2.process(lp1.process(in));
    }
};

struct LinkwitzRiley4_HP {
    Biquad hp1, hp2;
    void init(float f0, float Fs) {
        hp1.setHPF(f0, 0.70710678f, Fs);
        hp2.setHPF(f0, 0.70710678f, Fs);
        reset();
    }
    void reset() {
        hp1.reset();
        hp2.reset();
    }
    inline float process(float in) {
        return hp2.process(hp1.process(in));
    }
};

// Subsonic DC / Motor Rumble Blocker (1st-order highpass, rock-solid stability)
struct SubsonicBlocker {
    float xPrev;
    float yPrev;
    float R; // Pole position (e.g. 0.9965f for ~26.7 Hz cutoff at 48kHz)

    void init(float fc, float Fs) {
        R = 1.0f - (2.0f * (float)M_PI * fc / Fs);
        if (R < 0.90f) R = 0.90f;
        if (R > 0.999f) R = 0.999f;
        reset();
    }

    void reset() {
        xPrev = 0.0f;
        yPrev = 0.0f;
    }

    inline float process(float x) {
        float y = x - xPrev + R * yPrev;
        xPrev = x;
        yPrev = y;
        if (fabsf(yPrev) < 1e-15f) yPrev = 0.0f;
        return y;
    }
};


// --- Global Engine State ---
static float g_SampleRate = SAMPLING_FREQ;
static ProcessingMode g_CurrentMode = MODE_DNR; // Boot directly into DNR
static bool g_SubsonicEnabled = true;

// Soft limiter policy: engaged only for modes that can produce gain > 0 dB
// (EXCITER). All other modes are unity-gain or attenuation-only, so the
// limiter is bypassed to keep the tape decoder paths mathematically linear.
static bool g_UseSoftLimiter = false;

// --- Per-Block Envelope Coefficients (derived from the DSP_TC_* constants) ---
// Recomputed by UpdateBlockEnvelopeCoefficients() whenever the block period
// changes, so no envelope time constant is tied to the DMA buffer size.
static float g_BlockSec = 0.0f;
static size_t g_CoefBlockFrames = 0;
static float g_CoefDeHissAtt = 0.0f;
static float g_CoefDeHissRel = 0.0f;
static float g_CoefDeHissGainSlew = 0.0f;
static float g_CoefExciterPresAtt = 0.0f;
static float g_CoefExciterPresRel = 0.0f;
static float g_CoefExciterAirAtt = 0.0f;
static float g_CoefExciterAirRel = 0.0f;
static float g_CoefExciterAirSlew = 0.0f;
static float g_CoefExciterGainSlew = 0.0f;
static float g_CoefDolbyBAtt = 0.0f;
static float g_CoefDolbyBRel = 0.0f;
static float g_CoefDolbyCAtt = 0.0f;
static float g_CoefDolbyCRel = 0.0f;
static float g_CoefDbxAtt = 0.0f;
static float g_CoefDbxRel = 0.0f;
static float g_CoefDbxGainSlew = 0.0f;
static float g_CoefDcOffset = 0.0f;

// Subsonic Motor Rumble Filters (25 Hz Stable Blocker)
static SubsonicBlocker g_SubsonicL, g_SubsonicR;

// De-Hiss 3-Band Linkwitz-Riley 4th-Order Crossover (Phase-Aligned)
// Crossover 1: 3000 Hz (Separates 0-3kHz Low/Mid fundamentals from High)
static LinkwitzRiley4_LP g_Lr4LowL, g_Lr4LowR;
static LinkwitzRiley4_HP g_Lr4HighL, g_Lr4HighR;
// Crossover 2: 7500 Hz (Separates 3-7.5kHz Presence from >7.5kHz Air/Hiss)
static LinkwitzRiley4_LP g_Lr4MidL, g_Lr4MidR;
static LinkwitzRiley4_HP g_Lr4AirL, g_Lr4AirR;
// 2nd-Order Allpass delay compensation on Low band (matches 7500Hz crossover phase shift)
static Biquad g_ApLowL, g_ApLowR;

// De-Hiss Envelopes & Dynamic Expansion Gains
static float g_DeHissMidEnv = 0.0f;
static float g_DeHissAirEnv = 0.0f;
static float g_DeHissMidGainCurrent = 1.0f;
static float g_DeHissAirGainCurrent = 1.0f;
static float g_DeHissMidGain_dB = 0.0f;
static float g_DeHissAirGain_dB = 0.0f;

// Psychoacoustic Harmonic Exciter & Tone Sculpting Filters
// Bandpass (2.0 kHz - 4.5 kHz, centered at 3.0 kHz) isolates upper-mid musical presence
static Biquad g_ExciterBpfL, g_ExciterBpfR;
// Highpass (> 4.2 kHz) isolates synthesized 2nd and 3rd order harmonics
static Biquad g_ExciterHpfL, g_ExciterHpfR;
// Highpass (> 7.0 kHz) detects actual high-frequency air energy for dynamic hiss expansion
static Biquad g_ExciterAirDetL, g_ExciterAirDetR;
// Analog Tape Warmth Low-Shelf (+2.0 dB at 120 Hz for rich bottom fullness)
static Biquad g_BassShelfL, g_BassShelfR;
// Air Sheen High-Shelf (+3.5 dB at 7.0 kHz for sparkling highs)
static Biquad g_AirShelfL, g_AirShelfR;

// Dolby B Playback Decoder (Sliding-Band High Shelf: up to -10 dB de-emphasis)
static Biquad g_DolbyBShelfL, g_DolbyBShelfR;
static Biquad g_DolbyBScL, g_DolbyBScR; // 2.5 kHz HPF detector
static float g_DolbyBEnv = 0.0f;
static float g_DolbyBGain_dB = -10.0f;
static float g_DolbyBLastGain_dB = -10.0f;

// Dolby C Playback Decoder (Dual-Stage Companding + Anti-Skew De-emphasis: up to -20 dB)
static Biquad g_DolbyCShelfL, g_DolbyCShelfR;
static Biquad g_DolbyCSkewL, g_DolbyCSkewR;
static Biquad g_DolbyCScL, g_DolbyCScR; // 2.0 kHz HPF detector
static float g_DolbyCEnv = 0.0f;
static float g_DolbyCGain_dB = -20.0f;
static float g_DolbyCLastGain_dB = -20.0f;

// DBX Type II Tape Decoder (1:2 Dynamic RMS Expander + Matched De-emphasis)
static Biquad g_DbxDeemphL, g_DbxDeemphR;
static float g_DbxEnvRms = 0.0f;
static float g_DbxGainSmooth = 0.0316f; // -30 dB linear resting gain
static float g_DbxExpansion_dB = -30.0f;

static float g_ExciterEnvL = 0.0f;
static float g_ExciterEnvR = 0.0f;
static float g_ExciterAirEnv = 0.0f;
static float g_ExciterActiveGain = 0.0f;
static float g_ExciterLastAirGain_dB = -12.0f;
static float g_ExciterHarmonicsPercent = 0.0f;
static float g_StereoWidthPercent = 100.0f;

// ADC DC Offset Tracking
static float g_DcOffsetL = 0.0f;
static float g_DcOffsetR = 0.0f;

// Real-Time Telemetry & Ballistic Filters
static float g_RmsSmoothL = 0.0f;
static float g_RmsSmoothR = 0.0f;
static float g_PeakL_dB = -60.0f;
static float g_PeakR_dB = -60.0f;
static float g_CPULoadPercent = 0.0f;

// Convert a continuous time constant into the equivalent one-pole coefficient
// for a discrete update period: alpha = 1 - exp(-period / tau).
static inline float BlockCoeff(float tauSec, float blockSec) {
    if (tauSec <= 0.0f) return 1.0f;
    return 1.0f - expf(-blockSec / tauSec);
}

// Recompute every per-block envelope coefficient from the DSP_TC_* time
// constants and the current block period. Also derives the per-sample DC
// tracker coefficient from the actual sample rate.
static void UpdateBlockEnvelopeCoefficients(float blockSeconds) {
    g_BlockSec = (blockSeconds > 0.0f) ? blockSeconds : (1.0f / g_SampleRate);

    g_CoefDeHissAtt       = BlockCoeff(DSP_TC_DEHISS_ATT_S, g_BlockSec);
    g_CoefDeHissRel       = BlockCoeff(DSP_TC_DEHISS_REL_S, g_BlockSec);
    g_CoefDeHissGainSlew  = BlockCoeff(DSP_TC_DEHISS_GAIN_SLEW_S, g_BlockSec);
    g_CoefExciterPresAtt  = BlockCoeff(DSP_TC_EXCITER_PRES_ATT_S, g_BlockSec);
    g_CoefExciterPresRel  = BlockCoeff(DSP_TC_EXCITER_PRES_REL_S, g_BlockSec);
    g_CoefExciterAirAtt   = BlockCoeff(DSP_TC_EXCITER_AIR_ATT_S, g_BlockSec);
    g_CoefExciterAirRel   = BlockCoeff(DSP_TC_EXCITER_AIR_REL_S, g_BlockSec);
    g_CoefExciterAirSlew  = BlockCoeff(DSP_TC_EXCITER_AIR_SLEW_S, g_BlockSec);
    g_CoefExciterGainSlew = BlockCoeff(DSP_TC_EXCITER_GAIN_SLEW_S, g_BlockSec);
    g_CoefDolbyBAtt       = BlockCoeff(DSP_TC_DOLBYB_ATT_S, g_BlockSec);
    g_CoefDolbyBRel       = BlockCoeff(DSP_TC_DOLBYB_REL_S, g_BlockSec);
    g_CoefDolbyCAtt       = BlockCoeff(DSP_TC_DOLBYC_ATT_S, g_BlockSec);
    g_CoefDolbyCRel       = BlockCoeff(DSP_TC_DOLBYC_REL_S, g_BlockSec);
    g_CoefDbxAtt          = BlockCoeff(DSP_TC_DBX_ATT_S, g_BlockSec);
    g_CoefDbxRel          = BlockCoeff(DSP_TC_DBX_REL_S, g_BlockSec);
    g_CoefDbxGainSlew     = BlockCoeff(DSP_TC_DBX_GAIN_SLEW_S, g_BlockSec);

    g_CoefDcOffset = (g_SampleRate > 0.0f)
                   ? (1.0f - expf(-1.0f / (DSP_TC_DC_OFFSET_S * g_SampleRate)))
                   : 0.0005f;
}

// --- Soft Limiter Policy ---
// The soft limiter exists only to tame the boost stages of the harmonic
// exciter. Every other mode is unity-gain or attenuation-only (the DBX
// expander gain is clamped to <= 0 dB), so the limiter is bypassed there to
// keep the tape decoder paths mathematically linear. The saturating int16
// conversion clamp remains as the final safety net.
static inline bool ModeNeedsSoftLimiter(ProcessingMode mode) {
    return (mode == MODE_EXCITER);
}

// --- Transparent Soft Limiter ---
static inline float SoftLimit(float in) {
    if (in > 0.90f) {
        return 0.90f + 0.09f * tanhf((in - 0.90f) / 0.09f);
    } else if (in < -0.90f) {
        return -0.90f + 0.09f * tanhf((in + 0.90f) / 0.09f);
    }
    return in;
}

// --- Public Engine Implementation ---

void DSP_Engine_Init(float sampleRate) {
    g_SampleRate = (sampleRate > 0.0f) ? sampleRate : SAMPLING_FREQ;

    // Derive per-block envelope coefficients from the configured DMA block
    // period and select the limiter policy for the current boot mode.
    UpdateBlockEnvelopeCoefficients((float)AUDIO_DMA_BUF_LEN / g_SampleRate);
    g_CoefBlockFrames = AUDIO_DMA_BUF_LEN;
    g_UseSoftLimiter = ModeNeedsSoftLimiter(g_CurrentMode);

    // 1. Initialize Subsonic 25 Hz Rumble Blockers
    g_SubsonicL.init(25.0f, g_SampleRate);
    g_SubsonicR.init(25.0f, g_SampleRate);

    // 2. Initialize De-Hiss 3-Band Linkwitz-Riley Crossovers
    g_Lr4LowL.init(3000.0f, g_SampleRate);
    g_Lr4LowR.init(3000.0f, g_SampleRate);
    g_Lr4HighL.init(3000.0f, g_SampleRate);
    g_Lr4HighR.init(3000.0f, g_SampleRate);

    g_Lr4MidL.init(7500.0f, g_SampleRate);
    g_Lr4MidR.init(7500.0f, g_SampleRate);
    g_Lr4AirL.init(7500.0f, g_SampleRate);
    g_Lr4AirR.init(7500.0f, g_SampleRate);

    g_ApLowL.setAllpass(7500.0f, 0.70710678f, g_SampleRate);
    g_ApLowR.setAllpass(7500.0f, 0.70710678f, g_SampleRate);
    g_ApLowL.reset();
    g_ApLowR.reset();

    g_DeHissMidEnv = 0.0f;
    g_DeHissAirEnv = 0.0f;
    g_DeHissMidGainCurrent = 0.398f; // -8 dB resting in silence
    g_DeHissAirGainCurrent = 0.126f; // -18 dB resting in silence
    g_DeHissMidGain_dB = -8.0f;
    g_DeHissAirGain_dB = -18.0f;

    // 3. Initialize Harmonic Exciter Filters
    // BPF: 3000 Hz, broad Q = 0.65 (musical presence band, zero impulse ringing)
    g_ExciterBpfL.setBPF(3000.0f, 0.65f, g_SampleRate);
    g_ExciterBpfR.setBPF(3000.0f, 0.65f, g_SampleRate);

    // HPF: 4800 Hz, Q = 0.7071 (extracts synthesized harmonics above 4.8 kHz)
    g_ExciterHpfL.setHPF(4800.0f, 0.7071f, g_SampleRate);
    g_ExciterHpfR.setHPF(4800.0f, 0.7071f, g_SampleRate);

    // Air Detector HPF: 7000 Hz (detects real HF content for dynamic hiss expansion)
    g_ExciterAirDetL.setHPF(7000.0f, 0.7071f, g_SampleRate);
    g_ExciterAirDetR.setHPF(7000.0f, 0.7071f, g_SampleRate);

    // Bass Warmth Low-Shelf (+3.5 dB at 100 Hz for rich analog body, punch, and fullness)
    g_BassShelfL.setLowShelf(100.0f, 3.5f, 0.7071f, g_SampleRate);
    g_BassShelfR.setLowShelf(100.0f, 3.5f, 0.7071f, g_SampleRate);

    // 4. Initialize Air Sheen High-Shelf (-12.0 dB resting in silence/hiss, up to +3.5 dB on music)
    g_AirShelfL.setHighShelf(6800.0f, -12.0f, 0.7071f, g_SampleRate);
    g_AirShelfR.setHighShelf(6800.0f, -12.0f, 0.7071f, g_SampleRate);
    g_ExciterLastAirGain_dB = -12.0f;

    // 5. Initialize Dolby B Playback Decoder (1.8 kHz High-Shelf, starts at -10 dB de-emphasis)
    g_DolbyBShelfL.setHighShelf(1800.0f, -10.0f, 0.7071f, g_SampleRate);
    g_DolbyBShelfR.setHighShelf(1800.0f, -10.0f, 0.7071f, g_SampleRate);
    g_DolbyBScL.setHPF(2500.0f, 0.7071f, g_SampleRate);
    g_DolbyBScR.setHPF(2500.0f, 0.7071f, g_SampleRate);

    // 6. Initialize Dolby C Playback Decoder (1.5 kHz High-Shelf, starts at -20 dB; 8.5 kHz Anti-Skew)
    g_DolbyCShelfL.setHighShelf(1500.0f, -20.0f, 0.7071f, g_SampleRate);
    g_DolbyCShelfR.setHighShelf(1500.0f, -20.0f, 0.7071f, g_SampleRate);
    g_DolbyCSkewL.setHighShelf(8500.0f, -3.5f, 0.7071f, g_SampleRate);
    g_DolbyCSkewR.setHighShelf(8500.0f, -3.5f, 0.7071f, g_SampleRate);
    g_DolbyCScL.setHPF(2000.0f, 0.7071f, g_SampleRate);
    g_DolbyCScR.setHPF(2000.0f, 0.7071f, g_SampleRate);

    // 7. Initialize DBX Type II De-emphasis (2.5 kHz High-Shelf, -6 dB tilt)
    g_DbxDeemphL.setHighShelf(2500.0f, -6.0f, 0.7071f, g_SampleRate);
    g_DbxDeemphR.setHighShelf(2500.0f, -6.0f, 0.7071f, g_SampleRate);

    DSP_Engine_Reset();
}

void DSP_Engine_Reset(void) {
    g_SubsonicL.reset();
    g_SubsonicR.reset();

    g_Lr4LowL.reset();  g_Lr4LowR.reset();
    g_Lr4HighL.reset(); g_Lr4HighR.reset();
    g_Lr4MidL.reset();  g_Lr4MidR.reset();
    g_Lr4AirL.reset();  g_Lr4AirR.reset();
    g_ApLowL.reset();   g_ApLowR.reset();

    g_ExciterBpfL.reset();
    g_ExciterBpfR.reset();
    g_ExciterHpfL.reset();
    g_ExciterHpfR.reset();
    g_ExciterAirDetL.reset();
    g_ExciterAirDetR.reset();
    g_BassShelfL.reset();
    g_BassShelfR.reset();
    g_AirShelfL.reset();
    g_AirShelfR.reset();
    g_AirShelfL.setHighShelf(6800.0f, -12.0f, 0.7071f, g_SampleRate);
    g_AirShelfR.setHighShelf(6800.0f, -12.0f, 0.7071f, g_SampleRate);
    g_ExciterLastAirGain_dB = -12.0f;
    g_ExciterEnvL = 0.0f;
    g_ExciterEnvR = 0.0f;
    g_ExciterAirEnv = 0.0f;
    g_ExciterActiveGain = 0.0f;
    g_ExciterHarmonicsPercent = 0.0f;

    g_DolbyBShelfL.reset();
    g_DolbyBShelfR.reset();
    g_DolbyBScL.reset();
    g_DolbyBScR.reset();

    g_DolbyCShelfL.reset();
    g_DolbyCShelfR.reset();
    g_DolbyCSkewL.reset();
    g_DolbyCSkewR.reset();
    g_DolbyCScL.reset();
    g_DolbyCScR.reset();

    g_DbxDeemphL.reset();
    g_DbxDeemphR.reset();

    g_DeHissMidEnv = 0.0f;
    g_DeHissAirEnv = 0.0f;
    g_DeHissMidGainCurrent = 0.398f;
    g_DeHissAirGainCurrent = 0.126f;
    g_DeHissMidGain_dB = -8.0f;
    g_DeHissAirGain_dB = -18.0f;

    g_DolbyBEnv = 0.0f;
    g_DolbyBGain_dB = -10.0f;
    g_DolbyBLastGain_dB = -10.0f;

    g_DolbyCEnv = 0.0f;
    g_DolbyCGain_dB = -20.0f;
    g_DolbyCLastGain_dB = -20.0f;

    g_DbxEnvRms = 0.0f;
    g_DbxGainSmooth = 0.0316f;
    g_DbxExpansion_dB = -30.0f;

    g_ExciterEnvL = 0.0f;
    g_ExciterEnvR = 0.0f;
    g_ExciterActiveGain = 0.0f;
    g_ExciterHarmonicsPercent = 0.0f;
    g_StereoWidthPercent = 100.0f;

    g_DcOffsetL = 0.0f;
    g_DcOffsetR = 0.0f;
    g_RmsSmoothL = 0.0f;
    g_RmsSmoothR = 0.0f;
    g_PeakL_dB = -60.0f;
    g_PeakR_dB = -60.0f;
}

void DSP_Engine_SetMode(ProcessingMode mode) {
    if (g_CurrentMode != mode) {
        g_CurrentMode = mode;
        g_UseSoftLimiter = ModeNeedsSoftLimiter(mode);
        if (mode == MODE_DNR) {
            g_Lr4LowL.reset();  g_Lr4LowR.reset();
            g_Lr4HighL.reset(); g_Lr4HighR.reset();
            g_Lr4MidL.reset();  g_Lr4MidR.reset();
            g_Lr4AirL.reset();  g_Lr4AirR.reset();
            g_ApLowL.reset();   g_ApLowR.reset();
            g_DeHissMidEnv = 0.0f;
            g_DeHissAirEnv = 0.0f;
            g_DeHissMidGainCurrent = 0.398f;
            g_DeHissAirGainCurrent = 0.126f;
            g_DeHissMidGain_dB = -8.0f;
            g_DeHissAirGain_dB = -18.0f;
        }
        if (mode == MODE_EXCITER) {
            g_ExciterBpfL.reset();
            g_ExciterBpfR.reset();
            g_ExciterHpfL.reset();
            g_ExciterHpfR.reset();
            g_ExciterAirDetL.reset();
            g_ExciterAirDetR.reset();
            g_BassShelfL.reset();
            g_BassShelfR.reset();
            g_AirShelfL.reset();
            g_AirShelfR.reset();
            g_AirShelfL.setHighShelf(6800.0f, -12.0f, 0.7071f, g_SampleRate);
            g_AirShelfR.setHighShelf(6800.0f, -12.0f, 0.7071f, g_SampleRate);
            g_ExciterEnvL = 0.0f;
            g_ExciterEnvR = 0.0f;
            g_ExciterAirEnv = 0.0f;
            g_ExciterActiveGain = 0.0f;
            g_ExciterLastAirGain_dB = -12.0f;
        }
        if (mode == MODE_DOLBY_B) {
            g_DolbyBShelfL.reset();
            g_DolbyBShelfR.reset();
            g_DolbyBScL.reset();
            g_DolbyBScR.reset();
            g_DolbyBEnv = 0.0f;
            g_DolbyBGain_dB = -10.0f;
            g_DolbyBLastGain_dB = -10.0f;
            g_DolbyBShelfL.setHighShelf(1800.0f, -10.0f, 0.7071f, g_SampleRate);
            g_DolbyBShelfR.setHighShelf(1800.0f, -10.0f, 0.7071f, g_SampleRate);
        }
        if (mode == MODE_DOLBY_C) {
            g_DolbyCShelfL.reset();
            g_DolbyCShelfR.reset();
            g_DolbyCSkewL.reset();
            g_DolbyCSkewR.reset();
            g_DolbyCScL.reset();
            g_DolbyCScR.reset();
            g_DolbyCEnv = 0.0f;
            g_DolbyCGain_dB = -20.0f;
            g_DolbyCLastGain_dB = -20.0f;
            g_DolbyCShelfL.setHighShelf(1500.0f, -20.0f, 0.7071f, g_SampleRate);
            g_DolbyCShelfR.setHighShelf(1500.0f, -20.0f, 0.7071f, g_SampleRate);
        }
        if (mode == MODE_DBX) {
            g_DbxDeemphL.reset();
            g_DbxDeemphR.reset();
            g_DbxEnvRms = 0.0f;
            g_DbxGainSmooth = 0.0316f;
            g_DbxExpansion_dB = -30.0f;
        }
    }
}

ProcessingMode DSP_Engine_GetMode(void) {
    return g_CurrentMode;
}

const char* DSP_Engine_GetModeName(ProcessingMode mode) {
    switch (mode) {
        case MODE_BYPASS:   return "BYPASS (Raw Tape)";
        case MODE_DNR:      return "DE-HISS (3-Band LR4)";
        case MODE_EXCITER:  return "EXCITER (Air+Sheen)";
        case MODE_DOLBY_B:  return "DOLBY-B (-10dB Decode)";
        case MODE_DOLBY_C:  return "DOLBY-C (-20dB Dual)";
        case MODE_DBX:      return "DBX TYPE-II (2:1 Expand)";
        default:            return "UNKNOWN";
    }
}

void DSP_Engine_SetSubsonic(bool enable) {
    g_SubsonicEnabled = enable;
}

bool DSP_Engine_GetSubsonic(void) {
    return g_SubsonicEnabled;
}

void DSP_Engine_GetVULevels(float* outPeakL_dB, float* outPeakR_dB) {
    if (outPeakL_dB) *outPeakL_dB = g_PeakL_dB;
    if (outPeakR_dB) *outPeakR_dB = g_PeakR_dB;
}

float DSP_Engine_GetDynamicCutoffHz(void) {
    return (g_DeHissAirGainCurrent >= 0.85f) ? 20000.0f : 7500.0f;
}

float DSP_Engine_GetNoiseReduction_dB(void) {
    return g_DeHissAirGain_dB;
}

void DSP_Engine_GetDeHissGains(float* outMid_dB, float* outAir_dB) {
    if (outMid_dB) *outMid_dB = g_DeHissMidGain_dB;
    if (outAir_dB) *outAir_dB = g_DeHissAirGain_dB;
}

float DSP_Engine_GetDecoderReduction_dB(void) {
    if (g_CurrentMode == MODE_DOLBY_B) return g_DolbyBGain_dB;
    if (g_CurrentMode == MODE_DOLBY_C) return g_DolbyCGain_dB;
    if (g_CurrentMode == MODE_DBX)     return g_DbxExpansion_dB;
    if (g_CurrentMode == MODE_DNR)     return g_DeHissAirGain_dB;
    return 0.0f;
}

float DSP_Engine_GetExciterHarmonicsPercent(void) {
    return g_ExciterHarmonicsPercent;
}

float DSP_Engine_GetExciterAirGain_dB(void) {
    return g_ExciterLastAirGain_dB;
}

float DSP_Engine_GetStereoWidthPercent(void) {
    return g_StereoWidthPercent;
}

void DSP_Engine_SetCPULoad(float loadPercent) {
    g_CPULoadPercent = loadPercent;
}

float DSP_Engine_GetCPULoad(void) {
    return g_CPULoadPercent;
}

// --- Main Audio Processing Block (Full 48 kHz Stereo, Zero Decimation) ---
void DSP_Engine_ProcessStereo(const int16_t* inBuf, int16_t* outBuf, size_t numFrames) {
    if (!inBuf || !outBuf || numFrames == 0) return;

    // Keep envelope time constants correct if the DMA block size ever changes
    if (numFrames != g_CoefBlockFrames) {
        UpdateBlockEnvelopeCoefficients((float)numFrames / g_SampleRate);
        g_CoefBlockFrames = numFrames;
    }

    // --- FIX: BYPASS MODE IS 100% BIT-PERFECT DIRECT COPY ---
    if (g_CurrentMode == MODE_BYPASS) {
        memcpy(outBuf, inBuf, numFrames * 2 * sizeof(int16_t));

        // RMS Energy Tracking with Dynamic DC Removal
        float sumSqL = 0.0f;
        float sumSqR = 0.0f;
        for (size_t i = 0; i < numFrames; i++) {
            float sL = (float)inBuf[i * 2]     / 32768.0f;
            float sR = (float)inBuf[i * 2 + 1] / 32768.0f;

            // Track and remove DC offset
            g_DcOffsetL += g_CoefDcOffset * (sL - g_DcOffsetL);
            g_DcOffsetR += g_CoefDcOffset * (sR - g_DcOffsetR);

            float acL = sL - g_DcOffsetL;
            float acR = sR - g_DcOffsetR;

            sumSqL += acL * acL;
            sumSqR += acR * acR;
        }

        float rmsL = sqrtf(sumSqL / (float)numFrames);
        float rmsR = sqrtf(sumSqR / (float)numFrames);

        // Smooth with VU ballistics (responsive attack, smooth analog decay)
        g_RmsSmoothL = (rmsL > g_RmsSmoothL) ? (g_RmsSmoothL * 0.40f + rmsL * 0.60f)
                                            : (g_RmsSmoothL * 0.85f + rmsL * 0.15f);
        g_RmsSmoothR = (rmsR > g_RmsSmoothR) ? (g_RmsSmoothR * 0.40f + rmsR * 0.60f)
                                            : (g_RmsSmoothR * 0.85f + rmsR * 0.15f);

        // Silence squelch gate: below ~ -38 dB (0.012f RMS) locks meters motionless at -60 dB
        const float SILENCE_GATE_RMS = 0.012f;

        if (g_RmsSmoothL < SILENCE_GATE_RMS) {
            g_PeakL_dB = -60.0f;
        } else {
            float dbL = 20.0f * log10f(g_RmsSmoothL * 1.4142f);
            if (dbL < -35.0f) dbL = -60.0f;
            if (dbL > 3.0f)   dbL = 3.0f;
            g_PeakL_dB = dbL;
        }

        if (g_RmsSmoothR < SILENCE_GATE_RMS) {
            g_PeakR_dB = -60.0f;
        } else {
            float dbR = 20.0f * log10f(g_RmsSmoothR * 1.4142f);
            if (dbR < -35.0f) dbR = -60.0f;
            if (dbR > 3.0f)   dbR = 3.0f;
            g_PeakR_dB = dbR;
        }

        g_DeHissMidGainCurrent = 1.0f;
        g_DeHissAirGainCurrent = 1.0f;
        g_DeHissMidGain_dB = 0.0f;
        g_DeHissAirGain_dB = 0.0f;
        g_ExciterHarmonicsPercent = 0.0f;
        g_StereoWidthPercent = 100.0f;
        return;
    }

    float sumSqL = 0.0f;
    float sumSqR = 0.0f;
    float blockMidSum = 0.0f;
    float blockAirSum = 0.0f;
    float blockDolbySum = 0.0f;
    float blockDbxSumSq = 0.0f;

    // Snapshot the limiter policy once per block for predictable branching
    const bool useLimiter = g_UseSoftLimiter;

    for (size_t i = 0; i < numFrames; i++) {
        // Convert 16-bit PCM to normalized float [-1.0, +1.0]
        float sL = (float)inBuf[i * 2]     / 32768.0f;
        float sR = (float)inBuf[i * 2 + 1] / 32768.0f;

        // 1. Subsonic 25 Hz High-Pass Blocker (Isolates motor rumble/DC)
        if (g_SubsonicEnabled) {
            sL = g_SubsonicL.process(sL);
            sR = g_SubsonicR.process(sR);
        } else {
            g_DcOffsetL += g_CoefDcOffset * (sL - g_DcOffsetL);
            g_DcOffsetR += g_CoefDcOffset * (sR - g_DcOffsetR);
            sL -= g_DcOffsetL;
            sR -= g_DcOffsetR;
        }

        // 2. Mode Processing
        if (g_CurrentMode == MODE_DNR) {
            // De-Hiss 3-Band Linkwitz-Riley 4th-Order Split (Phase-Aligned):
            // Crossover 1 at 3000 Hz: Low Band (0-3kHz) and High Band (>3kHz)
            float lowL  = g_ApLowL.process(g_Lr4LowL.process(sL));
            float lowR  = g_ApLowR.process(g_Lr4LowR.process(sR));
            float highL = g_Lr4HighL.process(sL);
            float highR = g_Lr4HighR.process(sR);

            // Crossover 2 at 7500 Hz: Mid Band (3-7.5kHz) and Air Band (>7.5kHz)
            float midL  = g_Lr4MidL.process(highL);
            float midR  = g_Lr4MidR.process(highR);
            float airL  = g_Lr4AirL.process(highL);
            float airR  = g_Lr4AirR.process(highR);

            blockMidSum += (fabsf(midL) + fabsf(midR)) * 0.5f;
            blockAirSum += (fabsf(airL) + fabsf(airR)) * 0.5f;

            // Reconstruct: Band 1 (0-3000 Hz) is 100% UNTOUCHED BIT-CLEAN!
            sL = lowL + g_DeHissMidGainCurrent * midL + g_DeHissAirGainCurrent * airL;
            sR = lowR + g_DeHissMidGainCurrent * midR + g_DeHissAirGainCurrent * airR;
        }
        else if (g_CurrentMode == MODE_EXCITER) {
            // Step A: Bandpass isolate presence band (3.0 kHz, broad Q=0.65 - zero ringing)
            float bpfL = g_ExciterBpfL.process(sL);
            float bpfR = g_ExciterBpfR.process(sR);

            // Step B: Air detector (> 7.0 kHz) accumulates energy for dynamic hiss expansion
            float airDetL = g_ExciterAirDetL.process(sL);
            float airDetR = g_ExciterAirDetR.process(sR);
            blockAirSum += (fabsf(airDetL) + fabsf(airDetR)) * 0.5f;

            // Musical presence envelope follower
            float absL = fabsf(bpfL);
            float absR = fabsf(bpfR);
            g_ExciterEnvL += (absL > g_ExciterEnvL ? g_CoefExciterPresAtt : g_CoefExciterPresRel) * (absL - g_ExciterEnvL);
            g_ExciterEnvR += (absR > g_ExciterEnvR ? g_CoefExciterPresAtt : g_CoefExciterPresRel) * (absR - g_ExciterEnvR);

            float injectL = 0.0f;
            float injectR = 0.0f;
            if (g_ExciterActiveGain > 0.02f) {
                // Left Channel: Smooth non-folding rational soft saturation
                float normL = bpfL / (g_ExciterEnvL + 0.035f);
                float satL = normL / (1.0f + 0.5f * fabsf(normL));
                float harmL = 0.60f * satL + 0.40f * (satL * fabsf(satL));
                float sparkleL = g_ExciterHpfL.process(harmL);

                // Silky dynamic injection (~18% drive, clamped against clipping)
                injectL = sparkleL * (0.18f * g_ExciterActiveGain);
                if (injectL > 0.22f) injectL = 0.22f; else if (injectL < -0.22f) injectL = -0.22f;

                // Right Channel: Smooth non-folding rational soft saturation
                float normR = bpfR / (g_ExciterEnvR + 0.035f);
                float satR = normR / (1.0f + 0.5f * fabsf(normR));
                float harmR = 0.60f * satR + 0.40f * (satR * fabsf(satR));
                float sparkleR = g_ExciterHpfR.process(harmR);

                injectR = sparkleR * (0.18f * g_ExciterActiveGain);
                if (injectR > 0.22f) injectR = 0.22f; else if (injectR < -0.22f) injectR = -0.22f;
            }

            // Step C: Analog Low-End Tape Warmth (+3.5 dB at 100 Hz for rich body, punch & fullness)
            sL = g_BassShelfL.process(sL);
            sR = g_BassShelfR.process(sR);

            // Step D: Dynamic Air Expander / De-Hiss (-12 dB in silence / hiss-only, up to +3.5 dB on real HF music)
            sL = g_AirShelfL.process(sL);
            sR = g_AirShelfR.process(sR);

            // Step E: Inject clean synthesized harmonics (noise-free, synthesized from midrange presence)
            sL += injectL;
            sR += injectR;
        }
        else if (g_CurrentMode == MODE_DOLBY_B) {
            // Dolby B: Detect high-frequency energy & apply sliding-band high-shelf de-emphasis
            float scL = fabsf(g_DolbyBScL.process(sL));
            float scR = fabsf(g_DolbyBScR.process(sR));
            blockDolbySum += (scL + scR) * 0.5f;

            sL = g_DolbyBShelfL.process(sL);
            sR = g_DolbyBShelfR.process(sR);
        }
        else if (g_CurrentMode == MODE_DOLBY_C) {
            // Dolby C: Anti-spectral skew + dual-stage de-emphasis
            float scL = fabsf(g_DolbyCScL.process(sL));
            float scR = fabsf(g_DolbyCScR.process(sR));
            blockDolbySum += (scL + scR) * 0.5f;

            sL = g_DolbyCSkewL.process(sL);
            sR = g_DolbyCSkewR.process(sR);
            sL = g_DolbyCShelfL.process(sL);
            sR = g_DolbyCShelfR.process(sR);
        }
        else if (g_CurrentMode == MODE_DBX) {
            // DBX Type II: RMS detection, matched de-emphasis & 1:2 downward expansion
            blockDbxSumSq += 0.5f * (sL * sL + sR * sR);

            sL = g_DbxDeemphL.process(sL) * g_DbxGainSmooth;
            sR = g_DbxDeemphR.process(sR) * g_DbxGainSmooth;
        }

        // Master Transparent Soft Limiter (EXCITER only - the sole mode that can
        // produce gain > 0 dB). All other modes stay linear; the saturating
        // int16 clamp below is the final safety net.
        if (useLimiter) {
            sL = SoftLimit(sL);
            sR = SoftLimit(sR);
        }

        // RMS Energy Tracking for VU meters
        sumSqL += sL * sL;
        sumSqR += sR * sR;

        // Convert back to 16-bit signed PCM
        int32_t outIntL = (int32_t)(sL * 32767.0f);
        int32_t outIntR = (int32_t)(sR * 32767.0f);

        if (outIntL > 32767) outIntL = 32767;
        else if (outIntL < -32768) outIntL = -32768;

        if (outIntR > 32767) outIntR = 32767;
        else if (outIntR < -32768) outIntR = -32768;

        outBuf[i * 2]     = (int16_t)outIntL;
        outBuf[i * 2 + 1] = (int16_t)outIntR;
    }

    // --- Post-Block Parameter Updates (Smooth and CPU-efficient) ---
    // 1. De-Hiss 3-Band Downward Expander (Active in MODE_DNR)
    if (g_CurrentMode == MODE_DNR) {
        float avgMid = (blockMidSum * 0.5f) / (float)numFrames;
        float avgAir = (blockAirSum * 0.5f) / (float)numFrames;

        // Attack/release coefficients derived from DSP_TC_DEHISS_* time constants
        g_DeHissMidEnv += (avgMid > g_DeHissMidEnv ? g_CoefDeHissAtt : g_CoefDeHissRel) * (avgMid - g_DeHissMidEnv);
        g_DeHissAirEnv += (avgAir > g_DeHissAirEnv ? g_CoefDeHissAtt : g_CoefDeHissRel) * (avgAir - g_DeHissAirEnv);

        // Calibrated thresholds for tape audio:
        // Band 3 (Air > 7.5 kHz): -18 dB tape hiss cut in silence
        const float noiseFloorAir = 0.005f;
        const float fullMusicAir  = 0.035f;
        float targetAirGain_dB = 0.0f;

        if (g_DeHissAirEnv <= noiseFloorAir) {
            targetAirGain_dB = -18.0f;
        } else if (g_DeHissAirEnv >= fullMusicAir) {
            targetAirGain_dB = 0.0f;
        } else {
            float t = (g_DeHissAirEnv - noiseFloorAir) / (fullMusicAir - noiseFloorAir);
            targetAirGain_dB = -18.0f * (1.0f - t);
        }

        // Apply Air band downward expansion gain to g_DeHissAirGainCurrent
        float targetAirLinear = powf(10.0f, targetAirGain_dB / 20.0f);
        g_DeHissAirGainCurrent += g_CoefDeHissGainSlew * (targetAirLinear - g_DeHissAirGainCurrent);
        g_DeHissAirGain_dB = 20.0f * log10f(g_DeHissAirGainCurrent + 1e-6f);

        // Band 2 (Mid 3.0-7.5 kHz): gentle -8 dB expansion in silence for DE-HISS
        const float noiseFloorMid = 0.008f;
        const float fullMusicMid  = 0.040f;
        float targetMidGain_dB = 0.0f;

        if (g_DeHissMidEnv <= noiseFloorMid) {
            targetMidGain_dB = -8.0f;
        } else if (g_DeHissMidEnv >= fullMusicMid) {
            targetMidGain_dB = 0.0f;
        } else {
            float t = (g_DeHissMidEnv - noiseFloorMid) / (fullMusicMid - noiseFloorMid);
            targetMidGain_dB = -8.0f * (1.0f - t);
        }

        float targetMidLinear = powf(10.0f, targetMidGain_dB / 20.0f);
        g_DeHissMidGainCurrent += g_CoefDeHissGainSlew * (targetMidLinear - g_DeHissMidGainCurrent);
        g_DeHissMidGain_dB = 20.0f * log10f(g_DeHissMidGainCurrent + 1e-6f);

        g_ExciterActiveGain = 0.0f;
        g_ExciterHarmonicsPercent = 0.0f;
        g_StereoWidthPercent = 100.0f;
    }
    // 2. Exciter Parameter Update (Active in MODE_EXCITER)
    else if (g_CurrentMode == MODE_EXCITER) {
        // Presence detection for harmonic drive
        float musicLevel = (g_ExciterEnvL + g_ExciterEnvR) * 0.5f;
        const float noiseFloorExciter = 0.006f; // -44 dBFS (prevents tape hiss from driving saturation)
        const float fullMusicExciter  = 0.030f; // -30 dBFS

        float exciterTarget = 0.0f;
        if (musicLevel > noiseFloorExciter) {
            exciterTarget = (musicLevel - noiseFloorExciter) / (fullMusicExciter - noiseFloorExciter);
            if (exciterTarget > 1.0f) exciterTarget = 1.0f;
            if (exciterTarget < 0.0f) exciterTarget = 0.0f;
        }
        g_ExciterActiveGain += g_CoefExciterGainSlew * (exciterTarget - g_ExciterActiveGain);
        g_ExciterHarmonicsPercent = g_ExciterActiveGain * 100.0f;

        // Dynamic Air Band Expander (De-Hiss):
        // Follow high-frequency energy (>7.0 kHz)
        float avgAir = blockAirSum / (float)numFrames;
        // Attack/release coefficients derived from DSP_TC_EXCITER_AIR_* time constants
        g_ExciterAirEnv += (avgAir > g_ExciterAirEnv ? g_CoefExciterAirAtt : g_CoefExciterAirRel) * (avgAir - g_ExciterAirEnv);

        // Calibrated thresholds for > 7.0 kHz tape hiss vs real musical treble:
        const float noiseFloorAir = 0.004f; // Tape hiss floor (~ -48 dBFS)
        const float fullMusicAir  = 0.025f; // Active musical treble (~ -32 dBFS)

        float targetAirGain_dB = -12.0f;
        if (g_ExciterAirEnv <= noiseFloorAir) {
            targetAirGain_dB = -12.0f; // Silence / hiss-only: suppress hiss by -12 dB
        } else if (g_ExciterAirEnv >= fullMusicAir) {
            targetAirGain_dB = 3.5f;   // Strong musical treble: full air sheen (+3.5 dB)
        } else {
            float t = (g_ExciterAirEnv - noiseFloorAir) / (fullMusicAir - noiseFloorAir);
            targetAirGain_dB = -12.0f + 15.5f * t; // Linear dB fade from -12 dB to +3.5 dB
        }

        // Smooth gain transition to avoid zipper noise
        float exciterAirGain_dB = g_ExciterLastAirGain_dB + g_CoefExciterAirSlew * (targetAirGain_dB - g_ExciterLastAirGain_dB);

        if (fabsf(exciterAirGain_dB - g_ExciterLastAirGain_dB) >= 0.4f) {
            g_AirShelfL.setHighShelf(6800.0f, exciterAirGain_dB, 0.7071f, g_SampleRate);
            g_AirShelfR.setHighShelf(6800.0f, exciterAirGain_dB, 0.7071f, g_SampleRate);
            g_ExciterLastAirGain_dB = exciterAirGain_dB;
        }

        g_DeHissMidGainCurrent = 1.0f;
        g_DeHissAirGainCurrent = powf(10.0f, exciterAirGain_dB / 20.0f);
        g_DeHissMidGain_dB = 0.0f;
        g_DeHissAirGain_dB = exciterAirGain_dB;
        g_StereoWidthPercent = 100.0f;
    }
    // 2. Dolby B Parameter Update
    else if (g_CurrentMode == MODE_DOLBY_B) {
        float avgSc = blockDolbySum / (float)numFrames;
        g_DolbyBEnv += (avgSc > g_DolbyBEnv ? g_CoefDolbyBAtt : g_CoefDolbyBRel) * (avgSc - g_DolbyBEnv);

        float t = (g_DolbyBEnv - 0.008f) / (0.120f - 0.008f);
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        g_DolbyBGain_dB = -10.0f * (1.0f - t);

        if (fabsf(g_DolbyBGain_dB - g_DolbyBLastGain_dB) >= 0.4f) {
            g_DolbyBShelfL.setHighShelf(1800.0f, g_DolbyBGain_dB, 0.7071f, g_SampleRate);
            g_DolbyBShelfR.setHighShelf(1800.0f, g_DolbyBGain_dB, 0.7071f, g_SampleRate);
            g_DolbyBLastGain_dB = g_DolbyBGain_dB;
        }
        g_DeHissMidGainCurrent = 1.0f;
        g_DeHissAirGainCurrent = 1.0f;
        g_DeHissMidGain_dB = 0.0f;
        g_DeHissAirGain_dB = g_DolbyBGain_dB;
        g_ExciterActiveGain = 0.0f;
        g_ExciterHarmonicsPercent = 0.0f;
        g_StereoWidthPercent = 100.0f;
    }
    // 3. Dolby C Parameter Update
    else if (g_CurrentMode == MODE_DOLBY_C) {
        float avgSc = blockDolbySum / (float)numFrames;
        g_DolbyCEnv += (avgSc > g_DolbyCEnv ? g_CoefDolbyCAtt : g_CoefDolbyCRel) * (avgSc - g_DolbyCEnv);

        float t = (g_DolbyCEnv - 0.006f) / (0.140f - 0.006f);
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        g_DolbyCGain_dB = -20.0f * (1.0f - t);

        if (fabsf(g_DolbyCGain_dB - g_DolbyCLastGain_dB) >= 0.5f) {
            g_DolbyCShelfL.setHighShelf(1500.0f, g_DolbyCGain_dB, 0.7071f, g_SampleRate);
            g_DolbyCShelfR.setHighShelf(1500.0f, g_DolbyCGain_dB, 0.7071f, g_SampleRate);
            g_DolbyCLastGain_dB = g_DolbyCGain_dB;
        }
        g_DeHissMidGainCurrent = 1.0f;
        g_DeHissAirGainCurrent = 1.0f;
        g_DeHissMidGain_dB = 0.0f;
        g_DeHissAirGain_dB = g_DolbyCGain_dB;
        g_ExciterActiveGain = 0.0f;
        g_ExciterHarmonicsPercent = 0.0f;
        g_StereoWidthPercent = 100.0f;
    }
    // 4. DBX Type II Parameter Update (decibel-linear 1:2 downward expander)
    else if (g_CurrentMode == MODE_DBX) {
        // Sidechain: block RMS of the still pre-emphasised (encoded) signal
        float blockRms = sqrtf(blockDbxSumSq / (float)numFrames);
        g_DbxEnvRms += (blockRms > g_DbxEnvRms ? g_CoefDbxAtt : g_CoefDbxRel) * (blockRms - g_DbxEnvRms);

        // Expansion law:  G_dB = (L_in - L_th) * (R - 1), clamped to
        // [-DBX_MAX_CUT_DB, 0]. With R = 2 the output slope below threshold is
        // 2:1 (1 dB in -> 2 dB out), unity above threshold, and gain never
        // exceeds 0 dB, so loud passages are left untouched and no boost or
        // waveform squaring occurs.
        float in_dB = 20.0f * log10f((g_DbxEnvRms + 1e-9f) / DBX_REF_AMPLITUDE);
        float targetGain_dB = (in_dB >= DBX_THRESHOLD_DB)
                            ? 0.0f
                            : ((in_dB - DBX_THRESHOLD_DB) * DBX_GAIN_SLOPE);
        if (targetGain_dB < -DBX_MAX_CUT_DB) targetGain_dB = -DBX_MAX_CUT_DB;
        if (targetGain_dB > 0.0f)            targetGain_dB = 0.0f;

        // Smooth in the dB domain (attack/release from DSP_TC_DBX_GAIN_SLEW_S),
        // then convert once per block to the linear multiplier applied per sample
        g_DbxExpansion_dB += g_CoefDbxGainSlew * (targetGain_dB - g_DbxExpansion_dB);
        g_DbxGainSmooth = powf(10.0f, g_DbxExpansion_dB / 20.0f);

        g_DeHissMidGainCurrent = 1.0f;
        g_DeHissAirGainCurrent = 1.0f;
        g_DeHissMidGain_dB = 0.0f;
        g_DeHissAirGain_dB = g_DbxExpansion_dB;
        g_ExciterActiveGain = 0.0f;
        g_ExciterHarmonicsPercent = 0.0f;
        g_StereoWidthPercent = 100.0f;
    }
    else {
        // BYPASS mode
        g_DeHissMidGainCurrent = 1.0f;
        g_DeHissAirGainCurrent = 1.0f;
        g_DeHissMidGain_dB = 0.0f;
        g_DeHissAirGain_dB = 0.0f;
        g_ExciterActiveGain = 0.0f;
        g_ExciterHarmonicsPercent = 0.0f;
        g_StereoWidthPercent = 100.0f;
    }

    // 4. VU Meter RMS Track with Ballistics & Silence Squelch
    float rmsL = sqrtf(sumSqL / (float)numFrames);
    float rmsR = sqrtf(sumSqR / (float)numFrames);

    g_RmsSmoothL = (rmsL > g_RmsSmoothL) ? (g_RmsSmoothL * 0.40f + rmsL * 0.60f)
                                        : (g_RmsSmoothL * 0.85f + rmsL * 0.15f);
    g_RmsSmoothR = (rmsR > g_RmsSmoothR) ? (g_RmsSmoothR * 0.40f + rmsR * 0.60f)
                                        : (g_RmsSmoothR * 0.85f + rmsR * 0.15f);

    const float SILENCE_GATE_RMS = 0.012f;

    if (g_RmsSmoothL < SILENCE_GATE_RMS) {
        g_PeakL_dB = -60.0f;
    } else {
        float dbL = 20.0f * log10f(g_RmsSmoothL * 1.4142f);
        if (dbL < -35.0f) dbL = -60.0f;
        if (dbL > 3.0f)   dbL = 3.0f;
        g_PeakL_dB = dbL;
    }

    if (g_RmsSmoothR < SILENCE_GATE_RMS) {
        g_PeakR_dB = -60.0f;
    } else {
        float dbR = 20.0f * log10f(g_RmsSmoothR * 1.4142f);
        if (dbR < -35.0f) dbR = -60.0f;
        if (dbR > 3.0f)   dbR = 3.0f;
        g_PeakR_dB = dbR;
    }
}
