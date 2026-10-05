#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
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
#define BATTERY_ADC_PIN 34
#define BATTERY_ADC_SAMPLES 12
#define VOLTAGE_DIVIDER_RATIO 2.0f
#define BATTERY_SAMPLE_INTERVAL_MS 3000UL
#define NEXT_BUTTON_PIN 4
#define PREV_BUTTON_PIN 33
#define PLAY_BUTTON_PIN 32
#define WIFI_SWITCH_PIN 16
#define BROWSER_BUTTON_PIN 17
#define LONG_PRESS_MS 800
#define PLAY_DOUBLE_CLICK_MS 400
#define WIFI_SWITCH_POLL_MS 250UL
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
#define WIFI_AP_SSID "vistream32"
#define WIFI_AP_PASSWORD "vistream1234"

// --- Ikony 8x8 (XBM) ---
static const unsigned char icon_folder[] PROGMEM = {
    0x0E, 0x7F, 0x81, 0x81, 0x81, 0x81, 0xFF, 0x00
};
static const unsigned char icon_note[] PROGMEM = {
    0x60, 0x60, 0x60, 0x60, 0x66, 0x6F, 0x0F, 0x00
};
static const unsigned char icon_back[] PROGMEM = {
    0x08, 0x0C, 0x7E, 0xFF, 0xFF, 0x7E, 0x0C, 0x08
};

// Kalibracja procentu baterii do wyświetlania (dopasuj pod realne napięcia)
constexpr float BATTERY_DISPLAY_FULL_V  = 4.2f;
constexpr float BATTERY_DISPLAY_EMPTY_V = 3.2f;


enum RepeatMode : uint8_t {
	REPEAT_OFF,
	REPEAT_ONE,
	REPEAT_ALL
};

enum Screen : uint8_t {
	SCREEN_BROWSER,
	SCREEN_NOWPLAYING
};

struct DirectoryEntry {
	String name;
	bool isDirectory;
	bool isParent;
	uint32_t size;
};

ESP32I2SAudio audio(I2S_BCLK, I2S_LRC, I2S_DOUT);
BackgroundAudioMP3 mp3(audio);
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

File f;
File uploadFile;
String uploadTargetPath;
bool uploadFailed = false;
uint8_t filebuff[512];
std::vector<String> tracks;
size_t currentTrack = 0;
std::vector<size_t> shuffleOrder;
size_t shuffleCursor = 0;
RepeatMode repeatMode = REPEAT_OFF;
std::vector<DirectoryEntry> directoryEntries;
String currentDirectory = "/";
size_t browserSelection = 0;
size_t browserOffset = 0;
Screen currentScreen = SCREEN_BROWSER;
volatile bool displayDirty = true;

volatile bool nextPressed = false;
volatile bool prevPressed = false;
volatile bool playPressed = false;
volatile bool browserPressed = false;
volatile bool nextReleased = false;
volatile bool prevReleased = false;
volatile bool playReleased = false;
volatile bool browserReleased = false;
volatile uint32_t nextPressMillis = 0;
volatile uint32_t prevPressMillis = 0;
volatile uint32_t playPressMillis = 0;
volatile uint32_t browserPressMillis = 0;
bool playModifierUsed = false;
bool playClickPending = false;
bool wifiApEnabled = false;
uint32_t lastPlayReleaseMillis = 0;
uint32_t lastWifiSwitchPollMillis = 0;
unsigned long lastButtonMillis = 0;
SemaphoreHandle_t audioMutex;
SemaphoreHandle_t stateMutex;
SemaphoreHandle_t sdMutex;
TaskHandle_t audioTaskHandle = nullptr;
TaskHandle_t uiTaskHandle = nullptr;
TaskHandle_t wifiTaskHandle = nullptr;
WebServer webServer(80);
size_t displayedTrack = static_cast<size_t>(-1);
bool displayedPaused = false;
volatile bool playbackPaused = false;
volatile bool playbackStopped = false;
float currentGain = DEFAULT_GAIN;
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
uint32_t lastBatterySampleMillis = 0;
String configuredSsid;
String configuredPassword;

bool isWifiClientConnected();
void showVolumeOverlay();

void updateBatteryVoltage() {
	uint32_t rawTotal = 0;
	for (uint8_t sample = 0; sample < BATTERY_ADC_SAMPLES; ++sample) {
		rawTotal += analogRead(BATTERY_ADC_PIN);
	}
	float rawAverage = static_cast<float>(rawTotal) / BATTERY_ADC_SAMPLES;
	batteryVoltage = (rawAverage / 4095.0f) * 3.3f * VOLTAGE_DIVIDER_RATIO;
	Serial.print("batteryVoltage: ");
	Serial.println(batteryVoltage, 3);
}

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

void IRAM_ATTR handleBrowserISR() {
	if (digitalRead(BROWSER_BUTTON_PIN) == LOW) {
		browserPressed = true;
		browserPressMillis = millis();
	} else {
		browserReleased = true;
	}
}

void applyWifiHardwareState(bool enabled) {
	if (enabled == wifiApEnabled) return;
	wifiApEnabled = enabled;
	if (wifiApEnabled) {
		WiFi.mode(WIFI_AP);
		WiFi.softAP(configuredSsid.c_str(), configuredPassword.c_str());
		Serial.print("Wi-Fi AP: ");
		Serial.println(configuredSsid);
		Serial.print("AP IP: ");
		Serial.println(WiFi.softAPIP());
	} else {
		WiFi.softAPdisconnect(true);
		WiFi.mode(WIFI_OFF);
		Serial.println("Wi-Fi AP OFF");
	}
	displayDirty = true;
}

void setScreen(Screen nextScreen) {
	if (currentScreen == nextScreen) return;
	previousScreen = currentScreen;
	currentScreen = nextScreen;
	screenTransitionStart = millis();
	uiStateChangeMillis = millis();
	displayDirty = true;
}

