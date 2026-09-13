#include <Arduino.h>
#include <ESP32I2SAudio.h>
#include <SD.h>
#include <BackgroundAudio.h>
#include <SPI.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <Preferences.h>
#include <vector>

// I2S DOUT uses GPIO 27; I2C remains on SDA 21 and SCL 22.
#define I2S_BCLK  26
#define I2S_LRC   25
#define I2S_DOUT  27
#define I2C_SDA   21
#define I2C_SCL   22

#define SD_CS     5
#define NEXT_BUTTON_PIN 4
#define PREV_BUTTON_PIN 33
#define PLAY_BUTTON_PIN 32
#define LONG_PRESS_MS 800
#define PLAY_DOUBLE_CLICK_MS 400
#define SEEK_BYTES_10_SECONDS 160000UL
#define STATE_SAVE_INTERVAL_MS 12000UL
#define APPROXIMATE_MP3_BYTES_PER_SECOND 16000UL
#define UI_ANIMATIONS 1
#define UI_FRAME_INTERVAL_MS 40UL
#define UI_TRANSITION_MS 240UL
#define UI_VOLUME_OVERLAY_MS 1500UL
#define BATTERY_WARNING_VOLTAGE 3.4f
#define DEFAULT_GAIN 0.005f
#define MAX_GAIN 0.05f
#define GAIN_STEP 0.0025f

enum Screen : uint8_t {
	SCREEN_BROWSER,
	SCREEN_NOWPLAYING
};

struct DirectoryEntry {
	String name;
	bool isDirectory;
	bool isParent;
};

ESP32I2SAudio audio(I2S_BCLK, I2S_LRC, I2S_DOUT);
BackgroundAudioMP3 mp3(audio);
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

File f;
uint8_t filebuff[512];
std::vector<String> tracks;
size_t currentTrack = 0;
std::vector<DirectoryEntry> directoryEntries;
String currentDirectory = "/";
size_t browserSelection = 0;
size_t browserOffset = 0;
Screen currentScreen = SCREEN_BROWSER;
volatile bool displayDirty = true;

volatile bool nextPressed = false;
volatile bool prevPressed = false;
volatile bool playPressed = false;
volatile bool nextReleased = false;
volatile bool prevReleased = false;
volatile bool playReleased = false;
volatile uint32_t nextPressMillis = 0;
volatile uint32_t prevPressMillis = 0;
volatile uint32_t playPressMillis = 0;
bool muteComboTriggered = false;
uint32_t muteComboStartedAt = 0;
bool playModifierUsed = false;
bool playClickPending = false;
uint32_t lastPlayReleaseMillis = 0;
unsigned long lastButtonMillis = 0;
SemaphoreHandle_t audioMutex;
SemaphoreHandle_t stateMutex;
SemaphoreHandle_t sdMutex;
TaskHandle_t audioTaskHandle = nullptr;
TaskHandle_t uiTaskHandle = nullptr;
size_t displayedTrack = static_cast<size_t>(-1);
bool displayedPaused = false;
volatile bool playbackPaused = false;
volatile bool playbackStopped = false;
float currentGain = DEFAULT_GAIN;
float gainBeforeMute = DEFAULT_GAIN;
bool muted = false;
Preferences preferences;
bool shuffleEnabled = false;
volatile uint32_t currentPositionSec = 0;
uint32_t trackPositionBaseSec = 0;
uint32_t playbackReferenceMillis = 0;
volatile bool stateSaveRequested = false;
uint32_t lastStateSaveMillis = 0;
volatile uint8_t splashProgress = 0;
volatile bool uiReady = false;
volatile uint32_t volumeOverlayUntil = 0;
volatile uint32_t uiStateChangeMillis = 0;
volatile uint32_t currentTrackDurationSec = 1;
volatile uint32_t screenTransitionStart = 0;
Screen previousScreen = SCREEN_BROWSER;
float batteryVoltage = 4.2f;

bool isWifiClientConnected();
void showVolumeOverlay();

