# Projekt: vistream32 — Zaawansowany Odtwarzacz Audio na ESP32

Specyfikacja funkcjonalna i sprzętowa do implementacji firmware'u.

---

## 0. Konfiguracja projektu (PlatformIO)

```ini
[env:esp32doit-devkit-v1]
platform = espressif32
board = esp32doit-devkit-v1
framework = arduino
lib_deps =
    earlephilhower/BackgroundAudio@^1.4.4
    olikraus/U8g2@^2.35.19
```

> Dodaj bibliotekę **U8g2** (a nie starego, niewspieranego `U8glib`) — obsługuje SSD1306 i ma pełne wsparcie animacji poprzez rysowanie klatek do bufora (`u8g2.clearBuffer()` / `u8g2.sendBuffer()`).

### Bazowy, działający kod audio

W repozytorium/uploadach znajduje się plik **`main.cpp`** (dostarczony przez użytkownika), w którym **odtwarzanie MP3 z karty SD przez `BackgroundAudioMP3` działa poprawnie**. Codex powinien traktować ten plik jako **sprawdzoną bazę warstwy audio** i:

- **Nie przepisywać od zera** logiki: inicjalizacji `ESP32I2SAudio`, `BackgroundAudioMP3`, skanowania folderu `/muzyka`, otwierania plików, karmienia dekodera w pętli (`mp3.availableForWrite()` / `mp3.write()`).
- **Rozbudowywać** ten kod o kolejne funkcje z niniejszej specyfikacji (pauza, poprzedni utwór, głośność, UI, Wi-Fi itd.), zachowując działający mechanizm dekodowania.
- Przenieść docelowo pętlę karmienia dekodera (`feed loop`) do **taska na Core 1** (patrz sekcja 2), a UI/przyciski/Wi-Fi obsługiwać z Core 0, zamiast trzymać wszystko w jednym `loop()`.

### ⚠️ Konflikt pinów do rozwiązania

W dostarczonym `main.cpp`:
- `I2S_DOUT = 22`
- Domyślne piny I2C na ESP32 DoIT DevKit to **SDA = 21, SCL = 22** — czyli SSD1306 na domyślnym I2C **koliduje z `I2S_DOUT`**.

Codex musi albo:
1. Zainicjować I2C na innych pinach (np. `Wire.begin(21, 27)` lub podobnych wolnych GPIO), albo
2. Przenieść `I2S_DOUT` na inny wolny pin (np. GPIO 27 lub 33) i zostawić I2C na 21/22.

Należy też sprawdzić kolizje pozostałych pinów przy dodawaniu kolejnych przycisków (Prev, Play/Pause), przełącznika Jack-detect i pomiaru ADC baterii — DoIT DevKit V1 ma ograniczoną liczbę bezpiecznych GPIO (unikać 6-11 zajętych przez flash SPI; GPIO 34-39 to tylko wejścia, dobre do ADC/detekcji, bez `INPUT_PULLUP`).

---

## 1. Sprzęt (Hardware / BOM)

| Moduł | Rola |
|---|---|
| ESP32 (dual-core) | Główny mikrokontroler |
| Wyświetlacz SSD1306 (OLED, I2C) | Interfejs użytkownika |
| Moduł karty SD (SPI) | Przechowywanie plików audio |
| PCM5102A | DAC I2S, wyjście audio |
| PAM8403 + głośnik | Wzmacniacz + głośnik wbudowany |
| Gniazdo Jack (z detekcją wpięcia) | Wyjście słuchawkowe, automatyczne przełączanie |
| TP4056 | Ładowarka Li-Ion |
| Akumulator 18650 (lub podobny) | Zasilanie |
| Przyciski: Play, Next, Prev | Sterowanie fizyczne |
| Włącznik główny | Odcięcie zasilania |
| Dzielnik napięcia + ADC ESP32 | Pomiar napięcia baterii |

---

## 2. Architektura systemu (wielowątkowość)

- **Core 0** — Wi-Fi (AP + serwer WWW) oraz obsługa karty SD (operacje plikowe, file explorer, upload/download).
- **Core 1** — Dekodowanie audio i strumieniowanie do I2S (czas krytyczny, nie może być blokowane przez Wi-Fi).
- Komunikacja między rdzeniami: kolejki FreeRTOS (`xQueue`) / semafory dla komend (play/pause/next) i stanu (pozycja, głośność).

