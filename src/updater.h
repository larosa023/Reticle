// Reticle - built-in updater, the suite's single-exe model (same as Hangar's own updater):
//   1. a background job finds the newest release of larosa023/Reticle on the chosen channel
//      (Stable, or Beta = pre-releases and versions with a "-"), downloads Reticle.exe and
//      verifies its SHA-256 against GitHub's digest and SHA256SUMS.txt (both must agree)
//   2. only when no game is running, Reticle renames itself to Reticle.exe.old, puts the new exe in
//      its place and starts it with --after-update
//   3. the new copy signals "healthy" once it's running; the old one exits. If that doesn't happen
//      within 20 seconds (or the new copy quits), the old one puts itself back and skips that version.
#pragma once
#include "common.h"

struct SelfUpdateState {
    enum Kind { Idle, Checking, UpToDate, Available, Downloading, Ready, Error } kind = Idle;
    std::wstring version;
    std::wstring notes;
    std::wstring staged;        // verified exe on disk (Ready)
    std::wstring message;       // Error text
    int percent = -1;
};

namespace updater {

// Background check; posts WM_RETICLE_SELF. With download=false it stops at Available.
void Check(HWND notify, bool force, bool download);
bool Apply(const std::wstring& exe, const std::wstring& version, std::wstring* error);

struct AfterUpdate {
    bool active = false;
    std::wstring fromVersion;
    std::wstring healthyEvent;
    DWORD oldPid = 0;
};
AfterUpdate ParseArgs(const wchar_t* cmdLine);
void SignalHealthy(const AfterUpdate& a);
void FinishAfterUpdate(const AfterUpdate& a);

} // namespace updater

// main.cpp
void ReleaseInstanceMutex();
void ReacquireInstanceMutex();