void IRAM_ATTR handleNextISR() {
	if (digitalRead(NEXT_BUTTON_PIN) == LOW) {
		nextPressed = true;
		nextPressMillis = millis();
	} else {
		nextReleased = true;
	}
}

void IRAM_ATTR handlePrevISR() {
	if (digitalRead(PREV_BUTTON_PIN) == LOW) {
		prevPressed = true;
		prevPressMillis = millis();
	} else {
		prevReleased = true;
	}
}

void IRAM_ATTR handlePlayISR() {
	if (digitalRead(PLAY_BUTTON_PIN) == LOW) {
		playPressed = true;
		playPressMillis = millis();
	} else {
		playReleased = true;
	}
}

void setScreen(Screen nextScreen) {
	if (currentScreen == nextScreen) return;
	previousScreen = currentScreen;
	currentScreen = nextScreen;
	screenTransitionStart = millis();
	uiStateChangeMillis = millis();
	displayDirty = true;
}

String joinPath(const String &base, const String &name) {
	if (base == "/") return "/" + name;
	return base + "/" + name;
}

String parentPath(const String &path) {
	if (path == "/") return "/";
	int separator = path.lastIndexOf('/');
	if (separator <= 0) return "/";
	return path.substring(0, separator);
}

bool scanDirectory(const char *path) {
	std::vector<DirectoryEntry> scannedEntries;
	File dir;

	xSemaphoreTake(sdMutex, portMAX_DELAY);
	dir = SD.open(path);
	if (!dir || !dir.isDirectory()) {
		xSemaphoreGive(sdMutex);
		return false;
	}

	if (String(path) != "/") {
		scannedEntries.push_back({"..", true, true});
	}

	File entry = dir.openNextFile();
	while (entry) {
		String name = entry.name();
		int separator = name.lastIndexOf('/');
		if (separator >= 0) name = name.substring(separator + 1);
		if (entry.isDirectory() || name.endsWith(".mp3") || name.endsWith(".MP3")) {
			scannedEntries.push_back({name, entry.isDirectory(), false});
		}
		entry.close();
		entry = dir.openNextFile();
	}
	dir.close();
	xSemaphoreGive(sdMutex);

	directoryEntries = scannedEntries;
	return true;
}

void requestStateSave() {
	stateSaveRequested = true;
}

void updatePlaybackPosition() {
	if (playbackPaused || playbackStopped) return;
	uint32_t elapsed = (millis() - playbackReferenceMillis) / 1000;
	xSemaphoreTake(stateMutex, portMAX_DELAY);
	currentPositionSec = trackPositionBaseSec + elapsed;
	xSemaphoreGive(stateMutex);
}

void resetPlaybackPosition(uint32_t positionSec = 0) {
	trackPositionBaseSec = positionSec;
	playbackReferenceMillis = millis();
	currentPositionSec = positionSec;
}

void saveStateToNvs() {
	String folder;
	uint16_t trackIndex;
	uint32_t position;
	float gain;
	bool shuffle;

	xSemaphoreTake(stateMutex, portMAX_DELAY);
	folder = currentDirectory;
	trackIndex = static_cast<uint16_t>(currentTrack);
	position = currentPositionSec;
	shuffle = shuffleEnabled;
	xSemaphoreGive(stateMutex);
	xSemaphoreTake(audioMutex, portMAX_DELAY);
	gain = currentGain;
	xSemaphoreGive(audioMutex);

	preferences.putString("lastFolder", folder);
	preferences.putUShort("lastTrackIndex", trackIndex);
	preferences.putUInt("lastPositionSec", position);
	preferences.putFloat("lastVolume", gain);
	preferences.putBool("shuffle", shuffle);
	stateSaveRequested = false;
	lastStateSaveMillis = millis();
}

