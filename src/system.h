// Reticle - permissions and startup.
//
// Frame capture needs a Windows trace session, which only administrators and members of the built-in
// "Performance Log Users" group may start. Reticle offers both ways:
//   * Restart as administrator (this session only), or
//   * a one-time grant: add your account to Performance Log Users (one UAC prompt, then sign out and
//     back in). After that Reticle captures frames without admin rights.
// CPU temperature always needs admin (the PawnIO driver only talks to elevated programs).
#pragma once
#include "common.h"

namespace sys {

bool IsElevated();
bool InPerfLogUsers();                     // the group is active in this sign-in
bool RestartElevated(const std::wstring& args);   // false if the UAC prompt was declined
bool GrantPerfLogUsers();                  // runs an elevated copy of Reticle with --grant-perflog
int HandleGrantPerfLog(const wchar_t* sid); // the elevated side; returns an exit code
bool PerfLogGrantedPendingSignIn();        // granted, but this sign-in doesn't have it yet

// Start with Windows: a Run entry, or a logon task with highest privileges when "as admin" is on
bool StartupEnabled();
bool StartupIsAdmin();                     // cached; RefreshStartupState() re-reads it (spawns schtasks)
void RefreshStartupState();
bool SetStartup(bool on, bool admin, std::wstring* error);

} // namespace sys
