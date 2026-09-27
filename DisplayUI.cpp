/*
 * DisplayUI.cpp
 * Flicker-Free Retro Hi-Fi Touch UI for M5Stack Core2 (320x240 LCD)
 * Uses LovyanGFX M5Canvas Double-Buffering (PSRAM Sprite)
 * M5CoreNR
 */

#include "Config.h"
#include "DisplayUI.h"
#include "DSP_Engine.h"
#include "AudioPipeline.h"
#include "BTAudio.h"
#include <Arduino.h>
#include <M5Unified.h>

// UI Color Palette (Vintage 1990s Japanese Hi-Fi Theme)
#define COLOR_BG            0x0000 // Deep Black
#define COLOR_HEADER_BG     0x10A2 // Dark Gunmetal
#define COLOR_PANEL_BG      0x0841 // Charcoal Slate
#define COLOR_PANEL_BORDER  0x2124 // Subtle Frame
#define COLOR_TEXT_MUTED    0x7BEF // Muted Gray
#define COLOR_TEXT_LABEL    0xAD55 // Silver
#define COLOR_TEXT_VALUE    0xFFFF // Crisp White
#define COLOR_CYAN          0x07FF // Fluorescent Cyan
#define COLOR_GREEN         0x07E0 // Fluorescent Green
#define COLOR_AMBER         0xFEA0 // Warm Amber / Gold
#define COLOR_RED           0xF800 // Peak Red
#define COLOR_MAGENTA       0xF81F // Dolby Magenta

// Button Geometry (4 buttons across 320 px)
#define BTN_COUNT       4
#define BTN_Y           132
#define BTN_H           74
#define BTN_W           73
#define BTN_GAP         6
#define BTN_MARGIN_X    5

// Touch Button Pages
#define UI_PAGE_COUNT   3   // 0 = Enhancements, 1 = Tape NR, 2 = Bluetooth

static const int16_t g_BtnX[BTN_COUNT] = {
    BTN_MARGIN_X,
    BTN_MARGIN_X + (BTN_W + BTN_GAP),
    BTN_MARGIN_X + (BTN_W + BTN_GAP) * 2,
    BTN_MARGIN_X + (BTN_W + BTN_GAP) * 3
};

// Double-Buffer Canvas (PSRAM Sprite for Flicker-Free Rendering)
static M5Canvas g_Canvas;
static bool g_CanvasCreated = false;
static uint32_t g_LastUiUpdate = 0;
static uint8_t g_UiPage = 0; // 0 = Enhancements, 1 = Tape NR (Dolby/DBX), 2 = Bluetooth

// Peak Hold Ticks for VU Meters
static float g_PeakHoldL = -60.0f;
static float g_PeakHoldR = -60.0f;
static uint32_t g_PeakHoldTimeL = 0;
static uint32_t g_PeakHoldTimeR = 0;

// Forward Declarations
static void DrawHeader(void);
static void DrawVUMeters(void);
static void DrawStatusPanel(void);
static void DrawModeButtons(void);
static void DrawGuideBar(void);

void DisplayUI_Init(void) {
    M5.Display.setRotation(1);

    // Initialize 16-bit Double-Buffer Canvas in PSRAM to completely eliminate flicker
    if (!g_CanvasCreated) {
        g_Canvas.setColorDepth(16);
        if (psramFound()) {
            g_Canvas.setPsram(true);
        }
        g_Canvas.createSprite(SCREEN_WIDTH, SCREEN_HEIGHT);
        g_CanvasCreated = true;
    }

    DisplayUI_Update();
}

void DisplayUI_Update(void) {
    uint32_t now = millis();
    if (now - g_LastUiUpdate < 33) return; // ~30 Hz smooth refresh rate
    g_LastUiUpdate = now;

    if (!g_CanvasCreated) return;

    // 1. Clear off-screen canvas
    g_Canvas.fillScreen(COLOR_BG);

    // 2. Render all components to off-screen canvas
    DrawHeader();
    DrawVUMeters();
    DrawStatusPanel();
    DrawModeButtons();
    DrawGuideBar();

    // 3. Push complete frame to physical LCD in one instant DMA transfer (Zero Flicker!)
    g_Canvas.pushSprite(&M5.Display, 0, 0);
}

