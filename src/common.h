// Reticle - a lightweight in-game performance overlay and crosshair for Windows (larosa023 suite)
#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

#ifndef RETICLE_VERSION
#define RETICLE_VERSION L"0.1.0"
#endif
#define RETICLE_REPO_OWNER L"larosa023"
#define RETICLE_REPO_NAME L"Reticle"
#define RETICLE_MAIN_CLASS L"Reticle.larosa023.Main"

// Messages posted to the main (settings/controller) window. lParam owns a heap object when noted.
enum : UINT {
    WM_RETICLE_TRAY = WM_APP + 1,      // tray icon callback
    WM_RETICLE_ACTIVATE,               // another Reticle.exe asked us to show Settings
    WM_RETICLE_SELF,                   // lParam: SelfUpdateState*
    WM_RETICLE_FOREGROUND,             // the foreground window changed (from the WinEvent hook)
    WM_RETICLE_QUIT,                   // Reticle.exe --quit
};
