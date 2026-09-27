/*
 * AudioPipeline.cpp
 * High-Performance I2S DMA Audio Driver and ES8388 Codec Manager
 * M5CoreNR (ESP32 / M5Stack Core2 + Module Audio)
 */

#include "Config.h"
#include "AudioPipeline.h"
#include "DSP_Engine.h"
#include "BTAudio.h"
#include <Arduino.h>
#include <M5Unified.h>
#include "audio_i2c.hpp"
#include "es8388.hpp"

#if __has_include(<driver/i2s_std.h>)
#include <driver/i2s_std.h>
#define USE_ESP_IDF_V5_I2S 1
#else
#include <driver/i2s.h>
#define USE_ESP_IDF_V5_I2S 0
#endif

#include <esp_heap_caps.h>
#include <soc/io_mux_reg.h>
#include <soc/soc.h>

// Module-Audio Library Instances
static AudioI2c g_AudioDevice;
static ES8388* g_pES8388 = nullptr;

// Audio Configuration & State
static uint8_t g_Volume = DEFAULT_HEADPHONE_VOL;
static bool g_IsMuted = false;
static bool g_HpModeAmerican = true; // Default to CTIA (standard for modern headphones)

// Signal routing: where the DSP input comes from, and whether the processed output
// is also fanned out to the Bluetooth A2DP source. Both are owned by Core 1 (UI).
static volatile AudioInputSource g_InputSource = AUDIO_IN_LINE;
static volatile bool g_BtTxTap = false;

// FreeRTOS Task & I2S Handles
static TaskHandle_t g_AudioTaskHandle = nullptr;
#if USE_ESP_IDF_V5_I2S
static i2s_chan_handle_t g_TxHandle = nullptr;
static i2s_chan_handle_t g_RxHandle = nullptr;
#endif

// --- I2S Driver Setup for DMA Streaming ---
static bool I2S_InitDriver(void) {
#if USE_ESP_IDF_V5_I2S
    Serial.println("[AUDIO] Configuring ESP32 I2S Peripheral for DMA Streaming (IDF v5 i2s_std + APLL)...");

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = AUDIO_DMA_BUF_COUNT;
    chan_cfg.dma_frame_num = AUDIO_DMA_BUF_LEN;
    chan_cfg.auto_clear = true;

    esp_err_t err = i2s_new_channel(&chan_cfg, &g_TxHandle, &g_RxHandle);
    if (err != ESP_OK) {
        Serial.printf("[AUDIO] ERROR: i2s_new_channel failed: %d\n", err);
        return false;
    }

    // High-precision Audio PLL (APLL) clock configuration
    // Generates exact 12.288 MHz master clock (256x oversampling at 48 kHz) with zero phase jitter,
    // completely eliminating fractional PLL divider jitter that causes digital ADC/DAC crackling.
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)SAMPLING_FREQ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = (gpio_num_t)SYS_I2S_MCLK_PIN, // GPIO 0
            .bclk = (gpio_num_t)SYS_I2S_SCLK_PIN, // GPIO 19
            .ws = (gpio_num_t)SYS_I2S_LRCK_PIN,   // GPIO 27
            .dout = (gpio_num_t)SYS_I2S_DOUT_PIN, // GPIO 2
            .din = (gpio_num_t)SYS_I2S_DIN_PIN,   // GPIO 34
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_APLL;
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT;

    err = i2s_channel_init_std_mode(g_TxHandle, &std_cfg);
    if (err != ESP_OK) {
        Serial.printf("[AUDIO] ERROR: i2s_channel_init_std_mode(TX) failed: %d\n", err);
        return false;
    }

    err = i2s_channel_init_std_mode(g_RxHandle, &std_cfg);
    if (err != ESP_OK) {
        Serial.printf("[AUDIO] ERROR: i2s_channel_init_std_mode(RX) failed: %d\n", err);
        return false;
    }

    err = i2s_channel_enable(g_TxHandle);
    if (err != ESP_OK) {
        Serial.printf("[AUDIO] ERROR: i2s_channel_enable(TX) failed: %d\n", err);
        return false;
    }

    err = i2s_channel_enable(g_RxHandle);
    if (err != ESP_OK) {
        Serial.printf("[AUDIO] ERROR: i2s_channel_enable(RX) failed: %d\n", err);
        return false;
    }

    return true;