String decodeUrlComponent(const String &value) {
	String decoded;
	decoded.reserve(value.length());
	for (size_t index = 0; index < value.length(); ++index) {
		char character = value[index];
		if (character == '+') {
			decoded += ' ';
			continue;
		}
		if (character == '%' && index + 2 < value.length()) {
			char high = value[index + 1];
			char low = value[index + 2];
			bool valid = ((high >= '0' && high <= '9') || (high >= 'A' && high <= 'F') || (high >= 'a' && high <= 'f')) &&
				((low >= '0' && low <= '9') || (low >= 'A' && low <= 'F') || (low >= 'a' && low <= 'f'));
			if (valid) {
				char hex[3] = {high, low, '\0'};
				decoded += static_cast<char>(strtol(hex, nullptr, 16));
				index += 2;
				continue;
			}
		}
		decoded += character;
	}
	return decoded;
}

String getDecodedPathArg(const String &name) {
	String rawValue = webServer.hasArg(name) ? webServer.arg(name) : "";
	return decodeUrlComponent(rawValue);
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
		scannedEntries.push_back({"..", true, true, 0});
	}

	File entry = dir.openNextFile();
	while (entry) {
		String name = entry.name();
		int separator = name.lastIndexOf('/');
		if (separator >= 0) name = name.substring(separator + 1);
		if (entry.isDirectory() || name.endsWith(".mp3") || name.endsWith(".MP3")) {
			scannedEntries.push_back({name, entry.isDirectory(), false, entry.isDirectory() ? 0 : static_cast<uint32_t>(entry.size())});
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
	uint8_t savedRepeatMode;

	xSemaphoreTake(stateMutex, portMAX_DELAY);
	folder = currentDirectory;
	trackIndex = static_cast<uint16_t>(currentTrack);
	position = currentPositionSec;
	shuffle = shuffleEnabled;
	savedRepeatMode = static_cast<uint8_t>(repeatMode);
	xSemaphoreGive(stateMutex);
	xSemaphoreTake(audioMutex, portMAX_DELAY);
	gain = currentGain;
	xSemaphoreGive(audioMutex);

	preferences.putString("lastFolder", folder);
	preferences.putUShort("lastTrackIndex", trackIndex);
	preferences.putUInt("lastPositionSec", position);
	preferences.putFloat("lastVolume", gain);
	preferences.putBool("shuffle", shuffle);
	preferences.putUChar("repeatMode", savedRepeatMode);
	stateSaveRequested = false;
	lastStateSaveMillis = millis();
}

void rebuildShuffleOrder(size_t firstTrack) {
	shuffleOrder.clear();
	for (size_t index = 0; index < tracks.size(); ++index) shuffleOrder.push_back(index);
	for (size_t index = shuffleOrder.size(); index > 1; --index) {
		size_t other = random(index);
		size_t swapIndex = index - 1;
		size_t value = shuffleOrder[swapIndex];
		shuffleOrder[swapIndex] = shuffleOrder[other];
		shuffleOrder[other] = value;
	}
	if (!shuffleOrder.empty()) {
		for (size_t index = 0; index < shuffleOrder.size(); ++index) {
			if (shuffleOrder[index] == firstTrack) {
				size_t value = shuffleOrder[0];
				shuffleOrder[0] = shuffleOrder[index];
				shuffleOrder[index] = value;
				break;
			}
		}
		shuffleCursor = 0;
	}
}

void cyclePlaybackMode() {
	if (repeatMode == REPEAT_OFF && !shuffleEnabled) {
		shuffleEnabled = true;
		repeatMode = REPEAT_OFF;
		rebuildShuffleOrder(currentTrack);
		Serial.println("Tryb: SHUFFLE");
	} else if (shuffleEnabled) {
		shuffleEnabled = false;
		repeatMode = REPEAT_ONE;
		Serial.println("Tryb: REPEAT ONE");
	} else if (repeatMode == REPEAT_ONE) {
		repeatMode = REPEAT_ALL;
		Serial.println("Tryb: REPEAT ALL");
	} else {
		repeatMode = REPEAT_OFF;
		Serial.println("Tryb: NORMAL");
	}
	requestStateSave();
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
	if (shuffleEnabled) rebuildShuffleOrder(currentTrack);
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

bool playFileFromPath(const String &filePath) {
	if (!filePath.startsWith("/") || filePath.endsWith("/")) return false;
	String folder = parentPath(filePath);
	String fileName = filePath.substring(filePath.lastIndexOf('/') + 1);
	if (!scanDirectory(folder.c_str())) return false;

	size_t selectedIndex = 0;
	bool found = false;
	for (const DirectoryEntry &entry : directoryEntries) {
		if (!entry.isDirectory && !entry.isParent) {
			if (entry.name == fileName) found = true;
			if (!found) ++selectedIndex;
		}
	}
	if (!found) return false;
	bool opened = loadPlaylistFromDirectory(folder, selectedIndex);
	if (opened) {
		setScreen(SCREEN_NOWPLAYING);
		displayDirty = true;
		requestStateSave();
	}
	return opened;
}

void advanceTrack() {
	xSemaphoreTake(stateMutex, portMAX_DELAY);
	if (shuffleEnabled && !shuffleOrder.empty()) {
		shuffleCursor = (shuffleCursor + 1) % shuffleOrder.size();
		currentTrack = shuffleOrder[shuffleCursor];
	} else {
		currentTrack = (currentTrack + 1) % tracks.size();
	}
	openTrack(currentTrack);
	xSemaphoreTake(audioMutex, portMAX_DELAY);
	playbackStopped = false;
	playbackPaused = false;
	mp3.unpause();
	xSemaphoreGive(audioMutex);
	xSemaphoreGive(stateMutex);
	requestStateSave();
}

void stopAtEnd() {
	xSemaphoreTake(audioMutex, portMAX_DELAY);
	mp3.flush();
	mp3.pause();
	playbackStopped = true;
	playbackPaused = true;
	xSemaphoreGive(audioMutex);
	requestStateSave();
}

void handleTrackEnd() {
	if (repeatMode == REPEAT_ONE) {
		openTrack(currentTrack);
		return;
	}

	if (shuffleEnabled && !shuffleOrder.empty()) {
		if (shuffleCursor + 1 < shuffleOrder.size()) {
			advanceTrack();
		} else if (repeatMode == REPEAT_ALL) {
			rebuildShuffleOrder(currentTrack);
			advanceTrack();
		} else {
			stopAtEnd();
		}
		return;
	}

	if (currentTrack + 1 < tracks.size()) {
		advanceTrack();
	} else if (repeatMode == REPEAT_ALL) {
		currentTrack = static_cast<size_t>(-1);
		advanceTrack();
	} else {
		stopAtEnd();
	}
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
	if (shuffleEnabled && !shuffleOrder.empty()) {
		shuffleCursor = shuffleCursor == 0 ? shuffleOrder.size() - 1 : shuffleCursor - 1;
		currentTrack = shuffleOrder[shuffleCursor];
	} else {
		currentTrack = currentTrack == 0 ? tracks.size() - 1 : currentTrack - 1;
	}
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
	mp3.setGain(currentGain);
	xSemaphoreGive(audioMutex);
	Serial.print("currentGain: ");
	Serial.println(currentGain, 3);
	requestStateSave();
	showVolumeOverlay();
}

void volumeDown() {
	xSemaphoreTake(audioMutex, portMAX_DELAY);
	currentGain = max(currentGain - GAIN_STEP, 0.0f);
	mp3.setGain(currentGain);
	xSemaphoreGive(audioMutex);
	Serial.print("currentGain: ");
	Serial.println(currentGain, 3);
	requestStateSave();
	showVolumeOverlay();
}

bool isWifiClientConnected() {
	return WiFi.softAPgetStationNum() > 0;
}

void showVolumeOverlay() {
	volumeOverlayUntil = millis() + UI_VOLUME_OVERLAY_MS;
	displayDirty = true;
}

const char remotePage[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>vistream32</title>
<style>
  :root{
    --bg:#17140f;
    --panel:#1d1911;
    --line:#3a3327;
    --text:#efe7d8;
    --text-dim:#9c927e;
    --accent:#e2a33b;
    --accent-dim:#4a3d21;
    --danger:#b85c4b;
    --mono:ui-monospace,SFMono-Regular,Consolas,"Liberation Mono",Menlo,monospace;
    --sans:system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;
  }
  *{box-sizing:border-box}
  body{
    margin:0;
    background:var(--bg);
    color:var(--text);
    font-family:var(--sans);
    -webkit-tap-highlight-color:transparent;
  }
  .frame{max-width:480px;margin:0 auto;padding:0 0 40px}

  header{
    padding:20px 20px 16px;
    display:flex;
    align-items:baseline;
    justify-content:space-between;
    border-bottom:1px solid var(--line);
  }
  header h1{
    font-size:16px;
    font-weight:600;
    letter-spacing:.02em;
    margin:0;
    color:var(--text);
  }
  header span{
    font-family:var(--mono);
    font-size:11px;
    color:var(--accent);
  }

  section{padding:18px 20px;border-bottom:1px solid var(--line)}
  section h2{
    font-size:12px;
    font-weight:600;
    color:var(--text-dim);
    margin:0 0 14px;
  }

  /* now playing display */
  .display{
    background:var(--panel);
    border:1px solid var(--line);
    border-radius:2px;
    padding:14px 16px;
    margin-bottom:16px;
  }
  #title{
    font-family:var(--mono);
    font-size:15px;
    color:var(--accent);
    white-space:nowrap;
    overflow:hidden;
    text-overflow:ellipsis;
    margin-bottom:10px;
  }
  .seekrow{display:flex;align-items:center;gap:10px}
  #time{font-family:var(--mono);font-size:11px;color:var(--text-dim);white-space:nowrap}

  input[type=range]{
    -webkit-appearance:none;
    appearance:none;
    width:100%;
    height:3px;
    background:var(--line);
    border-radius:2px;
    outline:none;
  }
  input[type=range]::-webkit-slider-thumb{
    -webkit-appearance:none;
    width:14px;height:14px;
    border-radius:50%;
    background:var(--accent);
    cursor:pointer;
    border:2px solid var(--bg);
  }
  input[type=range]::-moz-range-thumb{
    width:14px;height:14px;border-radius:50%;
    background:var(--accent);border:2px solid var(--bg);cursor:pointer;
  }
  input[type=range]::-webkit-slider-runnable-track{
    height:3px;border-radius:2px;background:var(--line);
  }

  /* transport */
  .transport{display:flex;gap:10px;margin-bottom:18px}
  .transport button{flex:1}
  button{
    font-family:var(--sans);
    font-size:14px;
    font-weight:600;
    color:var(--text);
    background:var(--panel);
    border:1px solid var(--line);
    border-radius:3px;
    padding:12px 10px;
    cursor:pointer;
  }
  button:active{background:var(--accent-dim);border-color:var(--accent)}
  #playPauseButton{
    background:var(--accent-dim);
    border-color:var(--accent);
    color:var(--accent);
  }
	.modes{display:grid;grid-template-columns:repeat(4,1fr);gap:6px;margin-top:14px}
	.modes button{padding:8px 4px;font-size:11px;font-family:var(--mono);color:var(--text-dim)}
	.modes button.active{background:var(--accent-dim);border-color:var(--accent);color:var(--accent)}

  .volrow{display:flex;align-items:center;gap:12px}
  .volrow label{font-size:11px;color:var(--text-dim);width:44px;flex-shrink:0}
  .volrow span{font-family:var(--mono);font-size:12px;color:var(--text-dim);width:32px;text-align:right;flex-shrink:0}

  /* files */
  .pathbar{
    font-family:var(--mono);
    font-size:12px;
    color:var(--text-dim);
    margin-bottom:10px;
  }
  #filesPath{color:var(--accent)}

  #files{border-top:1px solid var(--line)}
  .file{
    padding:10px 0;
    border-bottom:1px solid var(--line);
    display:flex;
    align-items:center;
    justify-content:space-between;
    gap:10px;
  }
  .file-name{
    font-size:13px;
    overflow:hidden;
    text-overflow:ellipsis;
    white-space:nowrap;
    cursor:pointer;
  }
  .file[data-is-dir="1"] .file-name{color:var(--text)}
  .file[data-is-dir="1"]{cursor:pointer}

  .file-actions{display:flex;gap:6px;flex-shrink:0}
  .file-actions button{
    padding:6px 9px;
    font-size:12px;
    font-family:var(--mono);
    min-width:0;
  }
  button[data-action="play"]{color:var(--accent);border-color:var(--accent-dim)}
  button[data-action="delete"]{color:var(--danger);border-color:var(--danger)}

  .uploadrow{display:flex;gap:8px;margin-top:14px}
  .uploadrow input[type=file]{
    flex:1;
    font-size:12px;
    color:var(--text-dim);
    background:var(--panel);
    border:1px solid var(--line);
    border-radius:3px;
    padding:8px;
  }
  .uploadrow button{flex-shrink:0}

  .folderrow{display:flex;gap:8px;margin-top:10px}
  .folderrow input{
    flex:1;
    background:var(--panel);
    border:1px solid var(--line);
    border-radius:3px;
    color:var(--text);
    font-size:13px;
    padding:10px;
  }

  form.config{display:flex;flex-direction:column;gap:8px}
  form.config input{
    background:var(--panel);
    border:1px solid var(--line);
    border-radius:3px;
    color:var(--text);
    font-size:13px;
    padding:10px;
  }
  form.config button{margin-top:2px}

  ::placeholder{color:var(--text-dim);opacity:.7}
</style>
</head>
<body>
<div class="frame">

  <header>
    <h1>vistream32</h1>
    <span>local player</span>
  </header>

  <section>
    <div class="display">
      <div id="title">Loading&hellip;</div>
      <div class="seekrow">
        <input id="seek" type="range" min="0" max="1" value="0" oninput="setSeek(this.value)">
      </div>
      <div id="time" style="margin-top:8px">00:00 / 00:00</div>
    </div>

    <div class="transport">
      <button onclick="post('/prev')">Prev</button>
      <button id="playPauseButton" onclick="togglePlayPause()">Play</button>
      <button onclick="post('/next')">Next</button>
    </div>

    <div class="volrow">
      <label for="volume">Volume</label>
      <input id="volume" type="range" min="0" max="100" value="0" oninput="setVolume(this.value)">
      <span id="vol">0</span>
    </div>
		<div class="modes" aria-label="Playback mode">
			<button data-mode="single" onclick="setMode('single')">Single</button>
			<button data-mode="repeat-one" onclick="setMode('repeat-one')">Repeat 1</button>
			<button data-mode="shuffle" onclick="setMode('shuffle')">Shuffle</button>
			<button data-mode="repeat-all" onclick="setMode('repeat-all')">Repeat All</button>
		</div>
  </section>

  <section>
    <h2>Files</h2>
    <div class="pathbar">path: <span id="filesPath">/</span></div>
    <div id="files">Loading&hellip;</div>

    <div class="uploadrow">
      <form id="uploadForm" style="display:flex;flex:1;gap:8px;align-items:center;flex-wrap:wrap">
        <input id="uploadFile" type="file" accept=".mp3,audio/mpeg">
        <button type="submit">Upload</button>
        <span id="uploadStatus" style="font-size:12px;opacity:0.8"></span>
      </form>
    </div>

    <div class="folderrow">
      <input id="newFolder" placeholder="New folder name">
      <button onclick="makeFolder()">Create</button>
    </div>
  </section>

  <section style="border-bottom:none">
    <h2>Network</h2>
    <form id="configForm" class="config">
      <input id="ssid" placeholder="SSID">
      <input id="password" type="password" placeholder="Password">
      <button type="submit">Save &amp; restart</button>
    </form>
  </section>

</div>

<script>
let seekTimer;let isPaused=true;let filesPath='/';async function post(path){await fetch(path,{method:'POST'});refresh()}
function togglePlayPause(){post(isPaused?'/play':'/pause')}
function setVolume(value){document.querySelector('#vol').textContent=value;clearTimeout(window.volumeTimer);window.volumeTimer=setTimeout(()=>fetch('/volume?value='+value,{method:'POST'}),120)}
function setSeek(value){clearTimeout(seekTimer);seekTimer=setTimeout(()=>fetch('/seek?position='+value,{method:'POST'}),200)}
async function setMode(mode){await fetch('/mode?value='+encodeURIComponent(mode),{method:'POST'});refresh()}
async function refresh(){let s=await (await fetch('/status')).json();isPaused=s.paused;document.querySelector('#playPauseButton').textContent=isPaused?'Play':'Pause';document.querySelector('#title').textContent=s.track;document.querySelector('#vol').textContent=s.volume;document.querySelector('#volume').value=s.volume;document.querySelector('#seek').max=s.duration;document.querySelector('#seek').value=s.position;document.querySelector('#time').textContent=s.positionText+' / '+s.durationText;document.querySelectorAll('[data-mode]').forEach(button=>button.classList.toggle('active',button.dataset.mode===s.mode))}
function parentPath(path){if(path==='/')return '/';let i=path.lastIndexOf('/');return i<=0?'/':path.slice(0,i)}
const listContainer=document.querySelector('#files');
async function loadFiles(path){filesPath=path||'/';let entries=await (await fetch('/files?path='+encodeURIComponent(filesPath))).json();document.querySelector('#filesPath').textContent=filesPath;listContainer.replaceChildren();entries.forEach(e=>{const full=e.name==='..'?parentPath(filesPath):(filesPath==='/'?'/'+e.name:filesPath+'/'+e.name);const el=document.createElement('div');el.className='file';el.dataset.path=full;el.dataset.isDir=e.isDirectory?'1':'0';const name=document.createElement('span');name.className='file-name';name.textContent=(e.isDirectory?'[D] ':'')+e.name;el.appendChild(name);if(!e.isDirectory){const actions=document.createElement('div');actions.className='file-actions';for(const action of ['play','download','delete']){const button=document.createElement('button');button.dataset.action=action;button.dataset.path=full;button.dataset.name=e.name;button.textContent=action[0].toUpperCase()+action.slice(1);actions.appendChild(button)}el.appendChild(actions)}listContainer.appendChild(el)});if(!entries.length)listContainer.textContent='Empty'}
listContainer.addEventListener('click',async(event)=>{const action=event.target.dataset.action;const path=event.target.dataset.path;if(action){if(action==='play')await post('/playFile?path='+encodeURIComponent(path));if(action==='download')window.location.href='/download?path='+encodeURIComponent(path);if(action==='delete'&&confirm('Na pewno usunąć '+event.target.dataset.name+'?')){await post('/delete?path='+encodeURIComponent(path));loadFiles(filesPath)}return}const el=event.target.closest('[data-path]');if(el&&el.dataset.isDir==='1'){loadFiles(el.dataset.path)}});
document.querySelector('#uploadForm').addEventListener('submit',async(event)=>{event.preventDefault();const file=document.querySelector('#uploadFile').files[0];if(!file)return;const status=document.querySelector('#uploadStatus');const form=new FormData();form.append('path',filesPath);form.append('file',file);const xhr=new XMLHttpRequest();xhr.open('POST','/upload?path='+encodeURIComponent(filesPath),true);xhr.upload.onprogress=(progressEvent)=>{const pct=progressEvent.lengthComputable?Math.round((progressEvent.loaded/progressEvent.total)*100):0;status.textContent='Uploading: '+pct+'%';};xhr.onload=()=>{if(xhr.status>=200&&xhr.status<300){status.textContent='Upload complete';loadFiles(filesPath);}else{const msg=(xhr.responseText||xhr.statusText||'Upload failed');status.textContent='Upload failed';alert('Upload failed: '+msg);}};xhr.onerror=()=>{status.textContent='Upload failed';alert('Upload failed');};xhr.send(form);});
async function makeFolder(){const name=document.querySelector('#newFolder').value.trim();if(name)await post('/mkdir?path='+encodeURIComponent((filesPath==='/'?'':filesPath)+'/'+name));loadFiles(filesPath)}
async function loadConfig(){const c=await (await fetch('/config')).json();document.querySelector('#ssid').value=c.ssid;}
document.querySelector('#configForm').addEventListener('submit',async(event)=>{event.preventDefault();await fetch('/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'ssid='+encodeURIComponent(document.querySelector('#ssid').value)+'&password='+encodeURIComponent(document.querySelector('#password').value)});alert('Restarting AP');});
refresh();loadFiles('/');setInterval(refresh,1500);
loadConfig();
</script>
</body>
</html>
)HTML";

String jsonEscape(const String &value) {
	String escaped;
	for (size_t index = 0; index < value.length(); ++index) {
		char character = value[index];
		if (character == '"' || character == '\\') escaped += '\\';
		if (character == '\n') escaped += "\\n";
		else if (character != '\r') escaped += character;
	}
	return escaped;
}

String formatTime(uint32_t seconds) {
	char buffer[8];
	snprintf(buffer, sizeof(buffer), "%02lu:%02lu", seconds / 60, seconds % 60);
	return String(buffer);
}

void sendPlayerStatus() {
	updatePlaybackPosition();
	String track;
	uint32_t position;
	uint32_t duration;
	bool paused;
	bool shuffle;
	RepeatMode repeat;
	xSemaphoreTake(stateMutex, portMAX_DELAY);
	track = tracks.empty() ? String("") : tracks[currentTrack];
	position = currentPositionSec;
	shuffle = shuffleEnabled;
	repeat = repeatMode;
	xSemaphoreGive(stateMutex);
	xSemaphoreTake(audioMutex, portMAX_DELAY);
	paused = playbackPaused || playbackStopped;
	float gain = currentGain;
	xSemaphoreGive(audioMutex);
	duration = currentTrackDurationSec;
	int separator = track.lastIndexOf('/');
	if (separator >= 0) track = track.substring(separator + 1);
	uint32_t displayPosition = min(position, duration);
	int volume = static_cast<int>(gain * 100.0f / MAX_GAIN);
	String mode = shuffle ? "shuffle" : (repeat == REPEAT_ONE ? "repeat-one" : (repeat == REPEAT_ALL ? "repeat-all" : "normal"));
	String json = "{\"track\":\"" + jsonEscape(track) + "\",\"position\":" + String(displayPosition) +
		",\"duration\":" + String(duration) + ",\"volume\":" + String(volume) +
		",\"paused\":" + String(paused ? "true" : "false") + ",\"mode\":\"" + mode +
		"\",\"wifiEnabled\":" + String(wifiApEnabled ? "true" : "false") +
		",\"positionText\":\"" + formatTime(displayPosition) + "\",\"durationText\":\"" + formatTime(duration) + "\"}";
	webServer.send(200, "application/json", json);
}

void webPlay() {
	Serial.println("Otrzymano /play");
	if (playbackPaused || playbackStopped) togglePlayPause();
	webServer.send(200, "text/plain", "OK");
}

void webPause() {
	Serial.println("Otrzymano /pause");
	if (!playbackPaused && !playbackStopped) togglePlayPause();
	webServer.send(200, "text/plain", "OK");
}

void webNext() {
	closeTrack();
	advanceTrack();
	setScreen(SCREEN_NOWPLAYING);
	webServer.send(200, "text/plain", "OK");
}

void webPrev() {
	closeTrack();
	retreatTrack();
	setScreen(SCREEN_NOWPLAYING);
	webServer.send(200, "text/plain", "OK");
}

void webMode() {
	String mode = webServer.hasArg("value") ? webServer.arg("value") : "single";
	if (mode == "shuffle") {
		shuffleEnabled = true;
		repeatMode = REPEAT_OFF;
		rebuildShuffleOrder(currentTrack);
	} else if (mode == "repeat-one") {
		shuffleEnabled = false;
		repeatMode = REPEAT_ONE;
		shuffleOrder.clear();
	} else if (mode == "repeat-all") {
		shuffleEnabled = false;
		repeatMode = REPEAT_ALL;
		shuffleOrder.clear();
	} else if (mode == "single") {
		shuffleEnabled = false;
		repeatMode = REPEAT_OFF;
		shuffleOrder.clear();
	} else {
		webServer.send(400, "text/plain", "Invalid mode");
		return;
	}
	requestStateSave();
	Serial.print("Tryb WWW: ");
	Serial.println(mode);
	webServer.send(200, "text/plain", "OK");
}

void webPlayFile() {
	String path = getDecodedPathArg("path");
	bool played = path.startsWith("/") && path.indexOf("..") < 0 && playFileFromPath(path);
	webServer.send(played ? 200 : 404, "text/plain", played ? "OK" : "File not found");
}

void webDelete() {
	String path = getDecodedPathArg("path");
	if (!path.startsWith("/") || path.indexOf("..") >= 0) {
		webServer.send(400, "text/plain", "Invalid path");
		return;
	}

	String activePath;
	xSemaphoreTake(stateMutex, portMAX_DELAY);
	if (!tracks.empty() && currentTrack < tracks.size()) activePath = tracks[currentTrack];
	xSemaphoreGive(stateMutex);
	if (path == activePath) stopPlayback();

	xSemaphoreTake(sdMutex, portMAX_DELAY);
	bool removed = SD.remove(path.c_str());
	xSemaphoreGive(sdMutex);
	webServer.send(removed ? 200 : 404, "text/plain", removed ? "OK" : "File not found");
}

void webMkdir() {
	String path = getDecodedPathArg("path");
	if (!path.startsWith("/") || path.indexOf("..") >= 0 || path == "/") {
		webServer.send(400, "text/plain", "Invalid path");
		return;
	}
	if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
		bool created = SD.mkdir(path.c_str());
		xSemaphoreGive(sdMutex);
		webServer.send(created ? 200 : 409, "text/plain", created ? "OK" : "Unable to create folder");
		return;
	}
	webServer.send(503, "text/plain", "Busy");
}

void handleUpload(HTTPUpload &upload) {
	static uint32_t uploadBytesTotal = 0;
	static uint32_t lastHeapLogMillis = 0;
	static uint32_t lastChunkLogBytes = 0;
	static uint32_t chunkCounter = 0;

	if (upload.status == UPLOAD_FILE_START) {
		uploadBytesTotal = 0;
		lastHeapLogMillis = millis();
		lastChunkLogBytes = 0;
		chunkCounter = 0;
		uploadFailed = false;
		uploadTargetPath = "";
		Serial.printf("[upload] start path=%s freeHeap=%u\n", getDecodedPathArg("path").c_str(), ESP.getFreeHeap());
		String folder = getDecodedPathArg("path");
		if (folder.length() == 0) folder = "/";
		String name = upload.filename;
		int separator = name.lastIndexOf('/');
		if (separator >= 0) name = name.substring(separator + 1);
		if (!folder.startsWith("/") || folder.indexOf("..") >= 0 || name.length() == 0 || name.indexOf("..") >= 0) {
			uploadFailed = true;
			Serial.println("[upload] invalid upload path");
			return;
		}
		uploadTargetPath = joinPath(folder, name);
		if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
			SD.remove(uploadTargetPath.c_str());
			uploadFile = SD.open(uploadTargetPath.c_str(), FILE_WRITE);
			xSemaphoreGive(sdMutex);
			if (!uploadFile) {
				uploadFailed = true;
				Serial.printf("[upload] failed to open %s for write\n", uploadTargetPath.c_str());
				return;
			}
		} else {
			uploadFile = File();
			uploadFailed = true;
			Serial.println("[upload] ERROR: could not acquire sdMutex on start within 200ms");
			if (!uploadTargetPath.isEmpty()) {
				SD.remove(uploadTargetPath.c_str());
			}
			return;
		}
	} else if (upload.status == UPLOAD_FILE_WRITE && uploadFile && !uploadFailed) {
		if (upload.currentSize > 0) {
			if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
				Serial.println("[upload] ERROR: could not acquire sdMutex in 200ms during write - aborting");
				uploadFailed = true;
				if (uploadFile) {
					uploadFile.close();
				}
				if (!uploadTargetPath.isEmpty()) {
					SD.remove(uploadTargetPath.c_str());
				}
				return;
			}
			size_t written = uploadFile.write(upload.buf, upload.currentSize);
			xSemaphoreGive(sdMutex);
			if (written != upload.currentSize) {
				Serial.printf("[upload] BŁĄD ZAPISU: żądano=%u zapisano=%u — przerywam upload\n", upload.currentSize, written);
				uploadFailed = true;
				if (uploadFile) {
					uploadFile.close();
				}
				if (!uploadTargetPath.isEmpty()) {
					SD.remove(uploadTargetPath.c_str());
				}
				return;
			}
			uploadBytesTotal += written;
			chunkCounter++;
			lastChunkLogBytes += written;
			if (millis() - lastHeapLogMillis >= 2000) {
				lastHeapLogMillis = millis();
				Serial.printf("[upload] heap total=%u freeHeap=%u\n", uploadBytesTotal, ESP.getFreeHeap());
			}
			if ((chunkCounter % 50 == 0) || (lastChunkLogBytes >= 500 * 1024)) {
				Serial.printf("[upload] chunkLog chunks=%u bytes=%u total=%u freeHeap=%u\n", chunkCounter, lastChunkLogBytes, uploadBytesTotal, ESP.getFreeHeap());
				lastChunkLogBytes = 0;
			}
		}
	} else if (upload.status == UPLOAD_FILE_END) {
		Serial.printf("[upload] end total=%u expected=%u freeHeap=%u failed=%d\n", uploadBytesTotal, upload.totalSize, ESP.getFreeHeap(), uploadFailed ? 1 : 0);
		if (uploadFile) {
			if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
				uploadFile.close();
				xSemaphoreGive(sdMutex);
			}
		}
		if (uploadFailed || uploadBytesTotal != upload.totalSize || uploadBytesTotal == 0) {
			uploadFailed = true;
			if (!uploadTargetPath.isEmpty()) {
				if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
					SD.remove(uploadTargetPath.c_str());
					xSemaphoreGive(sdMutex);
				}
			}
			Serial.println("[upload] upload failed and temporary file was removed");
		}
		uploadFile = File();
		uploadTargetPath = "";
	}
	yield();
}

