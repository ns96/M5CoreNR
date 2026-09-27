/*
 * DisplayUI.h
 * Retro Hi-Fi Touch UI for M5Stack Core2 (320x240 LCD)
 * M5CoreNR
 */

#ifndef DISPLAY_UI_H
#define DISPLAY_UI_H

#include <stdint.h>
#include <stdbool.h>
#include "Config.h"

#ifdef __cplusplus
extern "C" {
#endif

void DisplayUI_Init(void);
void DisplayUI_Update(void);
bool DisplayUI_HandleTouch(int16_t touchX, int16_t touchY);

#ifdef __cplusplus
}
#endif

#endif // DISPLAY_UI_H
