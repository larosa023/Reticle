// Reticle - shared state of the running app (owned by app.cpp, read by the overlay and Settings)
#pragma once
#include "common.h"
#include "games.h"
#include "frames.h"
#include "sensors.h"
#include "updater.h"
#include "bench.h"
#include "gfx.h"

struct AppState {
    HINSTANCE inst = nullptr;
    HWND ctl = nullptr;                 // hidden controller window (tray, hotkeys, timers)
    // the game
    bool gameForeground = false;        // a game is the foreground window right now
    games::Window game;                 // last detected game window
    std::wstring gameName;
    DWORD gamePid = 0;
    ULONGLONG sessionStart = 0;         // GetTickCount64 when this game session began
    bool exclusiveFullscreen = false;
    // readings
    frames::Live live;
    sensors::Reading sens;
    // benchmark
    bool benching = false;
    std::wstring benchExe, benchGame;
    double cpuSum = 0, gpuSum = 0; int cpuN = 0, gpuN = 0; float cpuTempMax = -1, gpuTempMax = -1;
    // updates
    SelfUpdateState upd;
    bool updateDeferredForGame = false;
    // theme accent for the overlay
    D2D1_COLOR_F accent = gfx::Rgb(0x7AA2FF);
    std::vector<std::wstring> hotkeyErrors;   // shortcuts another app already uses
};
extern AppState g_app;

namespace app {
HWND Create(HINSTANCE inst, bool openSettings, const updater::AfterUpdate& after, bool handedOver);
void FixPermissions(bool userAsked);    // one UAC prompt: admin startup task + Performance Log Users, then switch to the admin copy
void OpenSettings(const wchar_t* page = nullptr);
void ApplyHotkeys();                    // re-register after Settings changes them
void ThemeChanged();                    // recompute the overlay accent
void OverlayChanged();                  // style/stats changed: redraw now
void CrosshairChanged();
void StartBenchmark();
void StopBenchmark(bool save);
void CheckUpdates(bool force);
void InstallUpdateNow();
void RefreshCapture();                  // try to (re)start frame capture
void PreviewOverlay(bool on);           // show the overlay over the desktop while Settings edits it
bool PreviewingOverlay();
void BeginMoveOverlay();
std::wstring CurrentGameExe();          // "" when no game is running
bool GameRunning();
void Quit();
}
