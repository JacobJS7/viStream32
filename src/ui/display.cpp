#include "ui/display.h"
#include <Wire.h>
#include <U8g2lib.h>
#include <vector>
#include "config.h"
#include "state.h"
#include "net/wifi_ap.h"

static U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
static volatile uint32_t volumeOverlayUntil = 0;
static volatile uint32_t screenTransitionStart = 0;

void displayBegin() {
	Wire.begin(I2C_SDA, I2C_SCL);
	u8g2.begin();
}

void setScreen(Screen nextScreen) {
	if (currentScreen == nextScreen) return;
	currentScreen = nextScreen;
	screenTransitionStart = millis();
}

void showVolumeOverlay() {
	volumeOverlayUntil = millis() + UI_VOLUME_OVERLAY_MS;
}

// Rysuje klatkę do bufora; sendBuffer() robi uiTask. false = klatka pominięta.
bool drawNowPlaying() {
	uint32_t now = millis();
	String title;
	size_t trackIndex = 0;
	size_t trackCount = 0;
	bool paused = false;
	float displayGain = currentGain;
	if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
		trackIndex = currentTrack;
		trackCount = tracks.size();
		title = (tracks.empty() || currentTrack >= tracks.size()) ? String("Brak utworu") : tracks[currentTrack];
		xSemaphoreGive(stateMutex);
	} else {
		return false;
	}
	if (xSemaphoreTake(audioMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
		paused = playbackPaused || playbackStopped;
		displayGain = currentGain;
		xSemaphoreGive(audioMutex);
	} else {
		paused = false;
	}
	int separator = title.lastIndexOf('/');
	if (separator >= 0) title = title.substring(separator + 1);

	static size_t marqueeTrack = static_cast<size_t>(-1);
	static int marqueeOffset = 0;
	static uint32_t marqueeChangedAt = 0;
	if (trackIndex != marqueeTrack) {
		marqueeTrack = trackIndex;
		marqueeOffset = 0;
		marqueeChangedAt = now;
	}

	uint32_t positionSec = currentPositionSec;
	uint32_t durationSec = currentTrackDurationSec;
	if (durationSec == 0) durationSec = 1;
	uint32_t displayPosition = min(positionSec, durationSec);
	float progress = min(1.0f, static_cast<float>(positionSec) / durationSec);

	u8g2.clearBuffer();

	// --- Pasek statusu (góra) ---
	u8g2.setFont(u8g2_font_5x7_tf);
	char counterText[12];
	snprintf(counterText, sizeof(counterText), "%02u/%02u",
			static_cast<unsigned>(trackIndex + 1), static_cast<unsigned>(max((size_t)1, trackCount)));
	u8g2.drawStr(0, 7, counterText);

	int badgeX = 128;
	// Plakietka trybu (shuffle/repeat) jako odwrócony box, wyrównana do prawej
	const char* modeLabel = nullptr;
	if (shuffleEnabled) modeLabel = "S";
	else if (repeatMode == REPEAT_ONE) modeLabel = "R1";
	else if (repeatMode == REPEAT_ALL) modeLabel = "RA";
	if (modeLabel) {
		int labelWidth = u8g2.getStrWidth(modeLabel);
		int boxWidth = labelWidth + 4;
		badgeX -= boxWidth;
		u8g2.drawRBox(badgeX, 0, boxWidth, 8, 1);
		u8g2.setDrawColor(0);
		u8g2.drawStr(badgeX + 2, 7, modeLabel);
		u8g2.setDrawColor(1);
		badgeX -= 3;
	}

	// Ikona Wi-Fi (fale) — tylko gdy klient połączony
	if (isWifiClientConnected()) {
		badgeX -= 11;
		u8g2.drawCircle(badgeX + 4, 4, 1);
		u8g2.drawCircle(badgeX + 4, 4, 3);
		badgeX -= 3;
	}

	// Ikona baterii — realny procent wypełnienia, miga tylko gdy niski poziom
	bool batteryLow = batteryVoltage < BATTERY_WARNING_VOLTAGE;
	bool blinkOff = batteryLow && ((now / 500) % 2 == 0);
	if (!blinkOff) {
		badgeX -= 15;
		int bx = badgeX;
		u8g2.drawFrame(bx, 1, 12, 6);
		u8g2.drawBox(bx + 12, 2, 2, 4);
		float pct = constrain((batteryVoltage - BATTERY_DISPLAY_EMPTY_V) /
				(BATTERY_DISPLAY_FULL_V - BATTERY_DISPLAY_EMPTY_V), 0.0f, 1.0f);
		int fillWidth = static_cast<int>(pct * 10);
		if (fillWidth > 0) u8g2.drawBox(bx + 1, 2, fillWidth, 4);
	}

	u8g2.drawHLine(0, 10, 128);

	// --- Tytuł (pogrubiony, większy) ---
	u8g2.setFont(u8g2_font_7x13B_tf);  // jeśli brak w wersji u8g2, zamień na u8g2_font_helvB08_tr
	int titleWidth = u8g2.getUTF8Width(title.c_str());
	int titleX = 0;
	if (titleWidth > 128) {
		if (UI_ANIMATIONS && now - marqueeChangedAt > 1200) {
			uint32_t cycle = now - marqueeChangedAt - 1200;
			if (cycle < static_cast<uint32_t>(titleWidth * 40 + 500)) {
				marqueeOffset = min(titleWidth, static_cast<int>(cycle / 40));
			} else {
				marqueeChangedAt = now;
				marqueeOffset = 0;
			}
		}
		titleX = -marqueeOffset;
	}
	if (titleWidth <= 128) {
		u8g2.drawUTF8(0, 26, title.c_str());
	} else {
		u8g2.drawUTF8(titleX, 26, title.c_str());
		if (marqueeOffset >= titleWidth) u8g2.drawUTF8(128, 26, title.c_str());
	}

	// --- Pasek postępu ze "scrubberem" ---
	u8g2.setFont(u8g2_font_5x7_tf);
	u8g2.drawRFrame(0, 32, 128, 6, 2);
	int fillW = max(2, static_cast<int>(124 * progress));
	u8g2.drawRBox(2, 34, fillW, 2, 1);
	int scrubberX = 2 + fillW;
	u8g2.drawDisc(min(126, scrubberX), 35, 2);

	char timeText[20];
	snprintf(timeText, sizeof(timeText), "%02lu:%02lu / %02lu:%02lu",
			displayPosition / 60, displayPosition % 60, durationSec / 60, durationSec % 60);
	int timeWidth = u8g2.getStrWidth(timeText);
	u8g2.drawStr((128 - timeWidth) / 2, 46, timeText);

	// --- Dolny wiersz: play/pause + equalizer z peak-hold ---
	u8g2.drawCircle(9, 55, 8);
	if (paused) {
		u8g2.drawBox(6, 51, 2, 8);
		u8g2.drawBox(10, 51, 2, 8);
	} else {
		u8g2.drawTriangle(6, 50, 6, 60, 14, 55);
	}

	if (!paused && UI_ANIMATIONS) {
		static float peaks[5] = {0, 0, 0, 0, 0};
		for (uint8_t bar = 0; bar < 5; ++bar) {
			uint8_t height = 4 + ((now / 140 + bar * 3) % 10);
			int barX = 46 + bar * 12;
			u8g2.drawBox(barX, 62 - height, 6, height);
			if (height > peaks[bar]) peaks[bar] = height;
			else peaks[bar] = max(0.0f, peaks[bar] - 0.3f);
			u8g2.drawHLine(barX, 62 - static_cast<int>(peaks[bar]) - 1, 6);
		}
	}

	if (static_cast<int32_t>(volumeOverlayUntil - now) > 0) {
		char volumeText[16];
		snprintf(volumeText, sizeof(volumeText), "Vol: %d%%", static_cast<int>(displayGain * 100.0f / MAX_GAIN));
		u8g2.setDrawColor(0);
		u8g2.drawRBox(30, 44, 68, 16, 2);
		u8g2.setDrawColor(1);
		u8g2.drawRFrame(30, 44, 68, 16, 2);
		u8g2.setFont(u8g2_font_6x10_tf);
		int vw = u8g2.getStrWidth(volumeText);
		u8g2.drawStr(30 + (68 - vw) / 2, 55, volumeText);
	}
	return true;
}