void handleUploadRequest() {
	handleUpload(webServer.upload());
}

void uploadComplete() {
	if (uploadFailed) {
		webServer.send(500, "text/plain", "Upload failed - incomplete write");
		uploadFailed = false;
		return;
	}
	webServer.send(200, "text/plain", "OK");
	uploadFailed = false;
}

void webConfigGet() {
	webServer.send(200, "application/json", "{\"ssid\":\"" + jsonEscape(configuredSsid) + "\"}");
}

void webConfigPost() {
	String ssid = webServer.arg("ssid");
	String password = webServer.arg("password");
	if (ssid.length() == 0 || password.length() < 8) {
		webServer.send(400, "text/plain", "SSID and password are invalid");
		return;
	}
	preferences.putString("wifiSsid", ssid);
	preferences.putString("wifiPassword", password);
	webServer.send(200, "text/plain", "Restarting");
	delay(100);
	ESP.restart();
}

void webVolume() {
	int value = constrain(webServer.arg("value").toInt(), 0, 100);
	xSemaphoreTake(audioMutex, portMAX_DELAY);
	currentGain = MAX_GAIN * value / 100.0f;
	mp3.setGain(currentGain);
	xSemaphoreGive(audioMutex);
	Serial.print("currentGain: ");
	Serial.println(currentGain, 3);
	requestStateSave();
	showVolumeOverlay();
	webServer.send(200, "text/plain", "OK");
}