void restorePosition(uint32_t positionSec) {
	xSemaphoreTake(audioMutex, portMAX_DELAY);
	xSemaphoreTake(sdMutex, portMAX_DELAY);
	if (f) {
		uint32_t fileSize = f.size();
		// BackgroundAudioMP3 has no time index; use a bitrate-based byte estimate.
		uint64_t approximateOffset = static_cast<uint64_t>(positionSec) * APPROXIMATE_MP3_BYTES_PER_SECOND;
		if (fileSize > 0 && approximateOffset >= fileSize) approximateOffset = fileSize - 1;
		if (f.seek(static_cast<uint32_t>(approximateOffset))) {
			mp3.flush();
		}
	}
	xSemaphoreGive(sdMutex);
	xSemaphoreGive(audioMutex);
	resetPlaybackPosition(positionSec);
}

bool openTrack(size_t idx) {
	if (tracks.empty()) return false;
	if (idx >= tracks.size()) idx = 0;

	xSemaphoreTake(audioMutex, portMAX_DELAY);
	xSemaphoreTake(sdMutex, portMAX_DELAY);
	if (f) f.close();
	Serial.print("Otwieram: ");
	Serial.println(tracks[idx]);
	f = SD.open(tracks[idx].c_str());
	bool opened = f;
	if (opened) mp3.flush();
	xSemaphoreGive(sdMutex);
	xSemaphoreGive(audioMutex);

	if (!opened) {
		Serial.println("Nie można otworzyć pliku");
		return false;
	}
	xSemaphoreTake(sdMutex, portMAX_DELAY);
	uint32_t fileSize = f.size();
	xSemaphoreGive(sdMutex);
	// BackgroundAudioMP3 exposes no MP3 bitrate/header metadata; this is a deliberate estimate.
	currentTrackDurationSec = max(1UL, fileSize / APPROXIMATE_MP3_BYTES_PER_SECOND);
	resetPlaybackPosition();
	return true;
}

bool loadPlaylistFromDirectory(const String &path, size_t selectedIndex) {
	if (!scanDirectory(path.c_str())) return false;

	std::vector<String> playlist;
	for (const DirectoryEntry &entry : directoryEntries) {
		if (!entry.isDirectory && !entry.isParent) {
			playlist.push_back(joinPath(path, entry.name));
		}
	}
	if (playlist.empty()) return false;
	if (selectedIndex >= playlist.size()) selectedIndex = 0;

	xSemaphoreTake(stateMutex, portMAX_DELAY);
	currentDirectory = path;
	tracks = playlist;
	currentTrack = selectedIndex;
	bool opened = openTrack(currentTrack);
	if (opened) {
		xSemaphoreTake(audioMutex, portMAX_DELAY);
		playbackStopped = false;
		playbackPaused = false;
		mp3.unpause();
		xSemaphoreGive(audioMutex);
	}
	xSemaphoreGive(stateMutex);
	return opened;
}

void advanceTrack() {
	xSemaphoreTake(stateMutex, portMAX_DELAY);
	currentTrack = (currentTrack + 1) % tracks.size();
	openTrack(currentTrack);
	xSemaphoreTake(audioMutex, portMAX_DELAY);
	playbackStopped = false;
	playbackPaused = false;
	mp3.unpause();
	xSemaphoreGive(audioMutex);
	xSemaphoreGive(stateMutex);
	requestStateSave();
}

void closeTrack() {
	xSemaphoreTake(audioMutex, portMAX_DELAY);
	xSemaphoreTake(sdMutex, portMAX_DELAY);
	if (f) f.close();
	xSemaphoreGive(sdMutex);
	xSemaphoreGive(audioMutex);
}

void retreatTrack() {
	xSemaphoreTake(stateMutex, portMAX_DELAY);
	currentTrack = currentTrack == 0 ? tracks.size() - 1 : currentTrack - 1;
	openTrack(currentTrack);
	xSemaphoreTake(audioMutex, portMAX_DELAY);
	playbackStopped = false;
	playbackPaused = false;
	mp3.unpause();
	xSemaphoreGive(audioMutex);
	xSemaphoreGive(stateMutex);
	requestStateSave();
}