// Rysuje klatkę do bufora; sendBuffer() robi uiTask. false = klatka pominięta.
bool drawBrowser() {
	uint32_t now = millis();
	String header = "/";
	size_t selection = 0;
	size_t offset = 0;
	std::vector<DirectoryEntry> entries;
	if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
		header = currentDirectory;
		selection = browserSelection;
		offset = browserOffset;
		entries = directoryEntries;
		xSemaphoreGive(stateMutex);
	} else {
		return false;
	}
	static float highlightRow = 0.0f;
	static uint32_t lastFrameMillis = 0;
	uint32_t frameDelta = lastFrameMillis == 0 ? UI_FRAME_INTERVAL_MS : now - lastFrameMillis;
	lastFrameMillis = now;
	float targetRow = static_cast<float>(selection - offset);
	if (!UI_ANIMATIONS) highlightRow = targetRow;
	else highlightRow += (targetRow - highlightRow) * min(1.0f, frameDelta / 80.0f);
	if (highlightRow < 0) highlightRow = 0;
	if (highlightRow > 3) highlightRow = 3;

	u8g2.clearBuffer();
	u8g2.setFont(u8g2_font_6x10_tf);
	if (header.length() > 20) header = header.substring(header.length() - 20);
	u8g2.drawStr(0, 9, header.c_str());
	u8g2.drawHLine(0, 11, 128);

	for (size_t row = 0; row < 4; ++row) {
		size_t index = offset + row;
		if (index >= entries.size()) break;
		int baseline = 21 + static_cast<int>(row) * 11;
		if (static_cast<int>(row) == static_cast<int>(highlightRow + 0.5f)) {
			u8g2.setDrawColor(1);
			u8g2.drawBox(0, 21 + static_cast<int>(highlightRow) * 11 - 9, 128, 11);
			u8g2.setDrawColor(0);
		}

		DirectoryEntry entry = entries[index];
		String label = entry.isParent ? ".." : (entry.isDirectory ? "[D] " : "    ") + entry.name;
		if (label.length() > 20) label = label.substring(0, 20);
		u8g2.drawStr(0, baseline, label.c_str());
		u8g2.setDrawColor(1);
	}
	return true;
}