#else
    Serial.println("[AUDIO] Configuring ESP32 I2S Peripheral for DMA Streaming (Legacy)...");

    PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO0_U, FUNC_GPIO0_CLK_OUT1);
    WRITE_PERI_REG(PIN_CTRL, 0xFFF0);

    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX),
        .sample_rate = (uint32_t)SAMPLING_FREQ,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = AUDIO_DMA_BUF_COUNT,
        .dma_buf_len = AUDIO_DMA_BUF_LEN,
        .use_apll = true,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = SYS_I2S_SCLK_PIN,
        .ws_io_num = SYS_I2S_LRCK_PIN,
        .data_out_num = SYS_I2S_DOUT_PIN,
        .data_in_num = SYS_I2S_DIN_PIN
    };

    esp_err_t err = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("[AUDIO] ERROR: i2s_driver_install failed: %d\n", err);
        return false;
    }

    err = i2s_set_pin(I2S_NUM_0, &pin_config);
    if (err != ESP_OK) {
        Serial.printf("[AUDIO] ERROR: i2s_set_pin failed: %d\n", err);
        return false;
    }

    i2s_zero_dma_buffer(I2S_NUM_0);
    return true;
#endif
}

// --- High Priority Audio Task (Pinned to Core 0) ---
static void AudioTask(void* parameter) {
    Serial.println("[AUDIO_TASK] Running full-bandwidth 48kHz real-time DSP on Core 0 (NO DECIMATION)");

    int16_t rxDmaBuf[AUDIO_DMA_BUF_LEN * 2]; // Stereo 16-bit
    int16_t txDmaBuf[AUDIO_DMA_BUF_LEN * 2]; // Stereo 16-bit
    size_t bytesRead = 0;
    size_t bytesWritten = 0;

    // Available CPU cycles per DMA chunk at 240 MHz: (AUDIO_DMA_BUF_LEN / 48000) * 240,000,000
    const float totalAvailCycles = ((float)AUDIO_DMA_BUF_LEN / SAMPLING_FREQ) * 240000000.0f;
    static float smoothedCpuPercent = 0.0f;

    // Pre-load TX DMA queue with 2 chunks of silence (21.3 ms safety cushion)
    // In full-duplex I2S, this completely eliminates TX DMA starvation/underflow
    // while the task is blocked waiting for incoming RX DMA packets.
    memset(txDmaBuf, 0, sizeof(txDmaBuf));
#if USE_ESP_IDF_V5_I2S
    i2s_channel_write(g_TxHandle, txDmaBuf, sizeof(txDmaBuf), &bytesWritten, portMAX_DELAY);
    i2s_channel_write(g_TxHandle, txDmaBuf, sizeof(txDmaBuf), &bytesWritten, portMAX_DELAY);
#else
    i2s_write(I2S_NUM_0, txDmaBuf, sizeof(txDmaBuf), &bytesWritten, portMAX_DELAY);
    i2s_write(I2S_NUM_0, txDmaBuf, sizeof(txDmaBuf), &bytesWritten, portMAX_DELAY);
#endif

    for (;;) {
        // 1. Read incoming Line-In audio from ES8388 ADC via I2S DMA
#if USE_ESP_IDF_V5_I2S
        esp_err_t res = i2s_channel_read(g_RxHandle, rxDmaBuf, sizeof(rxDmaBuf), &bytesRead, portMAX_DELAY);
#else
        esp_err_t res = i2s_read(I2S_NUM_0, rxDmaBuf, sizeof(rxDmaBuf), &bytesRead, portMAX_DELAY);
#endif
        if (res != ESP_OK || bytesRead == 0) {
            vTaskDelay(1 / portTICK_PERIOD_MS);
            continue;
        }

        // Exact number of stereo sample pairs (NO DECIMATION!)
        int numStereoSamples = bytesRead / (sizeof(int16_t) * 2);

        // 2. Profile the whole per-block path: input source + DSP + Bluetooth fan-out
        uint32_t tStart = ESP.getCycleCount();

        // 2a. Bluetooth RX: swap the Line In block for decoded A2DP audio that
        //     BTAudio has already resampled to exactly this block size at 48 kHz.
        //     The ADC read above still paces the loop and clocks the ES8388.
        if (g_InputSource == AUDIO_IN_BT) {
            BTAudio_PopTo48k(rxDmaBuf, numStereoSamples);
        }

        if (g_IsMuted) {
            memset(txDmaBuf, 0, bytesRead);
        } else {
            // Process all 48,000 stereo samples per second with zero decimation
            DSP_Engine_ProcessStereo(rxDmaBuf, txDmaBuf, numStereoSamples);
        }

        // 2b. Bluetooth TX: fan the very same block out to the A2DP source, so the
        //     wired headphones and the Bluetooth speaker get identical audio.
        //     Placed after the mute handling, so MUTE mutes both outputs.
        if (g_BtTxTap) {
            BTAudio_PushFrom48k(txDmaBuf, numStereoSamples);
        }

        uint32_t tElapsed = ESP.getCycleCount() - tStart;
        float currentCpuPercent = ((float)tElapsed / totalAvailCycles) * 100.0f;
        smoothedCpuPercent = (smoothedCpuPercent * 0.90f) + (currentCpuPercent * 0.10f);
        DSP_Engine_SetCPULoad(smoothedCpuPercent);

        // 3. Write processed audio to Headphone DAC via I2S DMA
#if USE_ESP_IDF_V5_I2S
        i2s_channel_write(g_TxHandle, txDmaBuf, bytesRead, &bytesWritten, portMAX_DELAY);
#else
        i2s_write(I2S_NUM_0, txDmaBuf, bytesRead, &bytesWritten, portMAX_DELAY);
#endif
    }
}

