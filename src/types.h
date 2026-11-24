#pragma once
#include <Arduino.h>


enum RepeatMode : uint8_t {
	REPEAT_OFF,
	REPEAT_ONE,
	REPEAT_ALL
};

enum Screen : uint8_t {
	SCREEN_BROWSER,
	SCREEN_NOWPLAYING
};

struct DirectoryEntry {
	String name;
	bool isDirectory;
	bool isParent;
	uint32_t size;
};
