#pragma once
// Punkt dostępowy Wi-Fi sterowany przełącznikiem WIFI_SWITCH_PIN (task na Core 0).
#include <Arduino.h>

void applyWifiHardwareState(bool enabled);
bool isWifiClientConnected();
void wifiTask(void *parameter);
