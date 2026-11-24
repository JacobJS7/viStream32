#include "power/battery.h"
#include "config.h"
#include "state.h"

static uint32_t lastBatterySampleMillis = 0;

void batteryBegin() {
	analogReadResolution(12);
	analogSetAttenuation(ADC_11db);
}

void updateBatteryVoltage() {
	uint32_t rawTotal = 0;
	for (uint8_t sample = 0; sample < BATTERY_ADC_SAMPLES; ++sample) {
		rawTotal += analogRead(BATTERY_ADC_PIN);
	}
	float rawAverage = static_cast<float>(rawTotal) / BATTERY_ADC_SAMPLES;
	batteryVoltage = (rawAverage / 4095.0f) * 3.3f * VOLTAGE_DIVIDER_RATIO;
	Serial.print("batteryVoltage: ");
	Serial.println(batteryVoltage, 3);
}

void batteryPoll(uint32_t now) {
	if (now - lastBatterySampleMillis >= BATTERY_SAMPLE_INTERVAL_MS) {
		lastBatterySampleMillis = now;
		updateBatteryVoltage();
	}
}
