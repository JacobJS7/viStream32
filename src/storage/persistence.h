#pragma once
// Zapis/odczyt stanu w NVS (Preferences, namespace "vistream").
#include <Arduino.h>

// Otwiera NVS i wczytuje ustawienia: SSID/hasło AP, głośność, shuffle, repeat.
void persistenceBegin();
// Zapamiętany folder/utwór/pozycja do wznowienia odtwarzania.
void loadResumeState(String &folder, uint16_t &trackIndex, uint32_t &positionSec);
void saveWifiConfig(const String &ssid, const String &password);
// Zgłasza potrzebę zapisu; faktyczny zapis robi persistencePoll() z loop().
void requestStateSave();
void saveStateToNvs();
// Zapisuje stan, gdy zgłoszono zapis albo minął STATE_SAVE_INTERVAL_MS podczas odtwarzania.
void persistencePoll(uint32_t now);