void drawNoSd(uint32_t now) {
	u8g2.clearBuffer();
	u8g2.setFont(u8g2_font_9x15B_tr);
	const char *title = "No SD Card";
	u8g2.drawStr((128 - u8g2.getStrWidth(title)) / 2, 28, title);
	u8g2.setFont(u8g2_font_5x7_tf);
	if ((now / 600) % 2 == 0) {
		const char *hint = "insert card...";
		u8g2.drawStr((128 - u8g2.getStrWidth(hint)) / 2, 46, hint);
	}
	u8g2.sendBuffer();
}

void drawSplash(uint32_t now) {
	u8g2.clearBuffer();
	u8g2.setFont(u8g2_font_9x15B_tr);  // jeśli brak, zamień na u8g2_font_7x13B_tf
	int textWidth = u8g2.getStrWidth("vistream32");
	u8g2.drawStr((128 - textWidth) / 2, 24, "vistream32");

	u8g2.setFont(u8g2_font_5x7_tf);
	u8g2.drawStr(35, 34, "loading player");

	u8g2.drawRFrame(14, 42, 100, 8, 2);
	uint8_t width = static_cast<uint8_t>(min(96UL, static_cast<uint32_t>(splashProgress) * 96 / 100));
	if (UI_ANIMATIONS) width = max(width, static_cast<uint8_t>((now / 80) % 97));
	if (width > 2) u8g2.drawRBox(16, 44, width, 4, 1);
	u8g2.sendBuffer();
}

void drawScreenTransition(uint32_t now) {
	if (!UI_ANIMATIONS || screenTransitionStart == 0) return;
	uint32_t elapsed = now - screenTransitionStart;
	if (elapsed >= UI_TRANSITION_MS) return;
	uint8_t remaining = static_cast<uint8_t>(128 - (elapsed * 128 / UI_TRANSITION_MS));
	u8g2.setDrawColor(0);
	u8g2.drawBox(0, 0, remaining, 64);
	u8g2.setDrawColor(1);
}

void uiTask(void *parameter) {
	(void)parameter;
	for (;;) {
		uint32_t now = millis();
		if (sdMissing) {
			drawNoSd(now);
		} else if (!uiReady) {
			drawSplash(now);
		} else {
			bool drew = currentScreen == SCREEN_BROWSER ? drawBrowser() : drawNowPlaying();
			if (drew) {
				drawScreenTransition(now);  // musi być przed sendBuffer()
				u8g2.sendBuffer();
			}
		}
		vTaskDelay(pdMS_TO_TICKS(UI_FRAME_INTERVAL_MS));
	}
}
