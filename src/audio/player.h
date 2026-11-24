#pragma once
// Odtwarzacz: playlista, sterowanie, głośność i task karmiący dekoder (Core 1).
// Czas/pozycja/seek to estymaty z APPROXIMATE_MP3_BYTES_PER_SECOND
// (BackgroundAudioMP3 nie udostępnia indeksu czasu).
#include <Arduino.h>

// Inicjalizacja I2S i dekodera MP3. false = błąd.
bool playerBegin();
void audioFeedTask(void *parameter);

bool loadPlaylistFromDirectory(const String &path, size_t selectedIndex);
bool playFileFromPath(const String &filePath);
bool openTrack(size_t idx);
void closeTrack();
void advanceTrack();
void retreatTrack();
void handleTrackEnd();
void stopAtEnd();
void togglePlayPause();
void stopPlayback();
bool playlistEmpty();

void seekByBytes(int32_t delta);
void restorePosition(uint32_t positionSec);
void updatePlaybackPosition();
void resetPlaybackPosition(uint32_t positionSec = 0);

void volumeUp();
void volumeDown();
void setVolumePercent(int percent);

// Wymaga stateMutex po stronie wywołującego.
void rebuildShuffleOrder(size_t firstTrack);
void cyclePlaybackMode();
