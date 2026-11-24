#pragma once
// Stan współdzielony przez kilka modułów/tasków.
// Zmienne używane tylko w jednym module są `static` w jego .cpp, nie tutaj.
//
// Mutexy (FreeRTOS, nierekurencyjne). Kolejność brania: stateMutex -> audioMutex -> sdMutex.
//   stateMutex: playlista i pozycja (tracks, currentTrack, shuffleOrder, repeatMode,
//               shuffleEnabled, playlistDirectory, currentDirectory, directoryEntries,
//               browserSelection/Offset, currentPositionSec)
//   audioMutex: dekoder mp3, currentGain, currentTrackTitle/Artist/Bitrate
//   sdMutex:    każdy dostęp do karty SD (pliki, katalogi, upload/download)

#include <Arduino.h>
#include <vector>
#include "types.h"

// --- Playlista / odtwarzanie ---
extern std::vector<String> tracks;            // pełne ścieżki .mp3 bieżącej playlisty
extern size_t currentTrack;
extern std::vector<size_t> shuffleOrder;
extern RepeatMode repeatMode;
extern bool shuffleEnabled;
extern String playlistDirectory;              // folder playlisty (zapisywany do NVS)
extern volatile bool playbackPaused;
extern volatile bool playbackStopped;
extern float currentGain;
extern volatile uint32_t currentPositionSec;
extern volatile uint32_t currentTrackDurationSec;
// Z tagów ID3 bieżącego utworu (puste gdy brak). Chronione przez audioMutex.
extern String currentTrackTitle;
extern String currentTrackArtist;
extern uint16_t currentTrackBitrate;          // kbps pierwszej ramki, 0 = nieznany

// --- Przeglądarka OLED ---
extern String currentDirectory;               // folder pokazywany w przeglądarce
extern std::vector<DirectoryEntry> directoryEntries;
extern size_t browserSelection;
extern size_t browserOffset;

// --- UI / system ---
extern Screen currentScreen;
extern volatile bool uiReady;
extern volatile bool sdMissing;
extern volatile uint8_t splashProgress;
extern volatile bool otaInProgress;
extern volatile uint8_t otaProgress;          // 0..100
extern volatile uint32_t otaWrittenKb;        // do ekranu OTA
extern volatile uint32_t otaTotalKb;          // 0 = nieznany
extern float batteryVoltage;

// --- Wi-Fi ---
extern bool wifiApEnabled;
extern String configuredSsid;
extern String configuredPassword;

// --- Synchronizacja ---
extern SemaphoreHandle_t audioMutex;
extern SemaphoreHandle_t stateMutex;
extern SemaphoreHandle_t sdMutex;