// I2C Register helper for ES8388 (writes directly over Wire1 without needing private methods)
static bool ES8388_WriteReg(uint8_t reg, uint8_t data) {
    Wire1.beginTransmission(ES8388_ADDR);
    Wire1.write(reg);
    Wire1.write(data);
    return (Wire1.endTransmission() == 0);
}

// --- Public API ---
bool AudioPipeline_Init(void) {
    // 1. Initialize DSP Engine
    DSP_Engine_Init(SAMPLING_FREQ);

    // 2. Initialize I2S Peripheral & Master Clock (MCLK on GPIO 0) FIRST so codec has active clock
    if (!I2S_InitDriver()) {
        Serial.println("[AUDIO] ERROR: I2S Driver failed to initialize!");
        return false;
    }

    // 3. Initialize Module-Audio STM32 controller and ES8388 Codec on Wire1 (I2C_NUM_1)
    Serial.println("[AUDIO] Initializing Module-Audio (audio_i2c & es8388) on Wire1...");
    g_AudioDevice.begin(&Wire1, SYS_I2C_SDA_PIN, SYS_I2C_SCL_PIN);
    delay(20);
    
    // Configure Headphone Jack for US Standard (AMERICAN / CTIA)
    AudioPipeline_SetHPMode(true);
    g_AudioDevice.setMICStatus(AUDIO_MIC_OPEN);
    g_AudioDevice.setRGBBrightness(30);   // Comfortable 30% brightness
    g_AudioDevice.setRGBLED(0, 0x002244); // Soft Cyan/Blue (System Active)
    g_AudioDevice.setRGBLED(1, 0x000000);
    g_AudioDevice.setRGBLED(2, 0x000000);

    if (!g_pES8388) {
        g_pES8388 = new ES8388(&Wire1, SYS_I2C_SDA_PIN, SYS_I2C_SCL_PIN);
    }

    if (!g_pES8388->init()) {
        Serial.println("[AUDIO] WARNING: ES8388 init returned false!");
    } else {
        Serial.println("[AUDIO] ES8388 Codec initialized successfully.");
    }

    // CRITICAL FIX: Disable ES8388 hardware ALC (Automatic Level Control)
    // By default, M5Stack's ES8388::init() sets ADCCONTROL10 = 0xEA (stereo ALC on, +35.5dB max gain).
    // In silence or tape pauses, the hardware ALC cranks the PGA gain to maximum (+35.5dB) hunting
    // for audio, causing loud static hiss and wild VU meter spikes up to -4 dB!
    ES8388_WriteReg(ES8388_ADCCONTROL10, 0x00); // ALCSEL = 00 (ALC OFF, Manual Gain)
    ES8388_WriteReg(ES8388_ADCCONTROL11, 0x00); // ALC Min Gain = 0dB
    ES8388_WriteReg(ES8388_ADCCONTROL12, 0x00); // Reset ALC decay/attack
    ES8388_WriteReg(ES8388_ADCCONTROL13, 0x00); // Reset ALC timing
    ES8388_WriteReg(ES8388_ADCCONTROL14, 0x00); // Disable noise gate dither

    // Exact proven codec configuration from M5CoreFSK & Module-Audio reference
    g_pES8388->setADCInput(ADC_INPUT_LINPUT1_RINPUT1);
    g_pES8388->setMicGain(MIC_GAIN_6DB); // Clean +6dB analog preamp: optimal tape level with zero clipping
    g_pES8388->setADCVolume(100);        // 0dB ADC digital attenuation (full scale)
    g_pES8388->setDACOutput(DAC_OUTPUT_OUT1);
    g_pES8388->setBitsSample(ES_MODULE_ADC, BIT_LENGTH_16BITS);
    g_pES8388->setBitsSample(ES_MODULE_DAC, BIT_LENGTH_16BITS);
    g_pES8388->setSampleRate(SAMPLE_RATE_48K);

    // CRITICAL FIX: Disconnect analog bypasses and set 0dB clean mixer gain (0x88)
    // In ES8388, 0x88 sets: DAC to Output enabled (Bit 7=1), all Line-In analog bypasses MUTED (Bits 6,5=0), 0dB mixer gain (Bits 4:2=010b)
    // (Note: 0x90 sets Bits 4:2=100b which applies +6dB analog gain, causing severe mixer clipping and crackle!)
    g_pES8388->setMixSourceSelect(MIXADC, MIXADC);
    ES8388_WriteReg(ES8388_DACCONTROL17, 0x88); // Left Output: DAC only (0dB unity), Line-In bypass strictly MUTED
    ES8388_WriteReg(ES8388_DACCONTROL18, 0x38); // Standard internal DAC configuration
    ES8388_WriteReg(ES8388_DACCONTROL19, 0x38); // Standard internal DAC configuration
    ES8388_WriteReg(ES8388_DACCONTROL20, 0x88); // Right Output: DAC only (0dB unity), Line-In bypass strictly MUTED
    ES8388_WriteReg(ES8388_DACCONTROL21, 0x80); // CRITICAL: 0x80 enables DAC DLL/reference clock

    AudioPipeline_SetVolume(g_Volume);

    Serial.println("[AUDIO] ES8388 configured: APLL clock, ALC disabled, 0dB unity passthrough, 48kHz DSP.");
    return true;
}

