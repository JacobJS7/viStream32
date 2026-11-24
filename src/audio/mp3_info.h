#pragma once
// Odczyt metadanych MP3 bez dekodowania: tagi ID3 (tytuł/wykonawca), pierwsza ramka
// (bitrate, sample rate) i nagłówek Xing/Info/VBRI (liczba ramek, TOC dla VBR).
// Daje prawdziwy czas trwania i mapowanie sekunda -> bajt dla seeka i wznawiania.
#include <Arduino.h>
#include <FS.h>

struct Mp3Info {
	bool valid = false;           // znaleziono poprawną ramkę MPEG Layer III
	uint32_t audioStart = 0;      // offset pierwszej ramki audio (za ID3v2)
	uint32_t audioBytes = 0;      // bajty audio (bez ID3v2/ID3v1)
	uint32_t durationSec = 1;
	uint32_t bytesPerSec = 0;     // średnia, do liniowego seeka
	uint16_t bitrateKbps = 0;     // pierwsza ramka (dla VBR orientacyjnie)
	uint32_t sampleRate = 0;
	bool vbr = false;
	bool hasToc = false;
	uint8_t toc[100] = {0};       // Xing TOC: procent czasu -> procent/256 bajtów
	String title;                 // UTF-8, może być pusty
	String artist;
};

// Wołać pod sdMutex. Zawsze wypełnia `out` (przy błędzie wartości z estymaty
// `fallbackBytesPerSec`). Zostawia plik ustawiony na out.audioStart.
bool readMp3Info(File &file, Mp3Info &out, uint32_t fallbackBytesPerSec);

// Offset bajtowy w pliku odpowiadający sekundzie `sec` (TOC dla VBR, inaczej liniowo).
uint32_t mp3ByteOffsetForSecond(const Mp3Info &info, uint32_t sec, uint32_t fileSize);