void togglePlayPause() {
	if (playbackStopped) {
		if (!openTrack(currentTrack)) return;
		xSemaphoreTake(audioMutex, portMAX_DELAY);
		playbackStopped = false;
		playbackPaused = false;
		mp3.unpause();
		xSemaphoreGive(audioMutex);
		return;
	}

	updatePlaybackPosition();
	xSemaphoreTake(audioMutex, portMAX_DELAY);
	if (playbackPaused) {
		mp3.unpause();
		playbackPaused = false;
	} else {
		mp3.pause();
		playbackPaused = true;
	}
	xSemaphoreGive(audioMutex);
	requestStateSave();
}

void stopPlayback() {
	xSemaphoreTake(audioMutex, portMAX_DELAY);
	xSemaphoreTake(sdMutex, portMAX_DELAY);
	if (f) {
		f.close();
	}
	xSemaphoreGive(sdMutex);
	mp3.flush();
	mp3.pause();
	playbackStopped = true;
	playbackPaused = true;
	xSemaphoreGive(audioMutex);
	resetPlaybackPosition();
	requestStateSave();
}

void seekByBytes(int32_t delta) {
	xSemaphoreTake(audioMutex, portMAX_DELAY);
	xSemaphoreTake(sdMutex, portMAX_DELAY);
	if (f && !playbackStopped) {
		uint32_t position = f.position();
		uint32_t fileSize = f.size();
		int64_t target = static_cast<int64_t>(position) + delta;
		if (target < 0) target = 0;
		if (fileSize > 0 && target >= fileSize) target = fileSize - 1;
		if (f.seek(static_cast<uint32_t>(target))) {
			mp3.flush();
			uint32_t estimatedPosition = static_cast<uint32_t>(target / APPROXIMATE_MP3_BYTES_PER_SECOND);
			resetPlaybackPosition(estimatedPosition);
			Serial.println(delta > 0 ? "Fast forward 10 s" : "Rewind 10 s");
		}
	}
	xSemaphoreGive(sdMutex);
	xSemaphoreGive(audioMutex);
	requestStateSave();
}

void volumeUp() {
	xSemaphoreTake(audioMutex, portMAX_DELAY);
	currentGain = min(currentGain + GAIN_STEP, MAX_GAIN);
	if (!muted) mp3.setGain(currentGain);
	xSemaphoreGive(audioMutex);
	Serial.print("currentGain: ");
	Serial.println(currentGain, 3);
	requestStateSave();
	showVolumeOverlay();
}

void volumeDown() {
	xSemaphoreTake(audioMutex, portMAX_DELAY);
	currentGain = max(currentGain - GAIN_STEP, 0.0f);
	if (!muted) mp3.setGain(currentGain);
	xSemaphoreGive(audioMutex);
	Serial.print("currentGain: ");
	Serial.println(currentGain, 3);
	requestStateSave();
	showVolumeOverlay();
}

void toggleMute() {
	xSemaphoreTake(audioMutex, portMAX_DELAY);
	if (muted) {
		currentGain = gainBeforeMute;
		mp3.setGain(currentGain);
		muted = false;
	} else {
		gainBeforeMute = currentGain;
		mp3.setGain(0.0f);
		muted = true;
	}
	xSemaphoreGive(audioMutex);
	Serial.print("currentGain: ");
	Serial.println(currentGain, 3);
	requestStateSave();
	showVolumeOverlay();
}

bool isWifiClientConnected() {
	return false;
}

void showVolumeOverlay() {
	volumeOverlayUntil = millis() + UI_VOLUME_OVERLAY_MS;
	displayDirty = true;
}

void refreshBrowser() {
	if (!scanDirectory(currentDirectory.c_str())) {
		Serial.print("Nie można otworzyć katalogu: ");
		Serial.println(currentDirectory);
		currentDirectory = parentPath(currentDirectory);
		scanDirectory(currentDirectory.c_str());
	}
	if (directoryEntries.empty()) {
		browserSelection = 0;
		browserOffset = 0;
		return;
	}
	if (browserSelection >= directoryEntries.size()) {
		browserSelection = directoryEntries.size() - 1;
	}
	if (browserSelection < browserOffset) browserOffset = browserSelection;
	if (browserSelection >= browserOffset + 4) browserOffset = browserSelection - 3;
}

