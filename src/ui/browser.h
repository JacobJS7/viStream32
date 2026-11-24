#pragma once
// Logika przeglądarki plików na OLED (stan w state.h: currentDirectory, directoryEntries...).
#include <Arduino.h>

void refreshBrowser();
void enterBrowser();
void moveBrowserSelection(int direction);
void selectBrowserEntry();
