#include "storage/persistence.h"
#include <Preferences.h>
#include "config.h"
#include "state.h"

static Preferences preferences;
static volatile bool stateSaveRequested = false;
static uint32_t lastStateSaveMillis = 0;

void persistenceBegin() {
	preferences.begin("vistream", false);
	configuredSsid = preferences.isKey("wifiSsid") ? preferences.getString("wifiSsid") : WIFI_AP_SSID;
	configuredPassword = preferences.isKey("wifiPassword") ? preferences.getString("wifiPassword") : WIFI_AP_PASSWORD;
	currentGain = constrain(preferences.getFloat("lastVolume", currentGain), 0.0f, MAX_GAIN);
	shuffleEnabled = preferences.getBool("shuffle", false);
	uint8_t storedRepeatMode = preferences.getUChar("repeatMode", REPEAT_OFF);
	repeatMode = storedRepeatMode <= REPEAT_ALL ? static_cast<RepeatMode>(storedRepeatMode) : REPEAT_OFF;
}

void loadResumeState(String &folder, uint16_t &trackIndex, uint32_t &positionSec) {
	folder = preferences.getString("lastFolder", "");
	trackIndex = preferences.getUShort("lastTrackIndex", 0);
	positionSec = preferences.getUInt("lastPositionSec", 0);
}

void saveWifiConfig(const String &ssid, const String &password) {
	preferences.putString("wifiSsid", ssid);
	preferences.putString("wifiPassword", password);
}

void requestStateSave() {
	stateSaveRequested = true;
}

void saveStateToNvs() {
	String folder;
	uint16_t trackIndex;
	uint32_t position;
	float gain;
	bool shuffle;
	uint8_t savedRepeatMode;

	xSemaphoreTake(stateMutex, portMAX_DELAY);
	folder = playlistDirectory;
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

void persistencePoll(uint32_t now) {
	if (stateSaveRequested ||
		(currentScreen == SCREEN_NOWPLAYING && now - lastStateSaveMillis >= STATE_SAVE_INTERVAL_MS)) {
		saveStateToNvs();
	}
}
