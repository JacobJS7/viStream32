#include "input/buttons.h"
#include "config.h"
#include "state.h"
#include "audio/player.h"
#include "ui/browser.h"
#include "ui/display.h"

static volatile bool nextPressed = false;
static volatile bool prevPressed = false;
static volatile bool playPressed = false;
static volatile bool browserPressed = false;
static volatile bool nextReleased = false;
static volatile bool prevReleased = false;
static volatile bool playReleased = false;
static volatile bool browserReleased = false;
static volatile uint32_t nextPressMillis = 0;
static volatile uint32_t prevPressMillis = 0;
static volatile uint32_t playPressMillis = 0;
static volatile uint32_t browserPressMillis = 0;
static bool playModifierUsed = false;
static bool playClickPending = false;
static uint32_t lastPlayReleaseMillis = 0;
static unsigned long lastButtonMillis = 0;

static void IRAM_ATTR handleNextISR() {
	if (digitalRead(NEXT_BUTTON_PIN) == LOW) {
		nextPressed = true;
		nextPressMillis = millis();
	} else {
		nextReleased = true;
	}
}

static void IRAM_ATTR handlePrevISR() {
	if (digitalRead(PREV_BUTTON_PIN) == LOW) {
		prevPressed = true;
		prevPressMillis = millis();
	} else {
		prevReleased = true;
	}
}

static void IRAM_ATTR handlePlayISR() {
	if (digitalRead(PLAY_BUTTON_PIN) == LOW) {
		playPressed = true;
		playPressMillis = millis();
	} else {
		playReleased = true;
	}
}

static void IRAM_ATTR handleBrowserISR() {
	if (digitalRead(BROWSER_BUTTON_PIN) == LOW) {
		browserPressed = true;
		browserPressMillis = millis();
	} else {
		browserReleased = true;
	}
}

void buttonsInitPins() {
	pinMode(NEXT_BUTTON_PIN, INPUT_PULLUP);
	pinMode(PREV_BUTTON_PIN, INPUT_PULLUP);
	pinMode(PLAY_BUTTON_PIN, INPUT_PULLUP);
	pinMode(WIFI_SWITCH_PIN, INPUT_PULLUP);
	pinMode(BROWSER_BUTTON_PIN, INPUT_PULLUP);
}

void buttonsAttachInterrupts() {
	attachInterrupt(digitalPinToInterrupt(NEXT_BUTTON_PIN), handleNextISR, CHANGE);
	attachInterrupt(digitalPinToInterrupt(PREV_BUTTON_PIN), handlePrevISR, CHANGE);
	attachInterrupt(digitalPinToInterrupt(PLAY_BUTTON_PIN), handlePlayISR, CHANGE);
	attachInterrupt(digitalPinToInterrupt(BROWSER_BUTTON_PIN), handleBrowserISR, CHANGE);
}

void buttonsPoll(unsigned long now) {
	if (browserReleased && browserPressed) {
		browserReleased = false;
		browserPressed = false;
		if (now - lastButtonMillis > 300) {
			lastButtonMillis = now;
			if (currentScreen == SCREEN_BROWSER) {
				setScreen(SCREEN_NOWPLAYING);
			} else {
				enterBrowser();
			}
		}
	}

	if (nextReleased && nextPressed) {
		nextReleased = false;
		nextPressed = false;
		if (now - lastButtonMillis > 300) {
			lastButtonMillis = now;
			if (playPressed && now - nextPressMillis < LONG_PRESS_MS) {
				volumeUp();
				playModifierUsed = true;
			} else if (currentScreen == SCREEN_BROWSER) {
				moveBrowserSelection(1);
			} else if (now - nextPressMillis >= LONG_PRESS_MS) {
				seekBySeconds(SEEK_STEP_SECONDS);
			} else {
				Serial.println("Przycisk NEXT: pomijanie utworu");
				closeTrack();
				advanceTrack();
			}
		}
	}

	if (prevReleased && prevPressed) {
		prevReleased = false;
		prevPressed = false;
		if (now - lastButtonMillis > 300) {
			lastButtonMillis = now;
			if (playPressed && now - prevPressMillis < LONG_PRESS_MS) {
				volumeDown();
				playModifierUsed = true;
			} else if (currentScreen == SCREEN_BROWSER) {
				moveBrowserSelection(-1);
			} else if (now - prevPressMillis >= LONG_PRESS_MS) {
				seekBySeconds(-SEEK_STEP_SECONDS);
			} else {
				Serial.println("Przycisk PREV: poprzedni utwór");
				closeTrack();
				retreatTrack();
			}
		}
	}

	if (playReleased && playPressed) {
		playReleased = false;
		playPressed = false;
		if (playModifierUsed) {
			playModifierUsed = false;
		} else if (now - lastButtonMillis > 300) {
			lastButtonMillis = now;
			if (now - playPressMillis >= LONG_PRESS_MS) {
				Serial.println("Długie PLAY: stop");
				playClickPending = false;
				if (currentScreen == SCREEN_BROWSER) {
					setScreen(SCREEN_NOWPLAYING);
				} else {
					stopPlayback();
				}
			} else if (playClickPending && now - lastPlayReleaseMillis <= PLAY_DOUBLE_CLICK_MS) {
				Serial.println("Podwójny PLAY: zmiana trybu");
				playClickPending = false;
				cyclePlaybackMode();
			} else {
				playClickPending = true;
				lastPlayReleaseMillis = now;
			}
		}
	}

	if (playClickPending && now - lastPlayReleaseMillis > PLAY_DOUBLE_CLICK_MS) {
		playClickPending = false;
		if (currentScreen == SCREEN_BROWSER) {
			selectBrowserEntry();
		} else {
			togglePlayPause();
		}
	}
}
