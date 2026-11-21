// Reticle - which window is a game.
//
// Reticle never opens a game's memory: it only asks Windows for the foreground window, its process
// id and the exe path (PROCESS_QUERY_LIMITED_INFORMATION), which anti-cheat allows.
#pragma once
#include "common.h"

namespace games {

struct Window {
    HWND hwnd = nullptr;
    DWORD pid = 0;
    std::wstring exe;          // lower-case file name
    std::wstring path;
    std::wstring title;
    RECT client{};             // screen coordinates (physical pixels)
    RECT monitor{};            // the monitor it's on
    UINT dpi = 96;
    bool coversMonitor = false;
};

bool Describe(HWND hwnd, Window& out);       // false for desktop/shell windows, our own, invisible or tiny ones
bool Blocklisted(const std::wstring& exe);    // browsers, launchers, chat, editors, the shell…
bool InGameLibrary(const std::wstring& path); // steamapps\common, Epic Games, XboxGames…
std::wstring FriendlyName(const Window& w);
bool ProcessAlive(DWORD pid);
bool ClientRect(HWND hwnd, RECT& out, RECT* monitor, UINT* dpi);
bool ExclusiveFullscreen();                   // a D3D app owns the screen (the overlay can't show)

} // namespace games
