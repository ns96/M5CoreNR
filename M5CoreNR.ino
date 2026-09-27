/*
 * M5CoreNR.ino
 * Real-Time Cassette Tape Audio Restoration Suite
 * Psychoacoustic Harmonic Exciter & Mid/Side Stereo Expander
 * For Cassette Tapes on M5Stack Core2 with M5 Module Audio (ES8388)
 */

#include "Config.h"
#include "DSP_Engine.h"
#include "AudioPipeline.h"
#include "DisplayUI.h"
#include "BTAudio.h"
#include <Arduino.h>
#include <M5Unified.h>
#include <esp_bt.h>

void setup() {
    // Radio policy: WiFi is NEVER initialised in this firmware, so the WiFi radio never
    // transmits and no WiFi/BT coexistence is engaged - the wired noise floor stays clean
    // by construction. (Calling WiFi.mode(WIFI_OFF) would pull the WiFi library into the
    // build and its IRAM optimised code no longer fits alongside the Bluetooth stack on the
    // ESP32, which fails the link with an iram0_0_seg overflow.)
    //
    // Bluetooth is also not started here: BTAudio keeps the radio off until the user selects
    // BT RX or BT TX on the third screen, so the default boot state is RF quiet.

    // 1. Initialize M5Unified hardware abstraction
    auto cfg = M5.config();
    cfg.internal_spk = false; // CRITICAL: Disable internal NS4168 speaker (Module Audio codec handles output)
    cfg.internal_mic = false; // CRITICAL: Disable internal SPM1423 mic (prevent GPIO 34/2/0 I2S bus contention)
    cfg.external_spk = 0;
    M5.begin(cfg);

    // Hardware isolation: Force internal SPM1423 PDM microphone into hardware sleep.
    // GPIO 12 is the mic clock line. Driving it firmly LOW forces the microphone into
    // power-down mode and tri-states its DATA line on GPIO 34, preventing any bus contention
    // or digital noise injection into the Module Audio ES8388 Line-In ADC!
    pinMode(12, OUTPUT);
    digitalWrite(12, LOW);

    Serial.begin(115200);
    delay(100);

    Serial.println("\n==============================================");
    Serial.printf("   %s %s - Cassette Audio Restoration Suite\n", APP_FIRMWARE_NAME, APP_FIRMWARE_VERSION);
    Serial.println("   M5Stack Core2 + Module Audio (ES8388)");
    Serial.println("   DNR + Exciter + Dolby B/C + DBX Tape Decoders");
    Serial.println("   Bluetooth A2DP RX (sink) + TX (source)");
    Serial.println("==============================================\n");

    // 2. Power Management (Disable internal speaker amp to eliminate noise)
    switch (M5.Power.getType()) {
        case m5::Power_Class::pmic_axp192:
            Serial.println("[POWER] AXP192 Detected. Disabling internal speaker amp...");
            M5.Power.Axp192.setGPIO2(false); // Disable internal speaker amp (headphone jack is on Module Audio)
            break;
        case m5::Power_Class::pmic_axp2101:
            Serial.println("[POWER] AXP2101 Detected. Audio rails active...");
            M5.Power.Axp2101.setALDO3(0);
            break;
        default:
            Serial.println("[POWER] Power IC initialized.");
            break;
    }

    // 3. Initialize Display UI
    Serial.println("[UI] Initializing 320x240 LCD Touch Interface...");
    DisplayUI_Init();

    // 4. Initialize Audio Pipeline & ES8388 Codec on Wire1
    Serial.println("[AUDIO] Initializing I2S DMA and ES8388 Audio Codec...");
    if (!AudioPipeline_Init()) {
        Serial.println("[AUDIO] FATAL: Audio Pipeline failed to initialize!");
    } else {
        Serial.println("[AUDIO] Audio Pipeline initialized successfully.");
    }

    // 5. Start Core 0 Real-Time Audio Task
    Serial.println("[AUDIO] Launching High-Priority Audio Task on Core 0...");
    AudioPipeline_Start();

    // 6. Prepare the Bluetooth A2DP module (allocates PSRAM rings only; no radio traffic
    //    happens until BT RX or BT TX is selected on the third UI page).
    Serial.println("[BT] Preparing Bluetooth A2DP module...");
    if (BTAudio_Init()) {
        Serial.println("[BT] Ready. Radio stays OFF until you press BT RX or BT TX.");
    } else {
        Serial.println("[BT] WARNING: Bluetooth module unavailable (see messages above).");
    }

    Serial.println("[SYSTEM] Ready! Plug cassette deck into Line In and headphones into Phone jack.");
}

void loop() {
    // 1. Poll touch and hardware buttons
    M5.update();

    // 2. Handle On-Screen Touch (Mode Selection Grid)
    auto touchDetail = M5.Touch.getDetail();
    if (touchDetail.wasPressed()) {
        DisplayUI_HandleTouch(touchDetail.x, touchDetail.y);
    }

    // 3. Handle Hardware Touch Buttons (Below LCD)
    // Left Touch Button (BtnA): Volume Down
    if (M5.BtnA.wasClicked() || M5.BtnA.wasHold()) {
        AudioPipeline_VolumeDown();
        M5.Power.setVibration(40);
        delay(15);
        M5.Power.setVibration(0);
        DisplayUI_Update();
    }

    // Center Touch Button (BtnB): Mute Toggle
    if (M5.BtnB.wasClicked()) {
        AudioPipeline_ToggleMute();
        M5.Power.setVibration(60);
        delay(20);
        M5.Power.setVibration(0);
        DisplayUI_Update();
    }

    // Right Touch Button (BtnC): Volume Up
    if (M5.BtnC.wasClicked() || M5.BtnC.wasHold()) {
        AudioPipeline_VolumeUp();
        M5.Power.setVibration(40);
        delay(15);
        M5.Power.setVibration(0);
        DisplayUI_Update();
    }

    // 4. Update Bluetooth link state (cheap poll; nothing happens while BT is off)
    BTAudio_Update();

    // 5. Update UI Telemetry (30 Hz refresh)
    DisplayUI_Update();

    // 6. Update Module-Audio RGB status LEDs
    AudioPipeline_UpdateLEDs();

    // 7. Yield briefly to FreeRTOS on Core 1
    vTaskDelay(10 / portTICK_PERIOD_MS);
}