void AudioPipeline_Start(void) {
    if (!g_AudioTaskHandle) {
        xTaskCreatePinnedToCore(
            AudioTask,
            "AudioTask",
            8192,
            NULL,
            5, // High priority
            &g_AudioTaskHandle,
            0  // Pinned to Core 0
        );
    }
}

void AudioPipeline_Stop(void) {
    if (g_AudioTaskHandle) {
        vTaskDelete(g_AudioTaskHandle);
        g_AudioTaskHandle = nullptr;
    }
#if USE_ESP_IDF_V5_I2S
    if (g_RxHandle) {
        i2s_channel_disable(g_RxHandle);
        i2s_del_channel(g_RxHandle);
        g_RxHandle = nullptr;
    }
    if (g_TxHandle) {
        i2s_channel_disable(g_TxHandle);
        i2s_del_channel(g_TxHandle);
        g_TxHandle = nullptr;
    }
#else
    i2s_driver_uninstall(I2S_NUM_0);
#endif
}

void AudioPipeline_SetInputSource(AudioInputSource source) {
    if (g_InputSource == source) return;
    g_InputSource = source;
    Serial.printf("[AUDIO] Input source: %s\n",
                  (source == AUDIO_IN_BT) ? "BLUETOOTH (A2DP RX)" : "LINE IN (ES8388 ADC)");
}

