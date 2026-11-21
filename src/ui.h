// Reticle - the Settings window (Direct2D, the suite's look)
#pragma once
#include "common.h"

namespace ui {
void Open(const wchar_t* page);        // create or bring to front; page: "home", "overlay", "crosshair", "games", "benchmarks", "shortcuts", "themes", "settings"
void Close();
bool IsOpen();
HWND Hwnd();
void Invalidate();                     // something it shows changed
void OnBenchmarksChanged();
const std::wstring& EditScope();       // "" = the default for all games, else a game's exe
}