void webSeek() {
	int32_t requestedPosition = static_cast<int32_t>(webServer.arg("position").toInt());
	uint32_t target = requestedPosition < 0 ? 0 : static_cast<uint32_t>(requestedPosition);
	updatePlaybackPosition();
	int32_t delta = static_cast<int32_t>(target) - static_cast<int32_t>(currentPositionSec);
	seekByBytes(delta * static_cast<int32_t>(APPROXIMATE_MP3_BYTES_PER_SECOND));
	webServer.send(200, "text/plain", "OK");
}

void webFiles() {
	String path = getDecodedPathArg("path");
	if (path.length() == 0) path = "/";
	if (!path.startsWith("/")) path = "/" + path;
	if (!scanDirectory(path.c_str())) {
		webServer.send(404, "application/json", "{\"error\":\"directory not found\"}");
		return;
	}

	String json = "[";
	for (size_t index = 0; index < directoryEntries.size(); ++index) {
		const DirectoryEntry &entry = directoryEntries[index];
		if (index > 0) json += ",";
		json += "{\"name\":\"" + jsonEscape(entry.name) + "\",\"isDirectory\":";
		json += entry.isDirectory ? "true" : "false";
		json += ",\"size\":" + String(entry.size) + "}";
	}
	json += "]";
	webServer.send(200, "application/json", json);
}

