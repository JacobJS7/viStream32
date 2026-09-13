#include <Arduino.h>
#include <ESP32I2SAudio.h>
#include <SD.h>
#include <BackgroundAudio.h>
#include <SPI.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <vector>

// I2S DOUT uses GPIO 22, so I2C is moved from the default SCL pin.
#define I2S_BCLK  26
#define I2S_LRC   25
#define I2S_DOUT  27
#define I2C_SDA   21
#define I2C_SCL   22

#define SD_CS     5
#define NEXT_BUTTON_PIN 4

ESP32I2SAudio audio(I2S_BCLK, I2S_LRC, I2S_DOUT);
BackgroundAudioMP3 mp3(audio);
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

File f;
uint8_t filebuff[512];
std::vector<String> tracks;
size_t currentTrack = 0;

volatile bool nextRequested = false;
volatile bool trackEnded = false;
unsigned long lastButtonMillis = 0;
SemaphoreHandle_t audioMutex;
TaskHandle_t audioTaskHandle = nullptr;

void IRAM_ATTR handleNextISR() { nextRequested = true; }

void scanMusicDir() {
	tracks.clear();
	File dir = SD.open("/muzyka");
	if (!dir) {
		Serial.println("Brak folderu /muzyka");
		return;
	}

	File entry = dir.openNextFile();
	while (entry) {
		if (!entry.isDirectory()) {
			String name = entry.name();
			if (name.endsWith(".mp3") || name.endsWith(".MP3")) {
				tracks.push_back(String("/muzyka/") + name);
			}
		}
		entry.close();
		entry = dir.openNextFile();
	}
	dir.close();
	Serial.print("Znaleziono utworów: ");
	Serial.println((int)tracks.size());
}

bool openTrack(size_t idx) {
	if (tracks.empty()) return false;
	if (idx >= tracks.size()) idx = 0;

	xSemaphoreTake(audioMutex, portMAX_DELAY);
	if (f) f.close();
	Serial.print("Otwieram: ");
	Serial.println(tracks[idx]);
	f = SD.open(tracks[idx].c_str());
	bool opened = f;
	if (opened) mp3.flush();
	xSemaphoreGive(audioMutex);

	if (!opened) {
		Serial.println("Nie można otworzyć pliku");
		return false;
	}
	trackEnded = false;
	return true;
}

void closeTrack() {
	xSemaphoreTake(audioMutex, portMAX_DELAY);
	if (f) f.close();
	xSemaphoreGive(audioMutex);
}

void audioFeedTask(void *parameter) {
	(void)parameter;
	for (;;) {
		bool fedData = false;

		xSemaphoreTake(audioMutex, portMAX_DELAY);
		while (f && mp3.availableForWrite() > 512) {
			int len = f.read(filebuff, sizeof(filebuff));
			if (len > 0) {
				mp3.write(filebuff, len);
				fedData = true;
			}
			if (len != (int)sizeof(filebuff)) {
				f.close();
				trackEnded = true;
				Serial.println("Koniec utworu: zamknięto plik SD");
				break;
			}
		}
		xSemaphoreGive(audioMutex);

		if (!fedData) vTaskDelay(pdMS_TO_TICKS(10));
	}
}

void drawNowPlaying() {
	String title = tracks.empty() ? String("Brak utworu") : tracks[currentTrack];
	int separator = title.lastIndexOf('/');
	if (separator >= 0) title = title.substring(separator + 1);

	u8g2.clearBuffer();
	u8g2.setFont(u8g2_font_6x10_tf);
	u8g2.drawStr(0, 10, "Now Playing");
	u8g2.drawHLine(0, 13, 128);
	u8g2.drawUTF8(0, 31, title.c_str());
	u8g2.drawTriangle(8, 47, 8, 59, 18, 53);
	u8g2.drawStr(25, 56, "PLAY");
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
	mp3.setGain(0.05f);
	scanMusicDir();
	if (tracks.empty()) {
		Serial.println("Brak utworów w /muzyka");
		fail();
	}

	if (!mp3.begin()) {
		Serial.println("Nie udało się uruchomić MP3 playera");
		fail();
	}

	audioMutex = xSemaphoreCreateMutex();
	if (audioMutex == nullptr || !openTrack(0)) fail();

	xTaskCreatePinnedToCore(
			audioFeedTask,
			"audio-feed",
			4096,
			nullptr,
			1,
			&audioTaskHandle,
			1);

	pinMode(NEXT_BUTTON_PIN, INPUT_PULLUP);
	attachInterrupt(digitalPinToInterrupt(NEXT_BUTTON_PIN), handleNextISR, FALLING);
	drawNowPlaying();
}

void loop() {
	if (trackEnded) {
		currentTrack = (currentTrack + 1) % tracks.size();
		openTrack(currentTrack);
		drawNowPlaying();
	}

	if (nextRequested) {
		unsigned long now = millis();
		nextRequested = false;
		if (now - lastButtonMillis > 300) {
			lastButtonMillis = now;
			Serial.println("Przycisk NEXT: pomijanie utworu");
			closeTrack();
			currentTrack = (currentTrack + 1) % tracks.size();
			openTrack(currentTrack);
			drawNowPlaying();
		}
	}

	delay(10);
}

void audio_info(const char *info)    { Serial.println(info); }
void audio_eof_mp3(const char *info) { Serial.println("Koniec pliku"); }
