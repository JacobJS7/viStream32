#include "ui/display.h"
#include <Wire.h>
#include <U8g2lib.h>
#include <vector>
#include "config.h"
#include "state.h"
#include "net/wifi_ap.h"
#include "storage/sd_fs.h"

// Ekran 128x64, monochromatyczny. Strefy Y każdego ekranu opisane przy funkcjach draw*.
// Czcionki *_tf mają tylko Latin-1 — polskie ogonki (poza ó) się nie wyświetlą.

static U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
static volatile uint32_t screenTransitionStart = 0;

// Nakładka na środku ekranu: głośność albo toast. Nowe wywołanie zastępuje poprzednie.
enum OverlayKind : uint8_t { OVERLAY_VOLUME, OVERLAY_TOAST };
static volatile uint32_t overlayUntil = 0;
static volatile OverlayKind overlayKind = OVERLAY_VOLUME;
static const char *volatile toastText = "";
static volatile ToastIcon toastIcon = TOAST_ICON_NONE;
static const char *volatile splashStep = "";

static constexpr uint32_t MARQUEE_HOLD_MS = 1200;
static constexpr uint32_t MARQUEE_STEP_MS = 40;
static constexpr int MARQUEE_GAP = 24;

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
	overlayKind = OVERLAY_VOLUME;
	overlayUntil = millis() + UI_VOLUME_OVERLAY_MS;
}

void showToast(const char *text, ToastIcon icon) {
	toastText = text;
	toastIcon = icon;
	overlayKind = OVERLAY_TOAST;
	overlayUntil = millis() + UI_TOAST_MS;
}

void setSplashStep(const char *step) {
	splashStep = step;
}

// ---------------------------------------------------------------- helpery

static uint32_t hashString(const String &s) {
	uint32_t h = 5381;
	for (size_t i = 0; i < s.length(); ++i) h = h * 33 + static_cast<uint8_t>(s[i]);
	return h;
}

// Przesunięcie przewijanego tekstu. Zmiana `key` (np. hash tekstu) zaczyna od początku.
struct Marquee {
	uint32_t key = 0;
	uint32_t startedAt = 0;
};

static int marqueeOffset(Marquee &m, uint32_t key, int textWidth, int boxWidth, uint32_t now) {
	if (key != m.key) {
		m.key = key;
		m.startedAt = now;
	}
	if (!UI_ANIMATIONS || textWidth <= boxWidth) return 0;
	uint32_t elapsed = now - m.startedAt;
	if (elapsed < MARQUEE_HOLD_MS) return 0;
	uint32_t scroll = (elapsed - MARQUEE_HOLD_MS) / MARQUEE_STEP_MS;
	if (scroll >= static_cast<uint32_t>(textWidth + MARQUEE_GAP)) {
		m.startedAt = now;  // druga kopia doszła do x=0 — pauza i od nowa
		return 0;
	}
	return static_cast<int>(scroll);
}

// Tekst w oknie [x, x+boxWidth): przewijany w kółko gdy się nie mieści.
static void drawMarquee(Marquee &m, const String &text, int x, int baseline, int boxWidth,
		int clipTop, int clipBottom, uint32_t now) {
	int width = u8g2.getUTF8Width(text.c_str());
	int offset = marqueeOffset(m, hashString(text), width, boxWidth, now);
	if (width <= boxWidth) {
		u8g2.drawUTF8(x, baseline, text.c_str());
		return;
	}
	u8g2.setClipWindow(x, clipTop, x + boxWidth, clipBottom);
	u8g2.drawUTF8(x - offset, baseline, text.c_str());
	u8g2.drawUTF8(x - offset + width + MARQUEE_GAP, baseline, text.c_str());
	u8g2.setMaxClipWindow();
}

// Usuwa ostatni znak UTF-8 (razem z bajtami kontynuacji).
static void popUtf8(String &s) {
	int i = static_cast<int>(s.length()) - 1;
	while (i > 0 && (static_cast<uint8_t>(s[i]) & 0xC0) == 0x80) --i;
	s.remove(max(0, i));
}

// Ucina tekst z ".." na końcu, żeby zmieścił się w `maxWidth` px bieżącą czcionką.
static String fitText(String text, int maxWidth) {
	if (u8g2.getUTF8Width(text.c_str()) <= maxWidth) return text;
	int dotsWidth = u8g2.getStrWidth("..");
	while (text.length() > 0 && u8g2.getUTF8Width(text.c_str()) + dotsWidth > maxWidth) popUtf8(text);
	return text + "..";
}