void webDownload() {
	String path = getDecodedPathArg("path");
	if (!path.startsWith("/") || path.indexOf("..") >= 0) {
		webServer.send(400, "text/plain", "Invalid path");
		return;
	}

	File download;
	bool validFile = false;
	uint32_t fileSize = 0;
	if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
		download = SD.open(path.c_str(), FILE_READ);
		validFile = download && !download.isDirectory();
		fileSize = validFile ? static_cast<uint32_t>(download.size()) : 0;
		xSemaphoreGive(sdMutex);
	}
	if (!validFile) {
		webServer.send(404, "text/plain", "File not found");
		return;
	}

	String fileName = path.substring(path.lastIndexOf('/') + 1);
	String contentType = (fileName.endsWith(".mp3") || fileName.endsWith(".MP3")) ? "audio/mpeg" : "application/octet-stream";
	webServer.sendHeader("Content-Disposition", "attachment; filename=\"" + fileName + "\"");
	webServer.setContentLength(fileSize);
	webServer.send(200, contentType, "");
	WiFiClient client = webServer.client();
	uint8_t buffer[1024];
	while (download) {
		xSemaphoreTake(sdMutex, portMAX_DELAY);
		bool hasData = download.available();
		size_t count = hasData ? download.read(buffer, sizeof(buffer)) : 0;
		xSemaphoreGive(sdMutex);
		if (!hasData || count == 0 || client.write(buffer, count) != count) break;
		taskYIELD();
	}
	xSemaphoreTake(sdMutex, portMAX_DELAY);
	download.close();
	xSemaphoreGive(sdMutex);
}