void enterBrowser() {
	setScreen(SCREEN_BROWSER);
	refreshBrowser();
	displayedTrack = static_cast<size_t>(-1);
	displayDirty = true;
}

void moveBrowserSelection(int direction) {
	if (directoryEntries.empty()) return;
	if (direction > 0) {
		browserSelection = (browserSelection + 1) % directoryEntries.size();
	} else {
		browserSelection = browserSelection == 0 ? directoryEntries.size() - 1 : browserSelection - 1;
	}
	if (browserSelection < browserOffset) browserOffset = browserSelection;
	if (browserSelection >= browserOffset + 4) browserOffset = browserSelection - 3;
	displayDirty = true;
}

void selectBrowserEntry() {
	if (directoryEntries.empty()) return;
	DirectoryEntry selected = directoryEntries[browserSelection];
	if (selected.isParent) {
		currentDirectory = parentPath(currentDirectory);
		browserSelection = 0;
		browserOffset = 0;
		refreshBrowser();
		displayDirty = true;
		return;
	}

	String selectedPath = joinPath(currentDirectory, selected.name);
	if (selected.isDirectory) {
		currentDirectory = selectedPath;
		browserSelection = 0;
		browserOffset = 0;
		refreshBrowser();
		displayDirty = true;
		return;
	}

	std::vector<String> playlist;
	size_t selectedTrack = 0;
	for (const DirectoryEntry &entry : directoryEntries) {
		if (!entry.isDirectory && !entry.isParent) {
			if (entry.name == selected.name) selectedTrack = playlist.size();
			playlist.push_back(joinPath(currentDirectory, entry.name));
		}
	}
	if (playlist.empty()) return;

	xSemaphoreTake(stateMutex, portMAX_DELAY);
	tracks = playlist;
	currentTrack = selectedTrack;
	if (openTrack(currentTrack)) {
		xSemaphoreTake(audioMutex, portMAX_DELAY);
		playbackStopped = false;
		playbackPaused = false;
		mp3.unpause();
		xSemaphoreGive(audioMutex);
		setScreen(SCREEN_NOWPLAYING);
		requestStateSave();
		displayDirty = true;
	}
	xSemaphoreGive(stateMutex);
}

void audioFeedTask(void *parameter) {
	(void)parameter;
	for (;;) {
		bool fedData = false;
		bool reachedEnd = false;
		updatePlaybackPosition();

		xSemaphoreTake(audioMutex, portMAX_DELAY);
		xSemaphoreTake(sdMutex, portMAX_DELAY);
		while (f && !playbackPaused && !playbackStopped && mp3.availableForWrite() > 512) {
			int len = f.read(filebuff, sizeof(filebuff));
			if (len > 0) {
				mp3.write(filebuff, len);
				fedData = true;
			}
			if (len != (int)sizeof(filebuff)) {
				f.close();
				reachedEnd = true;
				Serial.println("Koniec utworu: zamknięto plik SD");
				break;
			}
		}
		xSemaphoreGive(sdMutex);
		xSemaphoreGive(audioMutex);

		if (!fedData) vTaskDelay(pdMS_TO_TICKS(10));

		if (reachedEnd) {
			requestStateSave();
			advanceTrack();
		}
	}
}