// Ścieżka skracana od lewej: "/muzyka/rock/live" -> "../rock/live".
static String fitPathLeft(String path, int maxWidth) {
	if (u8g2.getUTF8Width(path.c_str()) <= maxWidth) return path;
	int dotsWidth = u8g2.getStrWidth("..");
	while (path.length() > 1 && u8g2.getUTF8Width(path.c_str()) + dotsWidth > maxWidth) {
		int next = path.indexOf('/', 1);
		if (next > 0 && u8g2.getUTF8Width(path.substring(next).c_str()) + dotsWidth > maxWidth) {
			path = path.substring(next);  // całe segmenty, póki się da
		} else {
			path = path.substring(1);
			while (path.length() > 0 && (static_cast<uint8_t>(path[0]) & 0xC0) == 0x80) path = path.substring(1);
		}
	}
	return ".." + path;
}

static void drawCentered(const char *text, int baseline) {
	u8g2.drawUTF8((128 - u8g2.getUTF8Width(text)) / 2, baseline, text);
}

static String fileNameOf(const String &path) {
	int sep = path.lastIndexOf('/');
	return sep >= 0 ? path.substring(sep + 1) : path;
}

static String stripMp3Extension(String name) {
	String lower = name;
	lower.toLowerCase();
	if (lower.endsWith(".mp3")) name.remove(name.length() - 4);
	return name;
}

static float volumeFraction(float gain) {
	return constrain(gain / MAX_GAIN, 0.0f, 1.0f);
}

// ---------------------------------------------------------------- ikony
// (x, y) = lewy górny róg. Rozmiar w komentarzu.

static void iconFolder(int x, int y) {  // 7x7
	u8g2.drawHLine(x, y, 3);
	u8g2.drawFrame(x, y + 1, 7, 6);
}

static void iconNote(int x, int y) {  // 7x7
	u8g2.drawVLine(x + 4, y, 5);
	u8g2.drawHLine(x + 4, y, 3);
	u8g2.drawPixel(x + 6, y + 1);
	u8g2.drawBox(x + 2, y + 4, 3, 3);
}

static void iconParent(int x, int y) {  // 7x7, strzałka w górę
	u8g2.drawTriangle(x + 3, y, x, y + 3, x + 6, y + 3);
	u8g2.drawBox(x + 2, y + 3, 3, 4);
}

static void iconPlay(int x, int y) {  // 7x7
	u8g2.drawTriangle(x + 1, y, x + 1, y + 6, x + 6, y + 3);
}

static void iconPlayLarge(int x, int y) {  // 8x8
	u8g2.drawTriangle(x + 1, y, x + 1, y + 7, x + 7, y + 3);
}

static void iconPauseLarge(int x, int y) {  // 8x8
	u8g2.drawBox(x + 1, y, 2, 8);
	u8g2.drawBox(x + 5, y, 2, 8);
}

static void iconSpeaker(int x, int y) {  // 8x8
	u8g2.drawBox(x, y + 2, 3, 4);
	u8g2.drawTriangle(x + 2, y + 3, x + 6, y - 1, x + 6, y + 8);
	u8g2.drawTriangle(x + 2, y + 4, x + 6, y - 1, x + 6, y + 8);
}

static void iconShuffle(int x, int y) {  // 9x7
	u8g2.drawLine(x, y + 1, x + 7, y + 5);
	u8g2.drawLine(x, y + 5, x + 7, y + 1);
	u8g2.drawTriangle(x + 6, y - 1, x + 6, y + 3, x + 9, y + 1);
	u8g2.drawTriangle(x + 6, y + 3, x + 6, y + 7, x + 9, y + 5);
}

static void iconRepeat(int x, int y) {  // 9x7
	u8g2.drawRFrame(x, y + 1, 9, 6, 2);
	u8g2.setDrawColor(0);
	u8g2.drawVLine(x + 5, y + 1, 1);
	u8g2.setDrawColor(1);
	u8g2.drawTriangle(x + 4, y - 1, x + 4, y + 3, x + 7, y + 1);
}