void setupWebServer() {
	webServer.on("/", HTTP_GET, []() { webServer.send_P(200, "text/html", remotePage); });
	webServer.on("/status", HTTP_GET, sendPlayerStatus);
	webServer.on("/play", HTTP_POST, webPlay);
	webServer.on("/pause", HTTP_POST, webPause);
	webServer.on("/next", HTTP_POST, webNext);
	webServer.on("/prev", HTTP_POST, webPrev);
	webServer.on("/mode", HTTP_POST, webMode);
	webServer.on("/playFile", HTTP_POST, webPlayFile);
	webServer.on("/delete", HTTP_POST, webDelete);
	webServer.on("/mkdir", HTTP_POST, webMkdir);
	webServer.on("/volume", HTTP_POST, webVolume);
	webServer.on("/seek", HTTP_POST, webSeek);
	webServer.on("/files", HTTP_GET, webFiles);
	webServer.on("/download", HTTP_GET, webDownload);
	webServer.on("/upload", HTTP_POST, uploadComplete, handleUploadRequest);
	webServer.on("/config", HTTP_GET, webConfigGet);
	webServer.on("/config", HTTP_POST, webConfigPost);
	webServer.begin();
}

void wifiTask(void *parameter) {
	(void)parameter;
	bool wifiSwitchState = digitalRead(WIFI_SWITCH_PIN) == LOW;
	wifiApEnabled = wifiSwitchState;
	if (wifiApEnabled) {
		WiFi.mode(WIFI_AP);
		WiFi.softAP(configuredSsid.c_str(), configuredPassword.c_str());
		Serial.print("Wi-Fi AP: ");
		Serial.println(configuredSsid);
		Serial.print("AP IP: ");
		Serial.println(WiFi.softAPIP());
	}
	setupWebServer();
	for (;;) {
		bool switchState = digitalRead(WIFI_SWITCH_PIN) == LOW;
		if (switchState != wifiApEnabled) {
			applyWifiHardwareState(switchState);
		}
		webServer.handleClient();
		yield();
		vTaskDelay(pdMS_TO_TICKS(2));
	}
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

	playFileFromPath(joinPath(currentDirectory, selected.name));
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
				handleTrackEnd();
		}
	}
}

