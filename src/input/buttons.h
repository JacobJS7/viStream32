#pragma once
// Przyciski NEXT/PREV/PLAY/BROWSER (INPUT_PULLUP, aktywne LOW, przerwania CHANGE).
#include <Arduino.h>

// pinMode dla przycisków i przełącznika Wi-Fi; musi być przed startem wifiTask.
void buttonsInitPins();
void buttonsAttachInterrupts();
// Debounce, długie/podwójne naciśnięcia i akcje; wołane z loop().
void buttonsPoll(unsigned long now);
