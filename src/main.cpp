#include <Arduino.h>
#include <ESP32I2SAudio.h>
#include <SD.h>
#include <BackgroundAudio.h>
#include <SPI.h>
#include <Wire.h>
#include <U8g2lib.h>
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
#define BROWSER_RETURN_MS 1500
#define SEEK_BYTES_10_SECONDS 160000UL

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
bool displayDirty = true;

volatile bool nextPressed = false;
volatile bool prevPressed = false;
volatile bool playPressed = false;
volatile bool nextReleased = false;
volatile bool prevReleased = false;
volatile bool playReleased = false;
volatile uint32_t nextPressMillis = 0;
volatile uint32_t prevPressMillis = 0;
volatile uint32_t playPressMillis = 0;
unsigned long lastButtonMillis = 0;
SemaphoreHandle_t audioMutex;
SemaphoreHandle_t stateMutex;
SemaphoreHandle_t sdMutex;
TaskHandle_t audioTaskHandle = nullptr;
size_t displayedTrack = static_cast<size_t>(-1);
bool displayedPaused = false;
volatile bool playbackPaused = false;
volatile bool playbackStopped = false;
float currentGain = 0.05f;
float gainBeforeMute = 0.05f;
bool muted = false;

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
	return true;
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

	xSemaphoreTake(audioMutex, portMAX_DELAY);
	if (playbackPaused) {
		mp3.unpause();
		playbackPaused = false;
	} else {
		mp3.pause();
		playbackPaused = true;
	}
	xSemaphoreGive(audioMutex);
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
			Serial.println(delta > 0 ? "Fast forward 10 s" : "Rewind 10 s");
		}
	}
	xSemaphoreGive(sdMutex);
	xSemaphoreGive(audioMutex);
}

void volumeUp() {
	xSemaphoreTake(audioMutex, portMAX_DELAY);
	currentGain = min(currentGain + 0.01f, 1.0f);
	if (!muted) mp3.setGain(currentGain);
	xSemaphoreGive(audioMutex);
}

void volumeDown() {
	xSemaphoreTake(audioMutex, portMAX_DELAY);
	currentGain = max(currentGain - 0.01f, 0.0f);
	if (!muted) mp3.setGain(currentGain);
	xSemaphoreGive(audioMutex);
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
	currentScreen = SCREEN_BROWSER;
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
		currentScreen = SCREEN_NOWPLAYING;
		displayDirty = true;
	}
	xSemaphoreGive(stateMutex);
}

void audioFeedTask(void *parameter) {
	(void)parameter;
	for (;;) {
		bool fedData = false;
		bool reachedEnd = false;

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
			advanceTrack();
		}
	}
}

void drawNowPlaying() {
	xSemaphoreTake(stateMutex, portMAX_DELAY);
	String title = tracks.empty() ? String("Brak utworu") : tracks[currentTrack];
	xSemaphoreGive(stateMutex);
	xSemaphoreTake(audioMutex, portMAX_DELAY);
	bool paused = playbackPaused || playbackStopped;
	xSemaphoreGive(audioMutex);
	int separator = title.lastIndexOf('/');
	if (separator >= 0) title = title.substring(separator + 1);
	if (title.length() > 20) title = title.substring(0, 20);

	u8g2.clearBuffer();
	u8g2.setFont(u8g2_font_6x10_tf);
	u8g2.drawStr(0, 10, "Now Playing");
	u8g2.drawHLine(0, 13, 128);
	u8g2.drawUTF8(0, 31, title.c_str());
	u8g2.drawTriangle(8, 47, 8, 59, 18, 53);
	u8g2.drawStr(25, 56, paused ? "PAUSE" : "PLAY");
	u8g2.sendBuffer();
}

void drawBrowser() {
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
		if (index == browserSelection) {
			u8g2.setDrawColor(1);
			u8g2.drawBox(0, baseline - 9, 128, 11);
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

void fail() {
	while (true) {
		delay(1000);
		ESP.restart();
	}
}

void setup() {
	Serial.begin(9600);
	Serial.println("MP3 player - BackgroundAudioMP3 example");

	Wire.begin(I2C_SDA, I2C_SCL);
	u8g2.begin();
	u8g2.clearBuffer();
	u8g2.setFont(u8g2_font_6x10_tf);
	u8g2.drawStr(0, 20, "Starting...");
	u8g2.sendBuffer();

	if (!SD.begin(SD_CS)) {
		Serial.println("Błąd karty SD!");
		fail();
	}
	Serial.println("Karta SD OK");

	audio.setPins(I2S_BCLK, I2S_LRC, I2S_DOUT);
	mp3.setGain(currentGain);

	if (!mp3.begin()) {
		Serial.println("Nie udało się uruchomić MP3 playera");
		fail();
	}

	audioMutex = xSemaphoreCreateMutex();
	stateMutex = xSemaphoreCreateMutex();
	sdMutex = xSemaphoreCreateMutex();
	if (audioMutex == nullptr || stateMutex == nullptr || sdMutex == nullptr) fail();

	currentDirectory = "/";
	refreshBrowser();

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
	if (nextReleased && nextPressed) {
		nextReleased = false;
		nextPressed = false;
		if (now - lastButtonMillis > 300) {
			lastButtonMillis = now;
			if (currentScreen == SCREEN_BROWSER) {
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
			if (currentScreen == SCREEN_BROWSER) {
				moveBrowserSelection(-1);
			} else if (now - prevPressMillis >= BROWSER_RETURN_MS) {
				Serial.println("Powrót do przeglądarki");
				enterBrowser();
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
		if (now - lastButtonMillis > 300) {
			lastButtonMillis = now;
			if (currentScreen == SCREEN_BROWSER) {
				if (now - playPressMillis < LONG_PRESS_MS) selectBrowserEntry();
			} else if (now - playPressMillis >= LONG_PRESS_MS) {
				Serial.println("Długie PLAY: stop");
				stopPlayback();
			} else {
				togglePlayPause();
			}
		}
	}

	size_t trackToDisplay;
	xSemaphoreTake(stateMutex, portMAX_DELAY);
	trackToDisplay = currentTrack;
	xSemaphoreGive(stateMutex);
	xSemaphoreTake(audioMutex, portMAX_DELAY);
	bool pausedToDisplay = playbackPaused || playbackStopped;
	xSemaphoreGive(audioMutex);
	if (currentScreen == SCREEN_NOWPLAYING &&
		(trackToDisplay != displayedTrack || pausedToDisplay != displayedPaused)) {
		displayDirty = true;
	}
	if (displayDirty) {
		if (currentScreen == SCREEN_BROWSER) {
			drawBrowser();
		} else {
			drawNowPlaying();
		}
		displayedTrack = trackToDisplay;
		displayedPaused = pausedToDisplay;
		displayDirty = false;
	}

	delay(10);
}

void audio_info(const char *info)    { Serial.println(info); }
void audio_eof_mp3(const char *info) { Serial.println("Koniec pliku"); }