void drawNowPlaying() {
	uint32_t now = millis();
	String title;
	size_t trackIndex = 0;
	size_t trackCount = 0;
	bool paused = false;
	float displayGain = currentGain;
	if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
		trackIndex = currentTrack;
		trackCount = tracks.size();
		title = tracks.empty() ? String("Brak utworu") : tracks[currentTrack];
		xSemaphoreGive(stateMutex);
	} else {
		return;
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

	u8g2.sendBuffer();
}

void drawBrowser() {
	uint32_t now = millis();
	String header = "/";
	size_t selection = 0;
	size_t offset = 0;
	std::vector<DirectoryEntry> entries;
	if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
		header = currentDirectory;
		selection = browserSelection;
		offset = browserOffset;
		xSemaphoreGive(stateMutex);
	}
	if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
		entries = directoryEntries;
		xSemaphoreGive(sdMutex);
	} else {
		return;
	}
	static float highlightRow = 0.0f;
	float targetRow = static_cast<float>(selection - offset);
	if (!UI_ANIMATIONS) highlightRow = targetRow;
	else highlightRow += (targetRow - highlightRow) * min(1.0f, (now % 1000) / 120.0f);
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
	Serial.begin(115200);
	Serial.println("MP3 player - BackgroundAudioMP3 example");
	preferences.begin("vistream", false);
	configuredSsid = preferences.isKey("wifiSsid") ? preferences.getString("wifiSsid") : WIFI_AP_SSID;
	configuredPassword = preferences.isKey("wifiPassword") ? preferences.getString("wifiPassword") : WIFI_AP_PASSWORD;
	currentGain = constrain(preferences.getFloat("lastVolume", currentGain), 0.0f, MAX_GAIN);
	shuffleEnabled = preferences.getBool("shuffle", false);
	uint8_t storedRepeatMode = preferences.getUChar("repeatMode", REPEAT_OFF);
	repeatMode = storedRepeatMode <= REPEAT_ALL ? static_cast<RepeatMode>(storedRepeatMode) : REPEAT_OFF;

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
	analogReadResolution(12);
	analogSetAttenuation(ADC_11db);
	updateBatteryVoltage();
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

	xTaskCreatePinnedToCore(
			wifiTask,
			"wifi-http",
			6144,
			nullptr,
			1,
			&wifiTaskHandle,
			0);

	pinMode(NEXT_BUTTON_PIN, INPUT_PULLUP);
	pinMode(PREV_BUTTON_PIN, INPUT_PULLUP);
	pinMode(PLAY_BUTTON_PIN, INPUT_PULLUP);
	pinMode(WIFI_SWITCH_PIN, INPUT_PULLUP);
	pinMode(BROWSER_BUTTON_PIN, INPUT_PULLUP);
	attachInterrupt(digitalPinToInterrupt(NEXT_BUTTON_PIN), handleNextISR, CHANGE);
	attachInterrupt(digitalPinToInterrupt(PREV_BUTTON_PIN), handlePrevISR, CHANGE);
	attachInterrupt(digitalPinToInterrupt(PLAY_BUTTON_PIN), handlePlayISR, CHANGE);
	attachInterrupt(digitalPinToInterrupt(BROWSER_BUTTON_PIN), handleBrowserISR, CHANGE);
}

