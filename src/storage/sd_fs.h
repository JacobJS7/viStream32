#pragma once
// Operacje na karcie SD i ścieżkach. Każda funkcja sama bierze sdMutex.
#include <Arduino.h>
#include <vector>
#include "types.h"

String joinPath(const String &base, const String &name);
String parentPath(const String &path);
// Wypełnia `scannedEntries` katalogami i plikami .mp3 (z wpisem ".." poza "/").
bool scanDirectory(const char *path, std::vector<DirectoryEntry> &scannedEntries);
// Usuwa plik lub katalog wraz z zawartością.
bool removePathRecursive(const String &path, uint8_t depth = 0);
