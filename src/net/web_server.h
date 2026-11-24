#pragma once
// Serwer HTTP: pilot, eksplorator plików SD, upload/download, konfiguracja AP.
// Strona: src/web/index.html (wbudowana przez board_build.embed_txtfiles).
#include <Arduino.h>

// Rejestracja tras (raz). Start/stop tylko przy włączonym Wi-Fi —
// WebServer::begin() bez stosu lwIP kończy się assertem.
void registerWebRoutes();
void webServerStart();
void webServerStop();
void webServerHandle();