// 1. Header Bar: Title, CPU %, Battery %, Latency ms, Volume %
static void DrawHeader(void) {
    g_Canvas.fillRect(0, 0, 320, 24, COLOR_HEADER_BG);
    g_Canvas.drawFastHLine(0, 24, 320, COLOR_PANEL_BORDER);

    g_Canvas.setTextDatum(middle_left);
    g_Canvas.setTextSize(1);

    // App Name
    g_Canvas.setTextColor(COLOR_CYAN, COLOR_HEADER_BG);
    g_Canvas.drawString("M5-CoreNR", 8, 12);

    // Bluetooth radio badge: always visible, also from the DSP pages
    //   red = library missing, amber = paired but idle, green = audio flowing,
    //   cyan = advertising as a sink, cobalt = searching for a speaker.
    BtAudioState btState = BTAudio_GetState();
    BtAudioMode  btBadgeMode = BTAudio_GetMode();
    const int btBadgeLevel = BTAudio_GetLinkLevel();
    uint16_t btBadgeColor = COLOR_TEXT_MUTED;
    if (btState == BT_STATE_NO_LIB)       btBadgeColor = COLOR_RED;
    else if (btBadgeLevel == 2)           btBadgeColor = COLOR_GREEN;
    else if (btBadgeLevel == 1)           btBadgeColor = COLOR_AMBER;
    else if (btBadgeMode == BT_MODE_RX)   btBadgeColor = COLOR_CYAN;
    else if (btBadgeMode == BT_MODE_TX)   btBadgeColor = 0x541F;
    g_Canvas.setTextColor(btBadgeColor, COLOR_HEADER_BG);
    g_Canvas.drawString(BTAudio_GetBadgeText(), 64, 12);

    // Live DSP CPU % (includes the Bluetooth resampler / fan-out when BT is active)
    float cpuLoad = DSP_Engine_GetCPULoad();
    char cpuBuf[16];
    snprintf(cpuBuf, sizeof(cpuBuf), "CPU:%2.0f%%", cpuLoad);
    g_Canvas.setTextColor(cpuLoad > 60.0f ? COLOR_AMBER : COLOR_TEXT_LABEL, COLOR_HEADER_BG);
    g_Canvas.drawString(cpuBuf, 102, 12);

    // Battery % (Cached to eliminate 30Hz I2C traffic to AXP power chip)
    static uint32_t lastBatCheck = 0;
    static int32_t cachedBat = 100;
    uint32_t now = millis();
    if (now - lastBatCheck > 4000 || lastBatCheck == 0) {
        lastBatCheck = now;
        cachedBat = M5.Power.getBatteryLevel();
        if (cachedBat < 0) cachedBat = 0;
        if (cachedBat > 100) cachedBat = 100;
    }
    char batBuf[16];
    snprintf(batBuf, sizeof(batBuf), "BAT:%d%%", (int)cachedBat);
    g_Canvas.setTextColor(cachedBat < 20 ? COLOR_RED : COLOR_TEXT_LABEL, COLOR_HEADER_BG);
    g_Canvas.drawString(batBuf, 150, 12);

    // Round-Trip Audio Latency
    char latBuf[16];
    snprintf(latBuf, sizeof(latBuf), "LAT:%2.1fms", AUDIO_ROUNDTRIP_LATENCY_MS);
    g_Canvas.setTextColor(COLOR_TEXT_LABEL, COLOR_HEADER_BG);
    g_Canvas.drawString(latBuf, 198, 12);

    // Volume / Mute
    g_Canvas.setTextDatum(middle_right);
    if (AudioPipeline_GetMute()) {
        g_Canvas.setTextColor(COLOR_RED, COLOR_HEADER_BG);
        g_Canvas.drawString("MUTE", 314, 12);
    } else {
        char volBuf[16];
        snprintf(volBuf, sizeof(volBuf), "V:%d", AudioPipeline_GetVolume());
        g_Canvas.setTextColor(COLOR_AMBER, COLOR_HEADER_BG);
        g_Canvas.drawString(volBuf, 314, 12);
    }
}

