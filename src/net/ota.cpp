#include "net/ota.h"
#include <Update.h>
#include "state.h"
#include "audio/player.h"
#include "net/web_server.h"

static bool otaFailed = false;
static String otaError;
static uint32_t otaExpectedBytes = 0;
static uint32_t otaWrittenBytes = 0;

// Przerywa aktualizację. `closeConnection` = zamknij połączenie, żeby WebServer
// nie czytał reszty body bez oddawania CPU (watchdog IDLE0 — jak przy uploadzie MP3).
static void otaFail(const String &reason, bool closeConnection) {
	otaFailed = true;
	otaError = reason;
	Update.abort();
	otaInProgress = false;
	Serial.printf("[ota] BŁĄD: %s\n", reason.c_str());
	if (closeConnection) webServerInstance().client().stop();
}

void handleOtaUpload(HTTPUpload &upload) {
	if (upload.status == UPLOAD_FILE_START) {
		otaFailed = false;
		otaError = "";
		otaWrittenBytes = 0;
		otaProgress = 0;
		otaWrittenKb = 0;
		otaTotalKb = 0;
		String name = upload.filename;
		name.toLowerCase();
		if (!name.endsWith(".bin")) {
			otaFail("Plik musi mieć rozszerzenie .bin", true);
			return;
		}
		// Content-Length obejmuje też nagłówki multipart — do paska postępu wystarczy.
		otaExpectedBytes = static_cast<uint32_t>(max(1, webServerInstance().clientContentLength()));
		otaTotalKb = otaExpectedBytes / 1024;
		Serial.printf("[ota] start %s, ok. %u B\n", upload.filename.c_str(), otaExpectedBytes);
		stopPlayback();
		otaInProgress = true;
		if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
			otaFail(Update.errorString(), true);
			return;
		}
	} else if (upload.status == UPLOAD_FILE_WRITE) {
		if (otaFailed) return;
		if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
			// Np. pierwszy bajt pliku to nie 0xE9 (to nie jest obraz firmware ESP32).
			otaFail(Update.errorString(), true);
			return;
		}
		otaWrittenBytes += upload.currentSize;
		otaWrittenKb = otaWrittenBytes / 1024;
		otaProgress = static_cast<uint8_t>(min(99UL, static_cast<unsigned long>(otaWrittenBytes) * 100UL / otaExpectedBytes));
	} else if (upload.status == UPLOAD_FILE_END) {
		if (otaFailed) return;
		if (Update.end(true)) {
			otaProgress = 100;
			Serial.printf("[ota] zapisano %u B, obraz poprawny\n", otaWrittenBytes);
		} else {
			otaFail(Update.errorString(), false);  // body już odebrane — odeślemy błąd
		}
	} else if (upload.status == UPLOAD_FILE_ABORTED) {
		if (!otaFailed) otaFail("Połączenie przerwane", false);
	}
	vTaskDelay(1);  // oddaj CPU (yield() nie wpuszcza IDLE0)
}

void otaComplete() {
	WebServer &server = webServerInstance();
	server.sendHeader("Connection", "close");
	if (otaFailed || !Update.isFinished()) {
		server.send(500, "text/plain", "Aktualizacja nieudana: " + otaError);
		otaInProgress = false;
		return;
	}
	server.send(200, "text/plain", "OK, restart");
	Serial.println("[ota] restart do nowego firmware");
	delay(500);
	ESP.restart();
}
