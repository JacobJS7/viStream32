#include "audio/player.h"
#include <ESP32I2SAudio.h>
#include <BackgroundAudio.h>
#include <SD.h>
#include <vector>
#include "config.h"
#include "state.h"
#include "storage/sd_fs.h"
#include "storage/persistence.h"
#include "ui/display.h"
#include "audio/mp3_info.h"

static ESP32I2SAudio audio(I2S_BCLK, I2S_LRC, I2S_DOUT);
static BackgroundAudioMP3 mp3(audio);
static File f;                     // aktualnie odtwarzany plik (sdMutex)
static Mp3Info trackInfo;          // metadane bieżącego utworu (audioMutex + sdMutex)
static uint8_t filebuff[512];
static size_t shuffleCursor = 0;
static uint32_t trackPositionBaseSec = 0;
static uint32_t playbackReferenceMillis = 0;

bool playerBegin() {
	audio.setPins(I2S_BCLK, I2S_LRC, I2S_DOUT);
	mp3.setGain(currentGain);
	return mp3.begin();
}

void updatePlaybackPosition() {
	if (playbackPaused || playbackStopped) return;
	uint32_t elapsed = (millis() - playbackReferenceMillis) / 1000;
	xSemaphoreTake(stateMutex, portMAX_DELAY);
	currentPositionSec = trackPositionBaseSec + elapsed;
	xSemaphoreGive(stateMutex);
}

void resetPlaybackPosition(uint32_t positionSec) {
	trackPositionBaseSec = positionSec;
	playbackReferenceMillis = millis();
	currentPositionSec = positionSec;
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
	xSemaphoreTake(stateMutex, portMAX_DELAY);
	if (repeatMode == REPEAT_OFF && !shuffleEnabled) {
		shuffleEnabled = true;
		repeatMode = REPEAT_OFF;
		rebuildShuffleOrder(currentTrack);
		Serial.println("Tryb: SHUFFLE");
		showToast("SHUFFLE", TOAST_ICON_SHUFFLE);
	} else if (shuffleEnabled) {
		shuffleEnabled = false;
		repeatMode = REPEAT_ONE;
		Serial.println("Tryb: REPEAT ONE");
		showToast("REPEAT ONE", TOAST_ICON_REPEAT);
	} else if (repeatMode == REPEAT_ONE) {
		repeatMode = REPEAT_ALL;
		Serial.println("Tryb: REPEAT ALL");
		showToast("REPEAT ALL", TOAST_ICON_REPEAT);
	} else {
		repeatMode = REPEAT_OFF;
		Serial.println("Tryb: NORMAL");
		showToast("NORMAL", TOAST_ICON_PLAY);
	}
	xSemaphoreGive(stateMutex);
	requestStateSave();
}

// Przewija otwarty plik do sekundy `sec` (TOC Xing dla VBR, inaczej liniowo).
// Wymaga audioMutex i sdMutex. Zwraca false gdy brak pliku lub seek się nie udał.
static bool seekFileToSecond(uint32_t sec) {
	if (!f) return false;
	uint32_t offset = mp3ByteOffsetForSecond(trackInfo, sec, static_cast<uint32_t>(f.size()));
	if (!f.seek(offset)) return false;
	mp3.flush();
	return true;
}

void restorePosition(uint32_t positionSec) {
	if (positionSec >= currentTrackDurationSec) positionSec = 0;
	xSemaphoreTake(audioMutex, portMAX_DELAY);
	xSemaphoreTake(sdMutex, portMAX_DELAY);
	seekFileToSecond(positionSec);
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
	if (opened) {
		// Metadane (ID3, bitrate, Xing/VBRI); plik zostaje ustawiony na pierwszej
		// ramce audio, więc dekoder nie dostaje tagów ani okładki.
		readMp3Info(f, trackInfo, APPROXIMATE_MP3_BYTES_PER_SECOND);
		mp3.flush();
		currentTrackDurationSec = trackInfo.durationSec;
		currentTrackTitle = trackInfo.title;
		currentTrackArtist = trackInfo.artist;
		currentTrackBitrate = trackInfo.bitrateKbps;
	}
	xSemaphoreGive(sdMutex);
	xSemaphoreGive(audioMutex);

	if (!opened) {
		Serial.println("Nie można otworzyć pliku");
		return false;
	}
	Serial.printf("MP3: %s %u kbps %u Hz %s, czas %u s, start audio %u\n",
			trackInfo.valid ? "OK" : "estymata", trackInfo.bitrateKbps, trackInfo.sampleRate,
			trackInfo.vbr ? "VBR" : "CBR", currentTrackDurationSec, trackInfo.audioStart);
	resetPlaybackPosition();
	return true;
}

