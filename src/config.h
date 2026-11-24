#pragma once
// Konfiguracja sprzętu i stałe strojenia. Jedyne miejsce na numery pinów.

// I2S DOUT uses GPIO 27; I2C remains on SDA 21 and SCL 22.
#define I2S_BCLK  26
#define I2S_LRC   25
#define I2S_DOUT  27
#define I2C_SDA   21
#define I2C_SCL   22

#define SD_CS     5
#define BATTERY_ADC_PIN 34
#define BATTERY_ADC_SAMPLES 12
#define VOLTAGE_DIVIDER_RATIO 2.0f
#define BATTERY_SAMPLE_INTERVAL_MS 3000UL
#define NEXT_BUTTON_PIN 4
#define PREV_BUTTON_PIN 33
#define PLAY_BUTTON_PIN 32
#define WIFI_SWITCH_PIN 16
#define BROWSER_BUTTON_PIN 17
#define LONG_PRESS_MS 800
#define PLAY_DOUBLE_CLICK_MS 400
#define WIFI_SWITCH_POLL_MS 250UL
#define SEEK_STEP_SECONDS 10
#define STATE_SAVE_INTERVAL_MS 12000UL
#define APPROXIMATE_MP3_BYTES_PER_SECOND 16000UL  // fallback (128 kbps), gdy nagłówek MP3 nieczytelny
#define UI_ANIMATIONS 1
#define UI_FRAME_INTERVAL_MS 40UL
#define UI_TRANSITION_MS 240UL
#define UI_VOLUME_OVERLAY_MS 1500UL
#define BATTERY_WARNING_VOLTAGE 3.4f
#define DEFAULT_GAIN 0.005f
#define MAX_GAIN 0.05f
#define GAIN_STEP 0.0025f
#define WIFI_AP_SSID "vistream32"
#define WIFI_AP_PASSWORD "vistream1234"


// Kalibracja procentu baterii do wyświetlania (dopasuj pod realne napięcia)
constexpr float BATTERY_DISPLAY_FULL_V  = 4.2f;
constexpr float BATTERY_DISPLAY_EMPTY_V = 3.2f;
