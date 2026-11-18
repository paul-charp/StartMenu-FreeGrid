#pragma once

#include <windows.h>

#include <string>

// Shared between main.cpp (commands) and ui.cpp (window).

std::wstring SelfPath();
bool WatcherRunning();
bool ActiveRunning();
bool RunValueSet();

constexpr wchar_t kWindowClass[] = L"StartGridWindow";

bool DoEnable(std::wstring& err);
void DoDisable(bool closeWindow);
void DoReset();

int RunWindow();