static void iconWifi(int x, int y) {  // 13x7, fale nad punktem
	int cx = x + 6;
	int cy = y + 6;
	u8g2.drawBox(cx, cy, 1, 1);
	u8g2.drawCircle(cx, cy, 2, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
	u8g2.drawCircle(cx, cy, 4, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
	u8g2.drawCircle(cx, cy, 6, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
}

static void iconBattery(int x, int y, float voltage) {  // 14x6
	u8g2.drawFrame(x, y, 12, 6);
	u8g2.drawBox(x + 12, y + 1, 2, 4);
	float pct = constrain((voltage - BATTERY_DISPLAY_EMPTY_V) /
			(BATTERY_DISPLAY_FULL_V - BATTERY_DISPLAY_EMPTY_V), 0.0f, 1.0f);
	int fillWidth = static_cast<int>(pct * 10 + 0.5f);
	if (fillWidth > 0) u8g2.drawBox(x + 1, y + 1, fillWidth, 4);
}

static void iconSdCard(int x, int y) {  // 18x24
	u8g2.drawLine(x + 5, y, x + 17, y);
	u8g2.drawLine(x + 17, y, x + 17, y + 23);
	u8g2.drawLine(x + 17, y + 23, x, y + 23);
	u8g2.drawLine(x, y + 23, x, y + 5);
	u8g2.drawLine(x, y + 5, x + 5, y);
	for (int i = 0; i < 4; ++i) u8g2.drawBox(x + 5 + i * 3, y + 3, 2, 5);
}

static void iconCross(int x, int y) {  // 9x9, gruba linia
	u8g2.drawLine(x, y, x + 8, y + 8);
	u8g2.drawLine(x + 1, y, x + 8, y + 7);
	u8g2.drawLine(x, y + 8, x + 8, y);
	u8g2.drawLine(x + 1, y + 8, x + 8, y + 1);
}

static void iconDownload(int cx, int y) {  // 11x10, środek w cx
	u8g2.drawBox(cx - 1, y, 3, 5);
	u8g2.drawTriangle(cx - 4, y + 5, cx + 4, y + 5, cx, y + 9);
	u8g2.drawHLine(cx - 5, y + 9, 11);
}

static void drawToastIcon(ToastIcon icon, int x, int y) {
	switch (icon) {
		case TOAST_ICON_PLAY: iconPlay(x + 1, y); break;
		case TOAST_ICON_SHUFFLE: iconShuffle(x, y); break;
		case TOAST_ICON_REPEAT: iconRepeat(x, y); break;
		default: break;
	}
}

// ---------------------------------------------------------------- Now Playing
// Y0–7 status | Y9 kropki | Y12–24 tytuł | Y27–34 artysta | Y38–43 postęp
// Y46–53 czas | Y56–63 play/pause, głośność, tryb

static void drawStatusBar(size_t trackIndex, size_t trackCount, uint32_t now) {
	u8g2.setFont(u8g2_font_5x7_tf);
	iconNote(0, 0);
	char counterText[16];
	snprintf(counterText, sizeof(counterText), "%02u/%02u",
			static_cast<unsigned>(trackCount == 0 ? 0 : trackIndex + 1), static_cast<unsigned>(trackCount));
	u8g2.drawStr(10, 7, counterText);

	int x = 128;
	bool batteryLow = batteryVoltage < BATTERY_WARNING_VOLTAGE;
	bool blinkOff = batteryLow && ((now / 500) % 2 == 0);
	x -= 14;
	if (!blinkOff) iconBattery(x, 1, batteryVoltage);
	if (isWifiClientConnected()) {
		x -= 16;
		iconWifi(x, 0);
	}

	for (int px = 0; px < 128; px += 2) u8g2.drawPixel(px, 9);
}

bool drawNowPlaying() {
	uint32_t now = millis();
	String trackPath;
	size_t trackIndex = 0;
	size_t trackCount = 0;
	RepeatMode mode = REPEAT_OFF;
	bool shuffle = false;
	if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
		trackIndex = currentTrack;
		trackCount = tracks.size();
		if (currentTrack < tracks.size()) trackPath = tracks[currentTrack];
		mode = repeatMode;
		shuffle = shuffleEnabled;
		xSemaphoreGive(stateMutex);
	} else {
		return false;
	}
	String tagTitle;
	String tagArtist;
	bool paused = false;
	float gain = currentGain;
	if (xSemaphoreTake(audioMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
		paused = playbackPaused || playbackStopped;
		gain = currentGain;
		tagTitle = currentTrackTitle;
		tagArtist = currentTrackArtist;
		xSemaphoreGive(audioMutex);
	}

	// Bez tagów: tytuł = nazwa pliku bez .mp3, artysta = folder.
	String title = tagTitle;
	String artist = tagArtist;
	if (trackPath.length() == 0) {
		title = "No track";
		artist = "";
	} else {
		if (title.length() == 0) title = stripMp3Extension(fileNameOf(trackPath));
		if (artist.length() == 0) {
			String folder = fileNameOf(parentPath(trackPath));
			artist = folder.length() > 0 ? folder : String("/");
		}
	}

	uint32_t durationSec = currentTrackDurationSec;
	if (durationSec == 0) durationSec = 1;
	uint32_t positionSec = min(static_cast<uint32_t>(currentPositionSec), durationSec);
	float progress = static_cast<float>(positionSec) / durationSec;

	u8g2.clearBuffer();
	drawStatusBar(trackIndex, trackCount, now);

	// Tytuł Y12–24 (7x13B, marquee), artysta Y27–34 (5x7)
	static Marquee titleMarquee;
	static Marquee artistMarquee;
	u8g2.setFont(u8g2_font_7x13B_tf);
	drawMarquee(titleMarquee, title, 0, 22, 128, 12, 25, now);
	u8g2.setFont(u8g2_font_5x7_tf);
	drawMarquee(artistMarquee, artist, 0, 33, 128, 27, 35, now);

	// Postęp Y38–43
	u8g2.drawRFrame(0, 38, 128, 6, 2);
	int fillW = static_cast<int>(122 * progress);
	if (fillW > 0) u8g2.drawBox(2, 40, fillW, 2);
	u8g2.drawDisc(min(125, max(2, 2 + fillW)), 40, 2);

	// Czas Y46–53, wyśrodkowany "01:22 / 04:32"
	char timeText[24];
	snprintf(timeText, sizeof(timeText), "%02lu:%02lu / %02lu:%02lu",
			static_cast<unsigned long>(positionSec / 60), static_cast<unsigned long>(positionSec % 60),
			static_cast<unsigned long>(durationSec / 60), static_cast<unsigned long>(durationSec % 60));
	drawCentered(timeText, 52);

	// Dół Y56–63: stan, głośność (10 segmentów), tryb
	if (paused) iconPauseLarge(0, 56);
	else iconPlayLarge(0, 56);

	iconSpeaker(13, 56);
	int level = static_cast<int>(volumeFraction(gain) * 10 + 0.5f);
	for (int i = 0; i < 10; ++i) {
		int sx = 25 + i * 6;
		if (i < level) u8g2.drawBox(sx, 57, 4, 6);
		else u8g2.drawFrame(sx, 57, 4, 6);
	}

	const char *modeLabel = nullptr;
	if (shuffle) modeLabel = "SHUF";
	else if (mode == REPEAT_ONE) modeLabel = "RPT1";
	else if (mode == REPEAT_ALL) modeLabel = "RALL";
	if (modeLabel) {
		int boxWidth = u8g2.getStrWidth(modeLabel) + 5;
		int bx = 128 - boxWidth;
		u8g2.drawRBox(bx, 56, boxWidth, 8, 1);
		u8g2.setDrawColor(0);
		u8g2.drawStr(bx + 3, 63, modeLabel);
		u8g2.setDrawColor(1);
	}
	return true;
}

// ---------------------------------------------------------------- Browser
// Y0–8 ścieżka + licznik | Y10 linia | 4 wiersze po 13 px od Y12 | suwak X125–127

static constexpr int BROWSER_ROWS = 4;
static constexpr int BROWSER_ROW_H = 13;
static constexpr int BROWSER_LIST_TOP = 12;

bool drawBrowser() {
	uint32_t now = millis();
	String header = "/";
	String playingPath;
	size_t selection = 0;
	size_t offset = 0;
	std::vector<DirectoryEntry> entries;
	if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
		header = currentDirectory;
		selection = browserSelection;
		offset = browserOffset;
		entries = directoryEntries;
		if (currentTrack < tracks.size()) playingPath = tracks[currentTrack];
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
	highlightRow = constrain(highlightRow, 0.0f, static_cast<float>(BROWSER_ROWS - 1));

	u8g2.clearBuffer();

	// Nagłówek Y0–8
	u8g2.setFont(u8g2_font_5x7_tf);
	char counterText[16];
	snprintf(counterText, sizeof(counterText), "%u/%u",
			static_cast<unsigned>(entries.empty() ? 0 : selection + 1), static_cast<unsigned>(entries.size()));
	int counterWidth = u8g2.getStrWidth(counterText);
	u8g2.drawStr(128 - counterWidth, 7, counterText);
	iconFolder(0, 1);
	u8g2.setFont(u8g2_font_6x10_tf);
	String shownPath = fitPathLeft(header, 128 - 10 - counterWidth - 4);
	u8g2.drawUTF8(10, 8, shownPath.c_str());
	u8g2.drawHLine(0, 10, 128);

	bool needsScrollbar = entries.size() > static_cast<size_t>(BROWSER_ROWS);
	int rowWidth = needsScrollbar ? 123 : 128;
	int textX = 12;
	int textWidth = rowWidth - textX - 2;

	if (entries.empty()) {
		drawCentered("(empty)", 40);
	}

	static Marquee selectionMarquee;
	for (int row = 0; row < BROWSER_ROWS; ++row) {
		size_t index = offset + row;
		if (index >= entries.size()) break;
		const DirectoryEntry &entry = entries[index];
		int top = BROWSER_LIST_TOP + row * BROWSER_ROW_H;
		int iconY = top + 3;
		int baseline = top + 10;

		if (entry.isParent) {
			iconParent(2, iconY);
		} else if (entry.isDirectory) {
			iconFolder(2, iconY);
		} else if (playingPath.length() > 0 && joinPath(header, entry.name) == playingPath) {
			iconPlay(2, iconY);
		} else {
			iconNote(2, iconY);
		}

		String label = entry.isParent ? String("..") : entry.name;
		if (index == selection) {
			drawMarquee(selectionMarquee, label, textX, baseline, textWidth, top, top + BROWSER_ROW_H - 1, now);
		} else {
			label = fitText(label, textWidth);
			u8g2.drawUTF8(textX, baseline, label.c_str());
		}
	}

	// Zaznaczenie: XOR odwraca to, co już narysowane — działa też w trakcie animacji.
	if (!entries.empty()) {
		int highlightY = BROWSER_LIST_TOP + static_cast<int>(highlightRow * BROWSER_ROW_H + 0.5f);
		u8g2.setDrawColor(2);
		u8g2.drawBox(0, highlightY, rowWidth, BROWSER_ROW_H);
		u8g2.setDrawColor(1);
	}

	// Suwak X125–127
	if (needsScrollbar) {
		const int trackTop = BROWSER_LIST_TOP;
		const int trackHeight = 64 - BROWSER_LIST_TOP;
		for (int py = trackTop; py < 64; py += 2) u8g2.drawPixel(126, py);
		int thumbHeight = max(4, trackHeight * BROWSER_ROWS / static_cast<int>(entries.size()));
		size_t maxOffset = entries.size() - BROWSER_ROWS;
		int thumbY = trackTop + static_cast<int>((trackHeight - thumbHeight) * min(offset, maxOffset) / maxOffset);
		u8g2.drawBox(125, thumbY, 3, thumbHeight);
	}
	return true;
}

// ---------------------------------------------------------------- nakładka
// Ramka X14–113, Y14–49

static void drawOverlay(uint32_t now) {
	if (static_cast<int32_t>(overlayUntil - now) <= 0) return;
	u8g2.setDrawColor(0);
	u8g2.drawRBox(14, 14, 100, 36, 3);
	u8g2.setDrawColor(1);
	u8g2.drawRFrame(14, 14, 100, 36, 3);
	u8g2.setFont(u8g2_font_6x10_tf);

	if (overlayKind == OVERLAY_VOLUME) {
		float fraction = volumeFraction(currentGain);  // odczyt 32-bit, bez blokady
		iconSpeaker(20, 20);
		u8g2.drawStr(32, 28, "VOLUME");
		char pctText[8];
		snprintf(pctText, sizeof(pctText), "%d%%", static_cast<int>(fraction * 100.0f + 0.5f));
		u8g2.drawStr(108 - u8g2.getStrWidth(pctText), 28, pctText);
		int level = static_cast<int>(fraction * 20 + 0.5f);
		for (int i = 0; i < 20; ++i) {
			int sx = 24 + i * 4;
			if (i < level) u8g2.drawBox(sx, 34, 3, 8);
			else u8g2.drawHLine(sx, 41, 3);
		}
	} else {
		const char *text = toastText;
		ToastIcon icon = toastIcon;
		int textWidth = u8g2.getStrWidth(text);
		int iconWidth = icon == TOAST_ICON_NONE ? 0 : 13;
		int x = 64 - (iconWidth + textWidth) / 2;
		drawToastIcon(icon, x, 28);
		u8g2.drawStr(x + iconWidth, 35, text);
	}
}

// ---------------------------------------------------------------- ekrany systemowe

// Y0–12 nagłówek | Y18–27 strzałka | Y30–39 pasek z % | Y44–50 kB | Y55–63 ostrzeżenie
void drawOta(uint32_t now) {
	uint8_t progress = min(static_cast<uint8_t>(100), static_cast<uint8_t>(otaProgress));
	u8g2.clearBuffer();

	u8g2.drawBox(0, 0, 128, 13);
	u8g2.setDrawColor(0);
	u8g2.setFont(u8g2_font_7x13B_tf);
	drawCentered("UPDATING", 11);
	u8g2.setDrawColor(1);

	if (progress >= 100 || (now / 400) % 2 == 0) iconDownload(64, 18);

	u8g2.drawRFrame(4, 30, 120, 10, 2);
	int fillW = 116 * progress / 100;
	if (fillW > 0) u8g2.drawBox(6, 32, fillW, 6);
	u8g2.setFont(u8g2_font_5x7_tf);
	char pctText[8];
	snprintf(pctText, sizeof(pctText), "%u%%", progress);
	u8g2.setFontMode(1);
	u8g2.setDrawColor(2);  // XOR: procent czytelny na wypełnionej i pustej części
	drawCentered(pctText, 38);
	u8g2.setDrawColor(1);
	u8g2.setFontMode(0);

	uint32_t totalKb = otaTotalKb;
	if (totalKb > 0) {
		char kbText[24];
		snprintf(kbText, sizeof(kbText), "%lu / %lu kB",
				static_cast<unsigned long>(min(static_cast<uint32_t>(otaWrittenKb), totalKb)),
				static_cast<unsigned long>(totalKb));
		drawCentered(kbText, 50);
	}

	u8g2.drawBox(0, 55, 128, 9);
	u8g2.setDrawColor(0);
	drawCentered("DO NOT POWER OFF", 62);
	u8g2.setDrawColor(1);
	u8g2.sendBuffer();
}

// Y4–27 ikona karty | Y33–42 napis | Y52–58 podpowiedź (miga)
void drawNoSd(uint32_t now) {
	u8g2.clearBuffer();
	iconSdCard(48, 4);
	iconCross(71, 11);
	u8g2.setFont(u8g2_font_6x10_tf);
	drawCentered("NO SD CARD", 42);
	if ((now / 600) % 2 == 0) {
		u8g2.setFont(u8g2_font_5x7_tf);
		drawCentered("insert card...", 58);
	}
	u8g2.sendBuffer();
}

// Y6–20 logo | Y24–30 wersja | Y40–45 pasek | Y54–61 krok startu
void drawSplash(uint32_t now) {
	(void)now;
	static float shownProgress = 0.0f;
	float target = min(100.0f, static_cast<float>(splashProgress));
	if (!UI_ANIMATIONS) shownProgress = target;
	else shownProgress += (target - shownProgress) * 0.25f;

	u8g2.clearBuffer();
	u8g2.setFont(u8g2_font_9x15B_tr);
	drawCentered("viStream32", 18);
	u8g2.setFont(u8g2_font_5x7_tf);
	drawCentered("MP3 PLAYER  v" FW_VERSION, 30);

	u8g2.drawRFrame(14, 40, 100, 6, 2);
	int width = static_cast<int>(96 * shownProgress / 100.0f + 0.5f);
	if (width > 0) u8g2.drawBox(16, 42, width, 2);

	drawCentered(splashStep, 61);
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
		if (otaInProgress) {
			drawOta(now);
		} else if (sdMissing) {
			drawNoSd(now);
		} else if (!uiReady) {
			drawSplash(now);
		} else {
			bool drew = currentScreen == SCREEN_BROWSER ? drawBrowser() : drawNowPlaying();
			if (drew) {
				drawOverlay(now);
				drawScreenTransition(now);  // musi być przed sendBuffer()
				u8g2.sendBuffer();
			}
		}
		vTaskDelay(pdMS_TO_TICKS(UI_FRAME_INTERVAL_MS));
	}
}