// 2. Stereo Dual VU Meters with Peak-Hold
static void DrawVUMeters(void) {
    float peakL_dB, peakR_dB;
    DSP_Engine_GetVULevels(&peakL_dB, &peakR_dB);

    uint32_t now = millis();

    // Silence threshold: below -35 dB is ambient noise / idle silence
    if (peakL_dB <= -35.0f) {
        peakL_dB = -60.0f;
    }
    if (peakR_dB <= -35.0f) {
        peakR_dB = -60.0f;
    }

    // Update Peak Hold: only engage when genuine audio signal is present
    if (peakL_dB > -35.0f && peakL_dB >= g_PeakHoldL) {
        g_PeakHoldL = peakL_dB;
        g_PeakHoldTimeL = now;
    } else if (now - g_PeakHoldTimeL > 600) {
        g_PeakHoldL -= 1.5f;
        if (g_PeakHoldL < -35.0f) g_PeakHoldL = -60.0f;
    }

    if (peakR_dB > -35.0f && peakR_dB >= g_PeakHoldR) {
        g_PeakHoldR = peakR_dB;
        g_PeakHoldTimeR = now;
    } else if (now - g_PeakHoldTimeR > 600) {
        g_PeakHoldR -= 1.5f;
        if (g_PeakHoldR < -35.0f) g_PeakHoldR = -60.0f;
    }

    const int16_t meterX = 28;
    const int16_t meterW = 282;
    const int16_t meterH = 9;

    // Channel labels
    g_Canvas.setTextDatum(middle_left);
    g_Canvas.setTextSize(1);
    g_Canvas.setTextColor(COLOR_TEXT_LABEL, COLOR_BG);
    g_Canvas.drawString("L", 10, 34);
    g_Canvas.drawString("R", 10, 48);

    auto dBToWidth = [&](float dB) -> int16_t {
        if (dB <= -35.0f) return 0;
        if (dB >= 3.0f) return meterW;
        return (int16_t)(((dB + 35.0f) / 38.0f) * meterW);
    };

    int16_t wL = dBToWidth(peakL_dB);
    int16_t wR = dBToWidth(peakR_dB);
    int16_t holdL = dBToWidth(g_PeakHoldL);
    int16_t holdR = dBToWidth(g_PeakHoldR);

    // Left Channel Bar
    g_Canvas.fillRect(meterX, 30, meterW, meterH, 0x0841);
    if (wL > 0) {
        int16_t greenW = min((int16_t)wL, (int16_t)(meterW * 0.70f));
        g_Canvas.fillRect(meterX, 30, greenW, meterH, COLOR_GREEN);
        if (wL > meterW * 0.70f) {
            int16_t amberW = min((int16_t)(wL - meterW * 0.70f), (int16_t)(meterW * 0.23f));
            g_Canvas.fillRect(meterX + (int)(meterW * 0.70f), 30, amberW, meterH, COLOR_AMBER);
        }
        if (wL > meterW * 0.93f) {
            int16_t redW = wL - (int)(meterW * 0.93f);
            g_Canvas.fillRect(meterX + (int)(meterW * 0.93f), 30, redW, meterH, COLOR_RED);
        }
    }
    // Peak Hold Tick: only display when active signal is present and bar is drawn
    if (wL > 0 && holdL > 4 && holdL <= meterW) {
        g_Canvas.drawFastVLine(meterX + holdL - 1, 30, meterH, COLOR_TEXT_VALUE);
    }

    // Right Channel Bar
    g_Canvas.fillRect(meterX, 44, meterW, meterH, 0x0841);
    if (wR > 0) {
        int16_t greenW = min((int16_t)wR, (int16_t)(meterW * 0.70f));
        g_Canvas.fillRect(meterX, 44, greenW, meterH, COLOR_GREEN);
        if (wR > meterW * 0.70f) {
            int16_t amberW = min((int16_t)(wR - meterW * 0.70f), (int16_t)(meterW * 0.23f));
            g_Canvas.fillRect(meterX + (int)(meterW * 0.70f), 44, amberW, meterH, COLOR_AMBER);
        }
        if (wR > meterW * 0.93f) {
            int16_t redW = wR - (int)(meterW * 0.93f);
            g_Canvas.fillRect(meterX + (int)(meterW * 0.93f), 44, redW, meterH, COLOR_RED);
        }
    }
    // Peak Hold Tick: only display when active signal is present and bar is drawn
    if (wR > 0 && holdR > 4 && holdR <= meterW) {
        g_Canvas.drawFastVLine(meterX + holdR - 1, 44, meterH, COLOR_TEXT_VALUE);
    }

    // Scale Ticks & Legend (-35, -20, -10, -4, 0, +3 dB)
    g_Canvas.setTextDatum(top_center);
    g_Canvas.setTextColor(COLOR_TEXT_MUTED, COLOR_BG);
    g_Canvas.drawString("-35", meterX, 55);
    g_Canvas.drawString("-20", meterX + dBToWidth(-20.0f), 55);
    g_Canvas.drawString("-10", meterX + dBToWidth(-10.0f), 55);
    g_Canvas.drawString("-4",  meterX + dBToWidth(-4.0f), 55);
    g_Canvas.drawString("0",   meterX + dBToWidth(0.0f), 55);
    g_Canvas.drawString("+3",  meterX + meterW - 4, 55);
}

