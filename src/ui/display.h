#pragma once
// OLED SSD1306 (U8g2, pełny bufor). uiTask rysuje klatkę co UI_FRAME_INTERVAL_MS na Core 0.
#include <Arduino.h>
#include "types.h"

void displayBegin();
void uiTask(void *parameter);
// Zmiana ekranu z animacją przejścia.
void setScreen(Screen nextScreen);
void showVolumeOverlay();