void drawNowPlaying() {
	uint32_t now = millis();
	String title;
	size_t trackIndex;
	xSemaphoreTake(stateMutex, portMAX_DELAY);
	trackIndex = currentTrack;
	title = tracks.empty() ? String("Brak utworu") : tracks[currentTrack];
	xSemaphoreGive(stateMutex);
	xSemaphoreTake(audioMutex, portMAX_DELAY);
	bool paused = playbackPaused || playbackStopped;
	xSemaphoreGive(audioMutex);
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
	xSemaphoreTake(audioMutex, portMAX_DELAY);
	float displayGain = currentGain;
	xSemaphoreGive(audioMutex);
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

	u8g2.clearBuffer();
	u8g2.setFont(u8g2_font_6x10_tf);
	u8g2.drawStr(0, 10, "Now Playing");
	if (batteryVoltage < BATTERY_WARNING_VOLTAGE && ((now / 500) % 2 == 0)) {
		u8g2.drawFrame(112, 2, 13, 7);
		u8g2.drawBox(125, 4, 2, 3);
	} else {
		u8g2.drawFrame(112, 2, 13, 7);
		u8g2.drawBox(125, 4, 2, 3);
	}
	u8g2.drawHLine(0, 13, 128);
	if (titleWidth <= 128) {
		u8g2.drawUTF8(0, 23, title.c_str());
	} else {
		u8g2.drawUTF8(titleX, 23, title.c_str());
		if (marqueeOffset >= titleWidth) u8g2.drawUTF8(128, 23, title.c_str());
	}
	u8g2.drawFrame(0, 27, 128, 6);
	u8g2.drawBox(1, 28, max(1, static_cast<int>(126 * progress)), 4);
	char timeText[20];
	snprintf(timeText, sizeof(timeText), "%02lu:%02lu / %02lu:%02lu",
			displayPosition / 60, displayPosition % 60, durationSec / 60, durationSec % 60);
	u8g2.drawStr(0, 42, timeText);

	uint32_t iconElapsed = now - uiStateChangeMillis;
	uint8_t iconSize = UI_ANIMATIONS ? min(8UL, 4UL + iconElapsed / 60) : 8;
	if (paused) {
		u8g2.drawBox(7, 48, 3, iconSize);
		u8g2.drawBox(14, 48, 3, iconSize);
	} else {
		u8g2.drawTriangle(7, 48, 7, 48 + iconSize, 7 + iconSize, 48 + iconSize / 2);
	}

	if (!paused && UI_ANIMATIONS) {
		for (uint8_t bar = 0; bar < 5; ++bar) {
			uint8_t height = 4 + ((now / 140 + bar * 3) % 10);
			u8g2.drawBox(42 + bar * 6, 60 - height, 4, height);
		}
	}
	if (isWifiClientConnected()) {
		u8g2.drawCircle(118, 57, 2);
		u8g2.drawCircle(118, 57, 6);
		u8g2.drawCircle(118, 57, 9);
	}
	if (static_cast<int32_t>(volumeOverlayUntil - now) > 0) {
		char volumeText[16];
		snprintf(volumeText, sizeof(volumeText), "Vol: %d%%", static_cast<int>(displayGain * 100.0f / MAX_GAIN));
		u8g2.setDrawColor(0);
		u8g2.drawBox(52, 44, 76, 18);
		u8g2.setDrawColor(1);
		u8g2.drawFrame(52, 44, 76, 18);
		u8g2.drawStr(58, 56, volumeText);
	}
	u8g2.sendBuffer();
}

void drawBrowser() {
	uint32_t now = millis();
	static float highlightRow = 0.0f;
	float targetRow = static_cast<float>(browserSelection - browserOffset);
	if (!UI_ANIMATIONS) highlightRow = targetRow;
	else highlightRow += (targetRow - highlightRow) * min(1.0f, (now % 1000) / 120.0f);
	if (highlightRow < 0) highlightRow = 0;
	if (highlightRow > 3) highlightRow = 3;

	u8g2.clearBuffer();
	u8g2.setFont(u8g2_font_6x10_tf);
	String header = currentDirectory;
	if (header.length() > 20) header = header.substring(header.length() - 20);
	u8g2.drawStr(0, 9, header.c_str());
	u8g2.drawHLine(0, 11, 128);

	for (size_t row = 0; row < 4; ++row) {
		size_t index = browserOffset + row;
		if (index >= directoryEntries.size()) break;
		int baseline = 21 + static_cast<int>(row) * 11;
		if (static_cast<int>(row) == static_cast<int>(highlightRow + 0.5f)) {
			u8g2.setDrawColor(1);
			u8g2.drawBox(0, 21 + static_cast<int>(highlightRow) * 11 - 9, 128, 11);
			u8g2.setDrawColor(0);
		}

		DirectoryEntry entry = directoryEntries[index];
		String label = entry.isParent ? ".." : (entry.isDirectory ? "[D] " : "    ") + entry.name;
		if (label.length() > 20) label = label.substring(0, 20);
		u8g2.drawStr(0, baseline, label.c_str());
		u8g2.setDrawColor(1);
	}
	u8g2.sendBuffer();
}