// 3. DSP Engine Status Panel: Active Mode, Dynamic LPF Cutoff, Exciter Harmonics %, Stereo Width %
static void DrawStatusPanel(void) {
    const int16_t panelY = 66;
    const int16_t panelH = 60;

    g_Canvas.fillRoundRect(6, panelY, 308, panelH, 4, COLOR_PANEL_BG);
    g_Canvas.drawRoundRect(6, panelY, 308, panelH, 4, COLOR_PANEL_BORDER);

    ProcessingMode mode = DSP_Engine_GetMode();

    // Line 1: Active Mode Label
    g_Canvas.setTextDatum(top_left);
    g_Canvas.setTextSize(1);
    g_Canvas.setTextColor(COLOR_TEXT_LABEL, COLOR_PANEL_BG);
    g_Canvas.drawString("DSP MODE:", 14, panelY + 6);

    uint16_t modeColor = COLOR_CYAN;
    if (mode == MODE_BYPASS)       modeColor = COLOR_AMBER;
    else if (mode == MODE_DNR)     modeColor = COLOR_GREEN;
    else if (mode == MODE_EXCITER) modeColor = COLOR_CYAN;
    else if (mode == MODE_DOLBY_B) modeColor = 0x541F; // Cobalt Blue
    else if (mode == MODE_DOLBY_C) modeColor = COLOR_MAGENTA;
    else if (mode == MODE_DBX)     modeColor = COLOR_AMBER;

    g_Canvas.setTextColor(modeColor, COLOR_PANEL_BG);
    g_Canvas.drawString(DSP_Engine_GetModeName(mode), 82, panelY + 6);

    // Line 2: Dynamic LPF / Decoder Reduction Depth
    char lpfBuf[52];
    if (mode == MODE_DNR) {
        float mid_dB = 0.0f, air_dB = 0.0f;
        DSP_Engine_GetDeHissGains(&mid_dB, &air_dB);
        snprintf(lpfBuf, sizeof(lpfBuf), "MID CUT: %4.1fdB | AIR HISS CUT: %4.1fdB", mid_dB, air_dB);
        g_Canvas.setTextColor(COLOR_GREEN, COLOR_PANEL_BG);
    } else if (mode == MODE_EXCITER) {
        float harm = DSP_Engine_GetExciterHarmonicsPercent();
        float airGain = DSP_Engine_GetExciterAirGain_dB();
        snprintf(lpfBuf, sizeof(lpfBuf), "BASS: +3.5dB | AIR: %+4.1fdB | EXCITER: %3.0f%%", airGain, harm);
        g_Canvas.setTextColor(COLOR_CYAN, COLOR_PANEL_BG);
    } else if (mode == MODE_DOLBY_B) {
        float red = DSP_Engine_GetDecoderReduction_dB();
        snprintf(lpfBuf, sizeof(lpfBuf), "DOLBY-B DECODER: %4.1fdB SLIDING EXPANDER", red);
        g_Canvas.setTextColor(0x541F, COLOR_PANEL_BG);
    } else if (mode == MODE_DOLBY_C) {
        float red = DSP_Engine_GetDecoderReduction_dB();
        snprintf(lpfBuf, sizeof(lpfBuf), "DOLBY-C DECODER: %4.1fdB DUAL-STAGE COMP", red);
        g_Canvas.setTextColor(COLOR_MAGENTA, COLOR_PANEL_BG);
    } else if (mode == MODE_DBX) {
        float exp = DSP_Engine_GetDecoderReduction_dB();
        snprintf(lpfBuf, sizeof(lpfBuf), "DBX TYPE-II: 1:2 DOWNWARD EXPANDER (%3.0fdB)", exp);
        g_Canvas.setTextColor(COLOR_AMBER, COLOR_PANEL_BG);
    } else {
        snprintf(lpfBuf, sizeof(lpfBuf), "PASSTHROUGH: 100%% BIT-PERFECT DIRECT COPY");
        g_Canvas.setTextColor(COLOR_TEXT_MUTED, COLOR_PANEL_BG);
    }
    g_Canvas.drawString(lpfBuf, 14, panelY + 22);

    // Line 3: Detail / Telemetry (the Bluetooth page shows the live BT link instead)
    char exciterBuf[64];
    if (g_UiPage == 2) {
        snprintf(exciterBuf, sizeof(exciterBuf), "%s", BTAudio_GetStatusText());
        BtAudioState btState = BTAudio_GetState();
        const int btLevel = BTAudio_GetLinkLevel();
        uint16_t btTextColor = COLOR_TEXT_MUTED;
        if (btState == BT_STATE_NO_LIB)        btTextColor = COLOR_RED;
        else if (btLevel == 2)                 btTextColor = COLOR_GREEN;
        else if (btLevel == 1)                 btTextColor = COLOR_AMBER;
        else if (btState != BT_STATE_OFF)      btTextColor = COLOR_CYAN;
        g_Canvas.setTextColor(btTextColor, COLOR_PANEL_BG);
    } else if (mode == MODE_EXCITER) {
        snprintf(exciterBuf, sizeof(exciterBuf), "TAPE WARMTH: ACTIVE    | DYNAMIC AIR EXPANDER");
        g_Canvas.setTextColor(COLOR_CYAN, COLOR_PANEL_BG);
    } else if (mode == MODE_DNR) {
        snprintf(exciterBuf, sizeof(exciterBuf), "0-3kHz: 100%% BIT-CLEAN  | 24dB/OCT LR4 SPLIT");
        g_Canvas.setTextColor(COLOR_TEXT_LABEL, COLOR_PANEL_BG);
    } else if (mode == MODE_DOLBY_B) {
        snprintf(exciterBuf, sizeof(exciterBuf), "TYPE-B DE-EMPHASIS: 1.8kHz HIGH-SHELF (-10dB)");
        g_Canvas.setTextColor(COLOR_TEXT_LABEL, COLOR_PANEL_BG);
    } else if (mode == MODE_DOLBY_C) {
        snprintf(exciterBuf, sizeof(exciterBuf), "ANTI-SKEW: ACTIVE      | DUAL SLIDING BAND: ON");
        g_Canvas.setTextColor(COLOR_TEXT_LABEL, COLOR_PANEL_BG);
    } else if (mode == MODE_DBX) {
        snprintf(exciterBuf, sizeof(exciterBuf), "RMS DETECTOR: TYPE-II  | DE-EMPHASIS: MATCHED");
        g_Canvas.setTextColor(COLOR_TEXT_LABEL, COLOR_PANEL_BG);
    } else {
        snprintf(exciterBuf, sizeof(exciterBuf), "FILTERS: BYPASSED      | 25Hz SUB: ON");
        g_Canvas.setTextColor(COLOR_TEXT_LABEL, COLOR_PANEL_BG);
    }
    g_Canvas.drawString(exciterBuf, 14, panelY + 38);
}