---

## 3. Podstawowe funkcje odtwarzacza (Audio Core)

- [ ] Play — uruchomienie wybranego utworu
- [ ] Pause — zatrzymanie z zachowaniem pozycji
- [ ] Stop — pełne zatrzymanie i powrót na początek
- [ ] Next — kolejny plik na liście
- [ ] Prev — poprzedni plik
- [ ] Fast Forward — przewijanie do przodu (np. skok 10 s)
- [ ] Rewind — przewijanie do tyłu
- [ ] Volume Up / Volume Down
- [ ] Mute — natychmiastowe wyciszenie jedną akcją

## 4. Kolejka i tryby odtwarzania

- [ ] Shuffle — losowa kolejność bez powtórzeń
- [ ] Repeat One — zapętlenie bieżącego utworu
- [ ] Repeat All — zapętlenie całej listy/folderu
- [ ] Tryb ciągły (Normal) — zatrzymanie po ostatnim utworze
- [ ] Auto-next — automatyczne przejście do kolejnego utworu po zakończeniu

## 5. Menu i UI na ekranie OLED (U8g2, z animacjami)

Biblioteka: **U8g2** (tryb pełnobuforowy `F` np. `U8G2_SSD1306_128X64_NONAME_F_HW_I2C`), aby móc płynnie rysować i podmieniać całe klatki bez migotania.

Zasada ogólna: UI ma być rysowane jako **maszyna stanów ekranów** (`SCREEN_SPLASH`, `SCREEN_BROWSER`, `SCREEN_NOWPLAYING`, `SCREEN_MENU`, `SCREEN_LOWBATT`...), a rysowanie klatki wykonywane cyklicznie (np. co 30–50 ms z osobnego, lekkiego taska/timera na Core 0), żeby nie blokować audio na Core 1.

- [ ] **Ekran powitalny (Splash/Boot)** — logo/nazwa projektu z prostą animacją wejścia (fade/wipe albo pasek ładowania podczas inicjalizacji SD/audio)
- [ ] Przeglądarka plików/folderów (nawigacja po drzewie katalogów SD), z animacją przewijania listy (płynne przesunięcie zaznaczenia / podświetlonego wiersza)
- [ ] Wybór folderu jako playlisty (załadowanie zawartości do kolejki)
- [ ] Wybór konkretnego utworu z listy
- [ ] Ekran "Now Playing":
  - ikona play/pauza (animowana zmiana ikony, nie tylko statyczny glif)
  - aktywny tryb (shuffle/repeat) jako mała ikona w rogu
  - **animowany "equalizer"/wizualizacja** — proste pulsujące słupki generowane cyklicznie (nie muszą być realnym FFT audio, wystarczy pseudolosowa/pulsująca animacja zsynchronizowana z odtwarzaniem, żeby ekran żył)
- [ ] Wyświetlanie nazwy utworu z efektem **marquee** — płynne przewijanie długich tytułów w pętli (przesunięcie x o 1 px na klatkę, zawijanie na koniec stringa)
- [ ] Pasek postępu (np. `01:23 / 03:45`) — animowane, płynne przesuwanie wypełnienia paska, nie skokowe
- [ ] Wskaźnik głośności (pasek / `Vol: 80%`) — krótka animacja "wjazdu" wartości przy zmianie (np. overlay znikający po 1–2 s)
- [ ] Wskaźnik baterii — animacja migania ikony przy niskim poziomie (low-battery blink)
- [ ] Status Wi-Fi — animowana ikona (np. pulsujące fale) gdy ktoś jest aktualnie połączony z AP
- [ ] Przejścia między ekranami (np. proste slide/wipe w poziomie) przy zmianie `SCREEN_*`, zamiast twardego przeskoku

### Uwagi implementacyjne dot. animacji

- Wszystkie animacje bazują na **czasie** (`millis()`), nie na liczbie klatek pętli — inaczej tempo animacji zależy od obciążenia CPU.
- Rysowanie klatki OLED nie może odbywać się w callbacku audio ani blokować karmienia dekodera MP3 — trzymać UI po stronie Core 0 / osobnego taska z niskim priorytetem.
- Dla marquee: licznik przesunięcia resetować dopiero po pełnym przewinięciu + krótkiej pauzie na początku/końcu (czytelniej niż ciągły "karuzelowy" ruch bez przerwy).