void drawSplash(uint32_t now) {
	u8g2.clearBuffer();
	u8g2.setFont(u8g2_font_6x10_tf);
	u8g2.drawStr(29, 22, "vistream32");
	u8g2.drawFrame(14, 35, 100, 8);
	uint8_t width = static_cast<uint8_t>(min(96UL, static_cast<uint32_t>(splashProgress) * 96 / 100));
	if (UI_ANIMATIONS) width = max(width, static_cast<uint8_t>((now / 80) % 97));
	u8g2.drawBox(16, 37, width, 4);
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
		if (!uiReady) {
			drawSplash(now);
		} else if (currentScreen == SCREEN_BROWSER) {
			drawBrowser();
			drawScreenTransition(now);
		} else {
			drawNowPlaying();
			drawScreenTransition(now);
		}
		vTaskDelay(pdMS_TO_TICKS(UI_FRAME_INTERVAL_MS));
	}
}

void fail() {
	while (true) {
		delay(1000);
		ESP.restart();
	}
}

void setup() {
	Serial.begin(9600);
	Serial.println("MP3 player - BackgroundAudioMP3 example");
	preferences.begin("vistream", false);
	currentGain = constrain(preferences.getFloat("lastVolume", currentGain), 0.0f, MAX_GAIN);
	shuffleEnabled = preferences.getBool("shuffle", false);

	Wire.begin(I2C_SDA, I2C_SCL);
	u8g2.begin();
	splashProgress = 10;
	xTaskCreatePinnedToCore(
			uiTask,
			"oled-ui",
			4096,
			nullptr,
			1,
			&uiTaskHandle,
			0);

	if (!SD.begin(SD_CS)) {
		Serial.println("Błąd karty SD!");
		fail();
	}
	Serial.println("Karta SD OK");
	splashProgress = 40;

	audio.setPins(I2S_BCLK, I2S_LRC, I2S_DOUT);
	mp3.setGain(currentGain);

	if (!mp3.begin()) {
		Serial.println("Nie udało się uruchomić MP3 playera");
		fail();
	}
	splashProgress = 70;

	audioMutex = xSemaphoreCreateMutex();
	stateMutex = xSemaphoreCreateMutex();
	sdMutex = xSemaphoreCreateMutex();
	if (audioMutex == nullptr || stateMutex == nullptr || sdMutex == nullptr) fail();

	currentDirectory = "/";
	String savedFolder = preferences.getString("lastFolder", "");
	uint16_t savedTrack = preferences.getUShort("lastTrackIndex", 0);
	uint32_t savedPosition = preferences.getUInt("lastPositionSec", 0);
	bool restored = false;
	if (savedFolder.length() > 0) {
		restored = loadPlaylistFromDirectory(savedFolder, savedTrack);
		if (restored) restorePosition(savedPosition);
	}
	if (restored) {
		setScreen(SCREEN_NOWPLAYING);
		displayDirty = true;
	} else if (loadPlaylistFromDirectory("/muzyka", 0)) {
		setScreen(SCREEN_NOWPLAYING);
		displayDirty = true;
	} else {
		currentDirectory = "/";
		refreshBrowser();
		setScreen(SCREEN_BROWSER);
		displayDirty = true;
	}
	splashProgress = 100;
	uiReady = true;

	xTaskCreatePinnedToCore(
			audioFeedTask,
			"audio-feed",
			4096,
			nullptr,
			1,
			&audioTaskHandle,
			1);

	pinMode(NEXT_BUTTON_PIN, INPUT_PULLUP);
	pinMode(PREV_BUTTON_PIN, INPUT_PULLUP);
	pinMode(PLAY_BUTTON_PIN, INPUT_PULLUP);
	attachInterrupt(digitalPinToInterrupt(NEXT_BUTTON_PIN), handleNextISR, CHANGE);
	attachInterrupt(digitalPinToInterrupt(PREV_BUTTON_PIN), handlePrevISR, CHANGE);
	attachInterrupt(digitalPinToInterrupt(PLAY_BUTTON_PIN), handlePlayISR, CHANGE);
}

