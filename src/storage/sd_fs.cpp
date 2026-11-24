#include "storage/sd_fs.h"
#include <SD.h>
#include "state.h"

String joinPath(const String &base, const String &name) {
	if (base == "/") return "/" + name;
	return base + "/" + name;
}

String parentPath(const String &path) {
	if (path == "/") return "/";
	int separator = path.lastIndexOf('/');
	if (separator <= 0) return "/";
	return path.substring(0, separator);
}

bool scanDirectory(const char *path, std::vector<DirectoryEntry> &scannedEntries) {
	scannedEntries.clear();
	File dir;

	xSemaphoreTake(sdMutex, portMAX_DELAY);
	dir = SD.open(path);
	if (!dir || !dir.isDirectory()) {
		xSemaphoreGive(sdMutex);
		return false;
	}

	if (String(path) != "/") {
		scannedEntries.push_back({"..", true, true, 0});
	}

	File entry = dir.openNextFile();
	while (entry) {
		String name = entry.name();
		int separator = name.lastIndexOf('/');
		if (separator >= 0) name = name.substring(separator + 1);
		if (entry.isDirectory() || name.endsWith(".mp3") || name.endsWith(".MP3")) {
			scannedEntries.push_back({name, entry.isDirectory(), false, entry.isDirectory() ? 0 : static_cast<uint32_t>(entry.size())});
		}
		entry.close();
		entry = dir.openNextFile();
	}
	dir.close();
	xSemaphoreGive(sdMutex);
	return true;
}

// Usuwa plik lub katalog wraz z zawartością. sdMutex brany per operacja,
// żeby długie kasowanie nie zagłodziło audioFeedTask.
bool removePathRecursive(const String &path, uint8_t depth) {
	if (depth > 8) return false;

	xSemaphoreTake(sdMutex, portMAX_DELAY);
	File node = SD.open(path.c_str());
	if (!node) {
		xSemaphoreGive(sdMutex);
		return false;
	}
	bool isDirectory = node.isDirectory();
	std::vector<String> children;
	if (isDirectory) {
		File child = node.openNextFile();
		while (child) {
			String name = child.name();
			int separator = name.lastIndexOf('/');
			if (separator >= 0) name = name.substring(separator + 1);
			children.push_back(name);
			child.close();
			child = node.openNextFile();
		}
	}
	node.close();
	if (!isDirectory) {
		bool removed = SD.remove(path.c_str());
		xSemaphoreGive(sdMutex);
		return removed;
	}
	xSemaphoreGive(sdMutex);

	for (const String &name : children) {
		if (!removePathRecursive(joinPath(path, name), depth + 1)) return false;
		taskYIELD();
	}

	xSemaphoreTake(sdMutex, portMAX_DELAY);
	bool removed = SD.rmdir(path.c_str());
	xSemaphoreGive(sdMutex);
	return removed;
}
