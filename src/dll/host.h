#pragma once

#include <windows.h>

#include <string>

#include "../common/shared.h"

// Shim for what the Windhawk API gave the mod.

void Log(const wchar_t* fmt, ...);

// [grid] key from settings.ini, 0 if missing.
int GetGridInt(const wchar_t* key);

// cells.txt as text, empty if missing; written atomically.
std::wstring ReadCells();
void WriteCells(const std::wstring& text);

// Calls onChange when settings.ini's last-write time changes.
void StartSettingsWatcher(void (*onChange)());
void StopSettingsWatcher();