AudioInputSource AudioPipeline_GetInputSource(void) {
    return g_InputSource;
}

void AudioPipeline_SetBtTxTap(bool enable) {
    if (g_BtTxTap == enable) return;
    g_BtTxTap = enable;
    Serial.printf("[AUDIO] Bluetooth TX fan-out: %s\n", enable ? "ON" : "OFF");
}

bool AudioPipeline_GetBtTxTap(void) {
    return g_BtTxTap;
}

// Mirror the ES8388 DAC volume law onto the Bluetooth TX stream, so the wired
// headphones and the Bluetooth listener hear the same level.
// ES8388::setDACVolume() maps volume 0-100 to register steps = (V * 33 + 50) / 100,
// clamped to BT_VOL_STEPS_MAX (0x21 = 33), at BT_VOL_DB_PER_STEP dB per step.
static float HeadphoneVolumeToBtGain(uint8_t volume) {
#if BT_TX_MIRROR_VOLUME
    if (volume == 0) {
        return powf(10.0f, -BT_VOL_FLOOR_DB / 20.0f);   // effectively silent
    }
    int steps = ((int)volume * BT_VOL_STEPS_MAX + 50) / 100;
    if (steps > BT_VOL_STEPS_MAX) steps = BT_VOL_STEPS_MAX;
    if (steps < 0) steps = 0;
    const float atten_dB = (float)(BT_VOL_STEPS_MAX - steps) * BT_VOL_DB_PER_STEP;
    return powf(10.0f, -atten_dB / 20.0f);
#else
    (void)volume;
    return 1.0f;
#endif
}

void AudioPipeline_SetVolume(uint8_t volume) {
    g_Volume = (volume > 100) ? 100 : volume;
    if (g_pES8388) {
        g_pES8388->setDACVolume(g_IsMuted ? 0 : g_Volume);
    }
    // Bluetooth TX follows the headphone level, and MUTE silences both outputs.
    BTAudio_SetTxGain(g_IsMuted ? 0.0f : HeadphoneVolumeToBtGain(g_Volume));
}

uint8_t AudioPipeline_GetVolume(void) {
    return g_Volume;
}

void AudioPipeline_VolumeUp(void) {
    if (g_IsMuted) {
        g_IsMuted = false;
    }
    if (g_Volume + VOLUME_STEP <= 100) {
        AudioPipeline_SetVolume(g_Volume + VOLUME_STEP);
    } else {
        AudioPipeline_SetVolume(100);
    }
}

void AudioPipeline_VolumeDown(void) {
    if (g_IsMuted) {
        g_IsMuted = false;
    }
    if (g_Volume >= VOLUME_STEP) {
        AudioPipeline_SetVolume(g_Volume - VOLUME_STEP);
    } else {
        AudioPipeline_SetVolume(0);
    }
}

void AudioPipeline_SetMute(bool mute) {
    g_IsMuted = mute;
    AudioPipeline_SetVolume(g_Volume);
}

bool AudioPipeline_GetMute(void) {
    return g_IsMuted;
}