void loop() {
	unsigned long now = millis();
	bool nextHeld = digitalRead(NEXT_BUTTON_PIN) == LOW;
	bool prevHeld = digitalRead(PREV_BUTTON_PIN) == LOW;
	if (nextHeld && prevHeld) {
		if (muteComboStartedAt == 0) muteComboStartedAt = now;
		if (!muteComboTriggered && now - muteComboStartedAt >= 500) {
			Serial.println("Next+Prev: powrót do przeglądarki");
			enterBrowser();
			muteComboTriggered = true;
		}
	} else if (!nextHeld && !prevHeld) {
		muteComboStartedAt = 0;
		muteComboTriggered = false;
	}

	if (nextReleased && nextPressed) {
		nextReleased = false;
		nextPressed = false;
		if (!muteComboTriggered && now - lastButtonMillis > 300) {
			lastButtonMillis = now;
			if (playPressed && now - nextPressMillis < LONG_PRESS_MS) {
				volumeUp();
				playModifierUsed = true;
			} else if (currentScreen == SCREEN_BROWSER) {
				moveBrowserSelection(1);
			} else if (now - nextPressMillis >= LONG_PRESS_MS) {
				seekByBytes(SEEK_BYTES_10_SECONDS);
			} else {
				Serial.println("Przycisk NEXT: pomijanie utworu");
				closeTrack();
				advanceTrack();
			}
		}
	}

	if (prevReleased && prevPressed) {
		prevReleased = false;
		prevPressed = false;
		if (!muteComboTriggered && now - lastButtonMillis > 300) {
			lastButtonMillis = now;
			if (playPressed && now - prevPressMillis < LONG_PRESS_MS) {
				volumeDown();
				playModifierUsed = true;
			} else if (currentScreen == SCREEN_BROWSER) {
				moveBrowserSelection(-1);
			} else if (now - prevPressMillis >= LONG_PRESS_MS) {
				seekByBytes(-static_cast<int32_t>(SEEK_BYTES_10_SECONDS));
			} else {
				Serial.println("Przycisk PREV: poprzedni utwór");
				closeTrack();
				retreatTrack();
			}
		}
	}

	if (playReleased && playPressed) {
		playReleased = false;
		playPressed = false;
		if (playModifierUsed) {
			playModifierUsed = false;
		} else if (now - lastButtonMillis > 300) {
			lastButtonMillis = now;
			if (now - playPressMillis >= LONG_PRESS_MS) {
				Serial.println("Długie PLAY: stop");
				playClickPending = false;
				stopPlayback();
			} else if (playClickPending && now - lastPlayReleaseMillis <= PLAY_DOUBLE_CLICK_MS) {
				Serial.println("Podwójny PLAY: mute/unmute");
				playClickPending = false;
				toggleMute();
			} else {
				playClickPending = true;
				lastPlayReleaseMillis = now;
			}
		}
	}

	if (playClickPending && now - lastPlayReleaseMillis > PLAY_DOUBLE_CLICK_MS) {
		playClickPending = false;
		if (currentScreen == SCREEN_BROWSER) {
			selectBrowserEntry();
		} else {
			togglePlayPause();
		}
	}

	if (currentScreen == SCREEN_NOWPLAYING) {
		updatePlaybackPosition();
	}
	if (stateSaveRequested ||
		(currentScreen == SCREEN_NOWPLAYING && now - lastStateSaveMillis >= STATE_SAVE_INTERVAL_MS)) {
		saveStateToNvs();
	}

	delay(10);
}

void audio_info(const char *info)    { Serial.println(info); }
void audio_eof_mp3(const char *info) { Serial.println("Koniec pliku"); }
