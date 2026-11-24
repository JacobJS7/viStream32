#include "net/web_server.h"
#include <WiFi.h>
#include <WebServer.h>
#include <SD.h>
#include <vector>
#include "config.h"
#include "state.h"
#include "audio/player.h"
#include "storage/sd_fs.h"
#include "storage/persistence.h"
#include "ui/display.h"

// src/web/index.html wbudowany w firmware (platformio.ini: board_build.embed_txtfiles).
extern const char index_html[] asm("_binary_src_web_index_html_start");

static WebServer webServer(80);
static File uploadFile;
static String uploadTargetPath;
static bool uploadFailed = false;

void webServerStart() { webServer.begin(); }
void webServerStop() { webServer.stop(); }
void webServerHandle() { webServer.handleClient(); }

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
	if (playlistEmpty()) {
		webServer.send(409, "text/plain", "No playlist");
		return;
	}
	closeTrack();
	advanceTrack();
	setScreen(SCREEN_NOWPLAYING);
	webServer.send(200, "text/plain", "OK");
}

void webPrev() {
	if (playlistEmpty()) {
		webServer.send(409, "text/plain", "No playlist");
		return;
	}
	closeTrack();
	retreatTrack();
	setScreen(SCREEN_NOWPLAYING);
	webServer.send(200, "text/plain", "OK");
}

void webMode() {
	String mode = webServer.hasArg("value") ? webServer.arg("value") : "single";
	if (mode != "shuffle" && mode != "repeat-one" && mode != "repeat-all" && mode != "single") {
		webServer.send(400, "text/plain", "Invalid mode");
		return;
	}
	xSemaphoreTake(stateMutex, portMAX_DELAY);
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
	} else {
		shuffleEnabled = false;
		repeatMode = REPEAT_OFF;
		shuffleOrder.clear();
	}
	xSemaphoreGive(stateMutex);
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
	if (!path.startsWith("/") || path.indexOf("..") >= 0 || path == "/") {
		webServer.send(400, "text/plain", "Invalid path");
		return;
	}
	while (path.length() > 1 && path.endsWith("/")) path.remove(path.length() - 1);

	String activePath;
	xSemaphoreTake(stateMutex, portMAX_DELAY);
	if (!tracks.empty() && currentTrack < tracks.size()) activePath = tracks[currentTrack];
	xSemaphoreGive(stateMutex);
	if (activePath.length() > 0 && (activePath == path || activePath.startsWith(path + "/"))) {
		stopPlayback();
	}

	bool removed = removePathRecursive(path);
	webServer.send(removed ? 200 : 404, "text/plain", removed ? "OK" : "Unable to delete");
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
	saveWifiConfig(ssid, password);
	webServer.send(200, "text/plain", "Restarting");
	delay(100);
	ESP.restart();
}

void webVolume() {
	setVolumePercent(webServer.arg("value").toInt());
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
	std::vector<DirectoryEntry> entries;
	if (!scanDirectory(path.c_str(), entries)) {
		webServer.send(404, "application/json", "{\"error\":\"directory not found\"}");
		return;
	}

	String json = "[";
	for (size_t index = 0; index < entries.size(); ++index) {
		const DirectoryEntry &entry = entries[index];
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

// Tylko rejestracja tras (raz). begin()/stop() robi applyWifiHardwareState(),
// bo WebServer::begin() bez uruchomionego Wi-Fi (brak stosu lwIP) kończy się assertem.
void registerWebRoutes() {
	webServer.on("/", HTTP_GET, []() { webServer.send(200, "text/html", index_html); });
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
}