void loop() {
	yield();
	unsigned long now = millis();
	if (now - lastBatterySampleMillis >= BATTERY_SAMPLE_INTERVAL_MS) {
		lastBatterySampleMillis = now;
		updateBatteryVoltage();
	}

	if (now - lastWifiSwitchPollMillis >= WIFI_SWITCH_POLL_MS) {
		lastWifiSwitchPollMillis = now;
		bool wifiSwitchState = digitalRead(WIFI_SWITCH_PIN) == LOW;
		if (wifiSwitchState != wifiApEnabled) {
			applyWifiHardwareState(wifiSwitchState);
		}
	}

	if (browserReleased && browserPressed) {
		browserReleased = false;
		browserPressed = false;
		if (now - lastButtonMillis > 300) {
			lastButtonMillis = now;
			if (currentScreen == SCREEN_BROWSER) {
				setScreen(SCREEN_NOWPLAYING);
				displayDirty = true;
			} else {
				enterBrowser();
			}
		}
	}

	if (nextReleased && nextPressed) {
		nextReleased = false;
		nextPressed = false;
		if (now - lastButtonMillis > 300) {
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
		if (now - lastButtonMillis > 300) {
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
				if (currentScreen == SCREEN_BROWSER) {
					setScreen(SCREEN_NOWPLAYING);
					displayDirty = true;
				} else {
					stopPlayback();
				}
			} else if (playClickPending && now - lastPlayReleaseMillis <= PLAY_DOUBLE_CLICK_MS) {
				Serial.println("Podwójny PLAY: zmiana trybu");
				playClickPending = false;
				cyclePlaybackMode();
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
