// viStream32 — punkt wejścia: kolejność inicjalizacji i uruchomienie tasków.
// Mapa modułów: zobacz CLAUDE.md (sekcja Architecture).
#include <Arduino.h>
#include <SD.h>
#include "config.h"
#include "state.h"
#include "audio/player.h"
#include "input/buttons.h"
#include "net/wifi_ap.h"
#include "power/battery.h"
#include "storage/persistence.h"
#include "ui/browser.h"
#include "ui/display.h"

static TaskHandle_t audioTaskHandle = nullptr;
static TaskHandle_t uiTaskHandle = nullptr;
static TaskHandle_t wifiTaskHandle = nullptr;

static void fail() {
	while (true) {
		delay(1000);
		ESP.restart();
	}
}

void setup() {
	Serial.begin(115200);
	Serial.println("MP3 player - BackgroundAudioMP3 example");
	persistenceBegin();

	displayBegin();
	setSplashStep("SD card...");
	splashProgress = 10;
	xTaskCreatePinnedToCore(
			uiTask,
			"oled-ui",
			4096,
			nullptr,
			1,
			&uiTaskHandle,
			0);

	// Brak karty: komunikat na OLED i ponawianie zamiast restartu w pętli.
	while (!SD.begin(SD_CS)) {
		Serial.println("Błąd karty SD! Ponawiam za 2 s");
		sdMissing = true;
		SD.end();
		delay(2000);
	}
	sdMissing = false;
	Serial.println("Karta SD OK");
	batteryBegin();
	updateBatteryVoltage();
	setSplashStep("Audio...");
	splashProgress = 40;

	if (!playerBegin()) {
		Serial.println("Nie udało się uruchomić MP3 playera");
		fail();
	}
	setSplashStep("Playlist...");
	splashProgress = 70;

	audioMutex = xSemaphoreCreateMutex();
	stateMutex = xSemaphoreCreateMutex();
	sdMutex = xSemaphoreCreateMutex();
	if (audioMutex == nullptr || stateMutex == nullptr || sdMutex == nullptr) fail();

	currentDirectory = "/";
	String savedFolder;
	uint16_t savedTrack;
	uint32_t savedPosition;
	loadResumeState(savedFolder, savedTrack, savedPosition);
	bool restored = false;
	if (savedFolder.length() > 0) {
		restored = loadPlaylistFromDirectory(savedFolder, savedTrack);
		if (restored) restorePosition(savedPosition);
	}
	if (restored) {
		setScreen(SCREEN_NOWPLAYING);
	} else if (loadPlaylistFromDirectory("/muzyka", 0)) {
		setScreen(SCREEN_NOWPLAYING);
	} else {
		currentDirectory = "/";
		refreshBrowser();
		setScreen(SCREEN_BROWSER);
	}
	setSplashStep("Ready");
	splashProgress = 100;
	uiReady = true;

	// Piny muszą mieć pull-upy zanim wifiTask odczyta przełącznik Wi-Fi.
	buttonsInitPins();

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

	buttonsAttachInterrupts();
}

void loop() {
	yield();
	unsigned long now = millis();
	batteryPoll(now);
	buttonsPoll(now);

	if (currentScreen == SCREEN_NOWPLAYING) {
		updatePlaybackPosition();
	}
	persistencePoll(now);

	delay(10);
}
