#include "ui/browser.h"
#include <vector>
#include "state.h"
#include "storage/sd_fs.h"
#include "audio/player.h"
#include "ui/display.h"

void refreshBrowser() {
	String directory = currentDirectory;
	std::vector<DirectoryEntry> entries;
	if (!scanDirectory(directory.c_str(), entries)) {
		Serial.print("Nie można otworzyć katalogu: ");
		Serial.println(directory);
		directory = parentPath(directory);
		scanDirectory(directory.c_str(), entries);
	}

	xSemaphoreTake(stateMutex, portMAX_DELAY);
	currentDirectory = directory;
	directoryEntries = entries;
	if (directoryEntries.empty()) {
		browserSelection = 0;
		browserOffset = 0;
	} else {
		if (browserSelection >= directoryEntries.size()) {
			browserSelection = directoryEntries.size() - 1;
		}
		if (browserSelection < browserOffset) browserOffset = browserSelection;
		if (browserSelection >= browserOffset + 4) browserOffset = browserSelection - 3;
	}
	xSemaphoreGive(stateMutex);
}

void enterBrowser() {
	// Po załadowaniu nowej playlisty przeglądarka otwiera się w jej folderze;
	// w przeciwnym razie zostaje tam, gdzie użytkownik ją zostawił.
	static String lastSyncedPlaylist;
	xSemaphoreTake(stateMutex, portMAX_DELAY);
	if (playlistDirectory.length() > 0 && playlistDirectory != lastSyncedPlaylist) {
		lastSyncedPlaylist = playlistDirectory;
		currentDirectory = playlistDirectory;
		browserSelection = 0;
		browserOffset = 0;
	}
	xSemaphoreGive(stateMutex);
	setScreen(SCREEN_BROWSER);
	refreshBrowser();
}

void moveBrowserSelection(int direction) {
	if (directoryEntries.empty()) return;
	if (direction > 0) {
		browserSelection = (browserSelection + 1) % directoryEntries.size();
	} else {
		browserSelection = browserSelection == 0 ? directoryEntries.size() - 1 : browserSelection - 1;
	}
	if (browserSelection < browserOffset) browserOffset = browserSelection;
	if (browserSelection >= browserOffset + 4) browserOffset = browserSelection - 3;
}

void selectBrowserEntry() {
	if (directoryEntries.empty()) return;
	DirectoryEntry selected = directoryEntries[browserSelection];
	if (selected.isParent) {
		currentDirectory = parentPath(currentDirectory);
		browserSelection = 0;
		browserOffset = 0;
		refreshBrowser();
		return;
	}

	String selectedPath = joinPath(currentDirectory, selected.name);
	if (selected.isDirectory) {
		currentDirectory = selectedPath;
		browserSelection = 0;
		browserOffset = 0;
		refreshBrowser();
		return;
	}

	playFileFromPath(joinPath(currentDirectory, selected.name));
}