// 4. 4 Touch Buttons per Page (Page 0 = Enhancements, Page 1 = Tape NR, Page 2 = Bluetooth)
static void DrawModeButtons(void) {
    ProcessingMode currentMode = DSP_Engine_GetMode();
    BtAudioMode btMode = BTAudio_GetMode();

    const char* titles_p0[BTN_COUNT] = { "BYPASS", "DE-HISS", "EXCITER", "NEXT >" };
    const char* subTitles_p0[BTN_COUNT] = { "(Raw Tape)", "(3-Band LR4)", "(Air/Spark)", "(Tape NR)" };

    const char* titles_p1[BTN_COUNT] = { "DOLBY B", "DOLBY C", "DBX", "NEXT >" };
    const char* subTitles_p1[BTN_COUNT] = { "(-10dB Dec)", "(-20dB Dual)", "(2:1 Expand)", "(Bluetooth)" };

    const char* titles_p2[BTN_COUNT] = { "BT RX", "BT TX", "BT OFF", "< BACK" };

    // The Bluetooth page subtitles are live status text, so they are built per frame.
    const int btLevel = BTAudio_GetLinkLevel();
    char btSub0[14], btSub1[14], btSub2[14];
    if (btMode == BT_MODE_RX)  snprintf(btSub0, sizeof(btSub0), "%s", (btLevel == 2) ? "LINKED" : (btLevel == 1) ? "CONNECTED" : "PAIRING");
    else                       snprintf(btSub0, sizeof(btSub0), "(Receive)");
    if (btMode == BT_MODE_TX)  snprintf(btSub1, sizeof(btSub1), "%s", (btLevel == 2) ? "STREAMING" : (btLevel == 1) ? "CONNECTED" : "SEARCHING");
    else                       snprintf(btSub1, sizeof(btSub1), "(To Speaker)");
    if (btMode == BT_MODE_OFF) snprintf(btSub2, sizeof(btSub2), "(Radio Off)");
    else                       snprintf(btSub2, sizeof(btSub2), "(Turn Off)");
    const char* subTitles_p2[BTN_COUNT] = { btSub0, btSub1, btSub2, "(Enhance)" };

    const char* const* titles = (g_UiPage == 0) ? titles_p0 : ((g_UiPage == 1) ? titles_p1 : titles_p2);
    const char* const* subTitles = (g_UiPage == 0) ? subTitles_p0 : ((g_UiPage == 1) ? subTitles_p1 : subTitles_p2);

    for (int i = 0; i < BTN_COUNT; i++) {
        int16_t bx = g_BtnX[i];
        bool isActive = false;
        bool isNavBtn = (i == 3);

        uint16_t fillCol = COLOR_PANEL_BG;
        uint16_t borderCol = COLOR_PANEL_BORDER;
        uint16_t textCol = COLOR_TEXT_MUTED;
        uint16_t subCol = 0x52AA;

        if (g_UiPage == 0) {
            if (i == 0 && currentMode == MODE_BYPASS)   isActive = true;
            if (i == 1 && currentMode == MODE_DNR)      isActive = true;
            if (i == 2 && currentMode == MODE_EXCITER)  isActive = true;
            if (i == 3 && (currentMode == MODE_DOLBY_B || currentMode == MODE_DOLBY_C || currentMode == MODE_DBX)) {
                borderCol = COLOR_CYAN;
                subCol = COLOR_CYAN;
            }
        } else if (g_UiPage == 1) {
            if (i == 0 && currentMode == MODE_DOLBY_B)  isActive = true;
            if (i == 1 && currentMode == MODE_DOLBY_C)  isActive = true;
            if (i == 2 && currentMode == MODE_DBX)      isActive = true;
            if (i == 3 && btMode != BT_MODE_OFF) {
                borderCol = COLOR_MAGENTA;
                subCol = COLOR_MAGENTA;
            }
        } else {
            if (i == 0 && btMode == BT_MODE_RX)   isActive = true;
            if (i == 1 && btMode == BT_MODE_TX)   isActive = true;
            if (i == 2 && btMode == BT_MODE_OFF)  isActive = true;
            if (i == 3 && (currentMode == MODE_BYPASS || currentMode == MODE_DNR || currentMode == MODE_EXCITER)) {
                borderCol = COLOR_CYAN;
                subCol = COLOR_CYAN;
            }
        }

        if (isActive) {
            if (g_UiPage == 0) {
                switch (i) {
                    case 0: // BYPASS
                        fillCol = 0x2180; borderCol = COLOR_AMBER; textCol = COLOR_AMBER; subCol = COLOR_TEXT_VALUE;
                        break;
                    case 1: // DNR
                        fillCol = 0x0280; borderCol = COLOR_GREEN; textCol = COLOR_GREEN; subCol = COLOR_TEXT_VALUE;
                        break;
                    case 2: // EXCITER
                        fillCol = 0x020B; borderCol = COLOR_CYAN;  textCol = COLOR_CYAN;  subCol = COLOR_TEXT_VALUE;
                        break;
                }
            } else if (g_UiPage == 1) {
                switch (i) {
                    case 0: // DOLBY B
                        fillCol = 0x0113; borderCol = 0x541F; textCol = 0x541F; subCol = COLOR_TEXT_VALUE;
                        break;
                    case 1: // DOLBY C
                        fillCol = 0x3006; borderCol = COLOR_MAGENTA; textCol = COLOR_MAGENTA; subCol = COLOR_TEXT_VALUE;
                        break;
                    case 2: // DBX
                        fillCol = 0x3200; borderCol = COLOR_AMBER; textCol = COLOR_AMBER; subCol = COLOR_TEXT_VALUE;
                        break;
                }
            } else {
                switch (i) {
                    case 0: // BT RX (receiving from a phone)
                        fillCol = 0x0280; borderCol = COLOR_GREEN; textCol = COLOR_GREEN; subCol = COLOR_TEXT_VALUE;
                        break;
                    case 1: // BT TX (streaming to a speaker)
                        fillCol = 0x020B; borderCol = COLOR_CYAN; textCol = COLOR_CYAN; subCol = COLOR_TEXT_VALUE;
                        break;
                    case 2: // BT OFF (radio disabled, RF quiet)
                        fillCol = 0x3000; borderCol = COLOR_RED; textCol = COLOR_RED; subCol = COLOR_TEXT_VALUE;
                        break;
                }
            }
        } else if (isNavBtn) {
            fillCol = 0x10A2;
            if (g_UiPage == 0)      textCol = COLOR_CYAN;
            else if (g_UiPage == 1) textCol = COLOR_MAGENTA;
            else                    textCol = COLOR_AMBER;
            if (borderCol == COLOR_PANEL_BORDER) {
                borderCol = (g_UiPage == 0) ? 0x2B1F : 0x52AA;
            }
        }

        // Button Box
        g_Canvas.fillRoundRect(bx, BTN_Y, BTN_W, BTN_H, 6, fillCol);
        g_Canvas.drawRoundRect(bx, BTN_Y, BTN_W, BTN_H, 6, borderCol);
        if (isActive) {
            g_Canvas.drawRoundRect(bx + 1, BTN_Y + 1, BTN_W - 2, BTN_H - 2, 5, borderCol);
        }

        // Button Title
        g_Canvas.setTextDatum(middle_center);
        g_Canvas.setTextSize(1);
        g_Canvas.setTextColor(textCol, fillCol);
        g_Canvas.drawString(titles[i], bx + (BTN_W / 2), BTN_Y + 26);

        // Button Subtitle
        g_Canvas.setTextColor(subCol, fillCol);
        g_Canvas.drawString(subTitles[i], bx + (BTN_W / 2), BTN_Y + 48);
    }
}