void AudioPipeline_ToggleMute(void) {
    AudioPipeline_SetMute(!g_IsMuted);
}

void AudioPipeline_SetHPMode(bool americanCTIA) {
    g_HpModeAmerican = americanCTIA;
    g_AudioDevice.setHPMode(g_HpModeAmerican ? AUDIO_HPMODE_AMERICAN : AUDIO_HPMODE_NATIONAL);
    delay(10);
    g_AudioDevice.setFlashWriteBack();
    Serial.printf("[AUDIO] Headphone Mode set to US Standard: %s\n", g_HpModeAmerican ? "AMERICAN (CTIA)" : "NATIONAL (OMTP)");
}

bool AudioPipeline_GetHPMode(void) {
    return g_HpModeAmerican;
}

void AudioPipeline_ToggleHPMode(void) {
    AudioPipeline_SetHPMode(!g_HpModeAmerican);
}

// Update the Module-Audio RGB LEDs to reflect active DSP Mode
void AudioPipeline_UpdateLEDs(void) {
    static uint32_t lastUpdate = 0;
    static uint32_t lastCol0 = 0xFFFFFFFF;
    static uint32_t lastCol1 = 0xFFFFFFFF;
    static uint32_t lastCol2 = 0xFFFFFFFF;

    uint32_t now = millis();
    if (now - lastUpdate < 60) return;
    lastUpdate = now;

    uint32_t col0 = 0x002244; // Soft Blue (System Normal)
    uint32_t col1 = 0x000000;
    uint32_t col2 = 0x000000;

    if (g_IsMuted) {
        col0 = 0x330000; // Dim Red
        col1 = 0x330000;
        col2 = 0x330000;
    } else {
        ProcessingMode mode = DSP_Engine_GetMode();
        switch (mode) {
            case MODE_BYPASS:
                col1 = 0x332200; // Soft Amber (Raw Passthrough)
                col2 = 0x000000;
                break;
            case MODE_DNR:
                col1 = 0x005500; // Green (Hiss Filter Active)
                col2 = 0x000000;
                break;
            case MODE_EXCITER:
                col1 = 0x004455; // Cyan (Harmonic Exciter Active)
                col2 = 0x004455;
                break;
            case MODE_DOLBY_B:
                col1 = 0x002266; // Blue (Dolby B Active)
                col2 = 0x002266;
                break;
            case MODE_DOLBY_C:
                col1 = 0x550055; // Magenta / Purple (Dolby C Active)
                col2 = 0x550055;
                break;
            case MODE_DBX:
                col1 = 0x553300; // Gold / Amber (DBX II Expander Active)
                col2 = 0x553300;
                break;
        }

        // LED 0 doubles as the Bluetooth radio indicator once BT is selected:
        // slow blink = stream linked, fast blink = pairing / searching.
        // LED 1 and LED 2 keep showing the active DSP mode.
        BtAudioMode btMode = BTAudio_GetMode();
        if (btMode != BT_MODE_OFF) {
            const bool linked = BTAudio_IsLinked();
            const uint32_t btPeriod = linked ? 900 : 220;
            const bool btOn = ((now / btPeriod) & 1) == 0;
            if (btMode == BT_MODE_RX) {
                col0 = btOn ? 0x006688 : 0x001122; // Cyan/blue = receiving from Bluetooth
            } else {
                col0 = btOn ? 0x440088 : 0x110022; // Purple = transmitting over Bluetooth
            }
        }
    }

    if (col0 != lastCol0) { g_AudioDevice.setRGBLED(0, col0); lastCol0 = col0; }
    if (col1 != lastCol1) { g_AudioDevice.setRGBLED(1, col1); lastCol1 = col1; }
    if (col2 != lastCol2) { g_AudioDevice.setRGBLED(2, col2); lastCol2 = col2; }
}

AudioI2c& AudioPipeline_GetDevice(void) {
    return g_AudioDevice;
}

ES8388& AudioPipeline_GetCodec(void) {
    return *g_pES8388;
}
