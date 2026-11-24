#pragma once
// OLED SSD1306 (U8g2, pełny bufor). uiTask rysuje klatkę co UI_FRAME_INTERVAL_MS na Core 0.
#include <Arduino.h>
#include "types.h"

enum ToastIcon : uint8_t {
	TOAST_ICON_NONE,
	TOAST_ICON_PLAY,
	TOAST_ICON_SHUFFLE,
	TOAST_ICON_REPEAT
};

void displayBegin();
void uiTask(void *parameter);
// Zmiana ekranu z animacją przejścia.
void setScreen(Screen nextScreen);
// Nakładka z paskiem głośności (UI_VOLUME_OVERLAY_MS).
void showVolumeOverlay();
// Krótki komunikat w ramce na środku ekranu (UI_TOAST_MS). `text` musi być stałym literałem.
void showToast(const char *text, ToastIcon icon = TOAST_ICON_NONE);
// Napis pod paskiem postępu na ekranie startowym. `step` musi być stałym literałem.
void setSplashStep(const char *step);
