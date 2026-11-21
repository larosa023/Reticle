// Reticle - a lightweight in-game performance overlay and crosshair for Windows
//
//   Reticle.exe                 start (or open Settings in the running copy)
//   Reticle.exe --tray          start quietly in the tray (used at sign-in)
//   Reticle.exe --quit          close the running copy (used by Hangar)
//   Reticle.exe --version       exit 0 (smoke tests)
#include "common.h"
#include "app.h"
#include "config.h"
#include "net.h"
#include "system.h"
#include "ui.h"
#include "util.h"
#include <objbase.h>
#include <shellapi.h>
#include <shobjidl.h>

static HANDLE g_mutex;
static const wchar_t* kMutexName = L"Local\\Reticle.larosa023.SingleInstance";

void ReleaseInstanceMutex() {
    if (g_mutex) { ReleaseMutex(g_mutex); CloseHandle(g_mutex); g_mutex = nullptr; }
}
void ReacquireInstanceMutex() {
    if (!g_mutex) g_mutex = CreateMutexW(nullptr, TRUE, kMutexName);
}

static bool HasArg(const wchar_t* cmd, const wchar_t* arg) { return wcsstr(cmd, arg) != nullptr; }

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    if (HasArg(cmdLine, L"--version")) return 0;
    if (const wchar_t* g = wcsstr(cmdLine, L"--grant-perflog ")) return sys::HandleGrantPerfLog(g + 16);

    if (HasArg(cmdLine, L"--quit")) {
        if (HWND w = FindWindowW(RETICLE_MAIN_CLASS, nullptr)) PostMessageW(w, WM_RETICLE_QUIT, 0, 0);
        return 0;
    }

    updater::AfterUpdate after = updater::ParseArgs(GetCommandLineW());
    bool handover = after.active || HasArg(cmdLine, L"--elevated");
    g_mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS && handover) {
        // the copy we're replacing (an update, or the non-admin copy that restarted us) is letting go
        CloseHandle(g_mutex);
        g_mutex = nullptr;
        for (int i = 0; i < 80 && !g_mutex; ++i) {
            Sleep(100);
            HANDLE m = CreateMutexW(nullptr, TRUE, kMutexName);
            if (GetLastError() != ERROR_ALREADY_EXISTS) g_mutex = m; else CloseHandle(m);
        }
        if (!g_mutex) return 0;
    } else if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!HasArg(cmdLine, L"--tray")) {
            if (HWND existing = FindWindowW(RETICLE_MAIN_CLASS, nullptr)) {
                DWORD pid = 0;
                GetWindowThreadProcessId(existing, &pid);
                AllowSetForegroundWindow(pid);
                PostMessageW(existing, WM_RETICLE_ACTIVATE, 0, 0);
            }
        }
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    SetCurrentProcessExplicitAppUserModelID(L"larosa023.Reticle");
    LogInit();
    Log(L"==== Reticle %ls starting (data: %ls%ls%ls)", RETICLE_VERSION, DataDir().c_str(), IsPortable() ? L", portable" : L"",
        sys::IsElevated() ? L", admin" : L"");
    ConfigLoad();
    net::Init();

    bool openSettings = !HasArg(cmdLine, L"--tray") && !after.active;
    if (!app::Create(inst, openSettings, after)) {
        MessageBoxW(nullptr, L"Reticle couldn't start. Details are in reticle.log.", L"Reticle", MB_ICONERROR);
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // Ctrl+, opens Settings from anywhere in Reticle's window
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_OEM_COMMA && (GetKeyState(VK_CONTROL) & 0x8000) && ui::IsOpen() &&
            (msg.hwnd == ui::Hwnd() || IsChild(ui::Hwnd(), msg.hwnd))) {
            ui::Open(L"settings");
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    ConfigFlush();
    net::Shutdown();
    Log(L"Reticle exiting");
    CoUninitialize();
    if (g_mutex) CloseHandle(g_mutex);
    return 0;
}