## 6. Web GUI (Wi-Fi AP + serwer HTTP)

- [ ] Zdalny pilot: Play, Pause, Next, Prev, Volume, slider postępu
- [ ] File Explorer — listowanie zawartości karty SD
- [ ] Upload plików z telefonu/komputera na SD
- [ ] Download plików z urządzenia
- [ ] Tworzenie folderów przez WWW
- [ ] Usuwanie plików i folderów
- [ ] Przełącznik Wi-Fi (uśpienie modułu dla oszczędności baterii)
- [ ] Panel konfiguracji: zmiana SSID i hasła AP

## 7. Zasilanie i system

- [ ] Fizyczny włącznik/wyłącznik (odcięcie zasilania)
- [ ] Pomiar napięcia baterii przez ADC (ciągły monitoring w tle)
- [ ] Ostrzeżenie o niskim poziomie baterii (komunikat "Battery Low!" i/lub sygnał dźwiękowy)
- [ ] Automatyczne bezpieczne wyłączenie / Deep Sleep przy napięciu < 3.2V
- [ ] Sygnalizacja LED stanu ładowania (w trakcie / zakończone)
- [ ] **Zapamiętywanie stanu (persistence, zapis do flash/NVS):**
  - ostatnia głośność
  - stan trybu shuffle
  - ostatni folder i utwór
  - dokładna pozycja (sekunda) odtwarzania w momencie wyłączenia
- [ ] Auto Sleep — wygaszenie ekranu / czuwanie po X minutach bezczynności (gdy muzyka nie gra)

## 8. Audio Hardware

- [ ] Automatyczne przełączanie wyjść: odcięcie głośnika wbudowanego po wykryciu wpięcia słuchawek do Jacka
- [ ] Anti-Pop: wyciszenie linii audio programowo/sprzętowo podczas startu i resetu (uniknięcie "strzału")
- [ ] Cyfrowa regulacja głośności: w rejestrach PCM5102A lub programowo przed wysłaniem na I2S

## 9. Obsługa sytuacji awaryjnych

- [ ] Brak karty SD → komunikat "No SD Card" + blokada funkcji odtwarzacza
- [ ] Uszkodzony/niekompatybilny plik → pominięcie pliku zamiast zawieszenia systemu
- [ ] Odłączenie karty SD "w locie" → bezpieczne przerwanie operacji zapisu/odczytu

---

## 10. Priorytety implementacji (sugerowana kolejność dla Codexa)

1. **Rozwiązać konflikt pinów I2S/I2C** (patrz sekcja 0) zanim dołoży się OLED do działającego `main.cpp`
2. Wziąć działający `main.cpp` jako bazę audio i przenieść feed-loop dekodera do taska na Core 1 (`xTaskCreatePinnedToCore`)
3. Dodać U8g2 i ekran OLED: najpierw statyczny "Now Playing" (bez animacji), potwierdzić że nie zakłóca audio
4. Dołożyć animacje UI (marquee, equalizer, przejścia ekranów) — patrz sekcja 5
5. Rozbudować sterowanie: Pause/Stop/Prev/Fast-Forward/Rewind/Volume/Mute (bazując na istniejącym Next)
6. File browser na OLED (nawigacja po katalogach SD zamiast sztywnego `/muzyka`)
7. Persistence (NVS): zapis/odczyt stanu przy starcie i wyłączeniu
8. Tryby odtwarzania: shuffle/repeat
9. Zarządzanie zasilaniem: pomiar napięcia, low-battery, deep sleep, anti-pop
10. Wi-Fi AP + serwer WWW: file explorer, upload/download, zdalny pilot
11. Obsługa błędów i przypadków brzegowych (brak SD, uszkodzone pliki, hot-unplug)

---

## Uwagi

- Priorytet czasowy ma dekodowanie audio (Core 1) — Wi-Fi i operacje SD (Core 0) nie mogą powodować przerw/trzasków w odtwarzaniu.
- Wszystkie zmiany stanu (głośność, shuffle, pozycja) powinny być zapisywane do NVS asynchronicznie, żeby nie blokować odtwarzania.