bool loadPlaylistFromDirectory(const String &path, size_t selectedIndex) {
	std::vector<DirectoryEntry> entries;
	if (!scanDirectory(path.c_str(), entries)) return false;

	std::vector<String> playlist;
	for (const DirectoryEntry &entry : entries) {
		if (!entry.isDirectory && !entry.isParent) {
			playlist.push_back(joinPath(path, entry.name));
		}
	}
	if (playlist.empty()) return false;
	if (selectedIndex >= playlist.size()) selectedIndex = 0;

	xSemaphoreTake(stateMutex, portMAX_DELAY);
	playlistDirectory = path;
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
	std::vector<DirectoryEntry> entries;
	if (!scanDirectory(folder.c_str(), entries)) return false;

	size_t selectedIndex = 0;
	bool found = false;
	for (const DirectoryEntry &entry : entries) {
		if (!entry.isDirectory && !entry.isParent) {
			if (entry.name == fileName) found = true;
			if (!found) ++selectedIndex;
		}
	}
	if (!found) return false;
	bool opened = loadPlaylistFromDirectory(folder, selectedIndex);
	if (opened) {
		setScreen(SCREEN_NOWPLAYING);
		requestStateSave();
	}
	return opened;
}

void advanceTrack() {
	xSemaphoreTake(stateMutex, portMAX_DELAY);
	if (tracks.empty()) {
		xSemaphoreGive(stateMutex);
		return;
	}
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
		xSemaphoreTake(stateMutex, portMAX_DELAY);
		openTrack(currentTrack);
		xSemaphoreGive(stateMutex);
		return;
	}

	if (shuffleEnabled && !shuffleOrder.empty()) {
		if (shuffleCursor + 1 < shuffleOrder.size()) {
			advanceTrack();
		} else if (repeatMode == REPEAT_ALL) {
			xSemaphoreTake(stateMutex, portMAX_DELAY);
			rebuildShuffleOrder(currentTrack);
			xSemaphoreGive(stateMutex);
			advanceTrack();
		} else {
			stopAtEnd();
		}
		return;
	}

	if (currentTrack + 1 < tracks.size()) {
		advanceTrack();
	} else if (repeatMode == REPEAT_ALL) {
		advanceTrack();  // (ostatni + 1) % size == 0
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
	if (tracks.empty()) {
		xSemaphoreGive(stateMutex);
		return;
	}
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

void seekToSecond(uint32_t sec) {
	uint32_t duration = currentTrackDurationSec;
	if (duration > 1 && sec >= duration) sec = duration - 1;
	xSemaphoreTake(audioMutex, portMAX_DELAY);
	xSemaphoreTake(sdMutex, portMAX_DELAY);
	if (!playbackStopped && seekFileToSecond(sec)) {
		resetPlaybackPosition(sec);
		Serial.printf("Seek: %u s\n", sec);
	}
	xSemaphoreGive(sdMutex);
	xSemaphoreGive(audioMutex);
	requestStateSave();
}

void seekBySeconds(int32_t deltaSec) {
	updatePlaybackPosition();  // bierze stateMutex — przed audio/sd (kolejność blokad)
	int64_t target = static_cast<int64_t>(currentPositionSec) + deltaSec;
	seekToSecond(target < 0 ? 0 : static_cast<uint32_t>(target));
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

bool playlistEmpty() {
	xSemaphoreTake(stateMutex, portMAX_DELAY);
	bool empty = tracks.empty();
	xSemaphoreGive(stateMutex);
	return empty;
}

void setVolumePercent(int percent) {
	int value = constrain(percent, 0, 100);
	xSemaphoreTake(audioMutex, portMAX_DELAY);
	currentGain = MAX_GAIN * value / 100.0f;
	mp3.setGain(currentGain);
	xSemaphoreGive(audioMutex);
	Serial.print("currentGain: ");
	Serial.println(currentGain, 3);
	requestStateSave();
	showVolumeOverlay();
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
