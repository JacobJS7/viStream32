#pragma once
// Aktualizacja firmware przez WWW (POST /update, multipart z plikiem .bin).
// Wymaga tablicy partycji z dwoma slotami OTA (platformio.ini: min_spiffs.csv).
// Plik do wgrania: .pio/build/esp32doit-devkit-v1/firmware.bin
#include <Arduino.h>
#include <WebServer.h>

// Handler kawałków uploadu (zapis do nieaktywnego slotu przez bibliotekę Update).
void handleOtaUpload(HTTPUpload &upload);
// Odpowiedź po zakończeniu uploadu; po sukcesie restartuje ESP.
void otaComplete();
