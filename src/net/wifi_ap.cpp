#include "net/wifi_ap.h"
#include <WiFi.h>
#include "config.h"
#include "state.h"
#include "net/web_server.h"

void applyWifiHardwareState(bool enabled) {
	if (enabled == wifiApEnabled) return;
	wifiApEnabled = enabled;
	if (wifiApEnabled) {
		WiFi.mode(WIFI_AP);
		WiFi.softAP(configuredSsid.c_str(), configuredPassword.c_str());
		Serial.print("Wi-Fi AP: ");
		Serial.println(configuredSsid);
		Serial.print("AP IP: ");
		Serial.println(WiFi.softAPIP());
		webServerStart();
	} else {
		webServerStop();
		WiFi.softAPdisconnect(true);
		WiFi.mode(WIFI_OFF);
		Serial.println("Wi-Fi AP OFF");
	}
}

bool isWifiClientConnected() {
	return wifiApEnabled && WiFi.softAPgetStationNum() > 0;
}

void wifiTask(void *parameter) {
	(void)parameter;
	registerWebRoutes();
	wifiApEnabled = false;
	applyWifiHardwareState(digitalRead(WIFI_SWITCH_PIN) == LOW);
	uint32_t lastSwitchPoll = 0;
	for (;;) {
		// Przełącznik Wi-Fi obsługiwany tylko tutaj (ten sam task co WebServer).
		uint32_t now = millis();
		if (now - lastSwitchPoll >= WIFI_SWITCH_POLL_MS) {
			lastSwitchPoll = now;
			bool switchState = digitalRead(WIFI_SWITCH_PIN) == LOW;
			if (switchState != wifiApEnabled) {
				applyWifiHardwareState(switchState);
			}
		}
		if (wifiApEnabled) webServerHandle();
		yield();
		vTaskDelay(pdMS_TO_TICKS(2));
	}
}