// 5. Bottom Guide Bar (Hardware Touch Button Cues)
static void DrawGuideBar(void) {
    const int16_t guideY = 214;
    const int16_t guideH = 26;

    g_Canvas.fillRect(0, guideY, 320, guideH, 0x0821);
    g_Canvas.drawFastHLine(0, guideY, 320, COLOR_PANEL_BORDER);

    g_Canvas.setTextDatum(middle_center);
    g_Canvas.setTextSize(1);
    g_Canvas.setTextColor(COLOR_TEXT_LABEL, 0x0821);

    // Button A Cue
    g_Canvas.drawString("[< VOL -] BtnA", 54, guideY + 13);

    // Button B Cue
    g_Canvas.drawString("[* MUTE] BtnB", 160, guideY + 13);

    // Button C Cue
    g_Canvas.drawString("[VOL + >] BtnC", 266, guideY + 13);

    // Page indicator dots, right aligned in the thin gap above the button row
    for (int i = 0; i < UI_PAGE_COUNT; i++) {
        const uint16_t dotColor = (i == (int)g_UiPage) ? COLOR_CYAN : 0x4208;
        g_Canvas.fillRect(292 + i * 8, 127, 4, 3, dotColor);
    }
}

// --- Touch Event Handler ---
bool DisplayUI_HandleTouch(int16_t touchX, int16_t touchY) {
    static uint32_t lastTouchTime = 0;
    uint32_t now = millis();
    if (now - lastTouchTime < 300) {
        return false; // Debounce all touches to prevent double-firing
    }

    // Touch Button Grid: mode selection, page navigation and Bluetooth control
    if (touchY >= 130 && touchY <= 208) {
        for (int i = 0; i < BTN_COUNT; i++) {
            if (touchX >= g_BtnX[i] && touchX <= g_BtnX[i] + BTN_W) {
                lastTouchTime = now;
                if (i == 3) {
                    // Navigation Button: cycles Enhancements -> Tape NR -> Bluetooth -> Enhancements
                    g_UiPage = (g_UiPage + 1) % UI_PAGE_COUNT;
                    M5.Power.setVibration(60);
                    delay(15);
                    M5.Power.setVibration(0);
                    g_LastUiUpdate = 0;
                    DisplayUI_Update();
                    return true;
                }

                // Bluetooth Page: BT RX / BT TX / BT OFF
                if (g_UiPage == 2) {
                    if (i == 0)      BTAudio_StartRx();
                    else if (i == 1) BTAudio_StartTx();
                    else             BTAudio_Stop();

                    M5.Power.setVibration(80);
                    delay(20);
                    M5.Power.setVibration(0);
                    g_LastUiUpdate = 0;
                    DisplayUI_Update();
                    return true;
                }

                // Mode Selection Button (pages 0 and 1)
                ProcessingMode newMode = MODE_BYPASS;
                if (g_UiPage == 0) {
                    if (i == 0) newMode = MODE_BYPASS;
                    else if (i == 1) newMode = MODE_DNR;
                    else if (i == 2) newMode = MODE_EXCITER;
                } else {
                    if (i == 0) newMode = MODE_DOLBY_B;
                    else if (i == 1) newMode = MODE_DOLBY_C;
                    else if (i == 2) newMode = MODE_DBX;
                }

                if (DSP_Engine_GetMode() != newMode) {
                    DSP_Engine_SetMode(newMode);
                    M5.Power.setVibration(80);
                    delay(20);
                    M5.Power.setVibration(0);
                    g_LastUiUpdate = 0;
                    DisplayUI_Update();
                }
                return true;
            }
        }
    }
    return false;
}
