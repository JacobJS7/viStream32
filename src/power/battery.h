#pragma once
// Pomiar napięcia baterii przez ADC (BATTERY_ADC_PIN, dzielnik VOLTAGE_DIVIDER_RATIO).
#include <Arduino.h>

void batteryBegin();
void updateBatteryVoltage();
// Pomiar co BATTERY_SAMPLE_INTERVAL_MS; wołane z loop().
void batteryPoll(uint32_t now);
