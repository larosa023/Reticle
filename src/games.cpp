// Reticle - which window is a game
#include "games.h"
#include "util.h"
#include <shellapi.h>
#include <algorithm>

namespace games {

static const wchar_t* kBlock[] = {
    // Windows shell
    L"explorer.exe", L"dwm.exe", L"searchhost.exe", L"searchapp.exe", L"startmenuexperiencehost.exe", L"shellexperiencehost.exe",
    L"applicationframehost.exe", L"textinputhost.exe", L"lockapp.exe", L"systemsettings.exe", L"taskmgr.exe", L"mmc.exe",
    L"rundll32.exe", L"dllhost.exe", L"widgets.exe", L"windowsterminal.exe", L"wt.exe", L"cmd.exe", L"powershell.exe", L"pwsh.exe",
    L"conhost.exe", L"notepad.exe", L"mspaint.exe", L"photos.exe", L"microsoft.photos.exe", L"snippingtool.exe", L"screenclippinghost.exe",
    // browsers
    L"chrome.exe", L"msedge.exe", L"firefox.exe", L"brave.exe", L"opera.exe", L"opera_gx.exe", L"vivaldi.exe", L"arc.exe", L"librewolf.exe",
    L"waterfox.exe", L"zen.exe", L"thorium.exe", L"msedgewebview2.exe",
    // launchers and stores
    L"steam.exe", L"steamwebhelper.exe", L"epicgameslauncher.exe", L"battle.net.exe", L"eadesktop.exe", L"origin.exe", L"galaxyclient.exe",
    L"ubisoftconnect.exe", L"upc.exe", L"riotclientservices.exe", L"riotclientux.exe", L"leagueclientux.exe", L"xboxpcapp.exe",
    L"gamingservices.exe", L"playnite.desktopapp.exe", L"playnite.fullscreenapp.exe", L"heroic.exe", L"rockstarlauncher.exe",
    L"amazongames.exe", L"itch.exe", L"bsglauncher.exe",
    // chat, media, work
    L"discord.exe", L"discordptb.exe", L"discordcanary.exe", L"vesktop.exe", L"spotify.exe", L"teams.exe", L"ms-teams.exe", L"slack.exe",
    L"zoom.exe", L"outlook.exe", L"olk.exe", L"winword.exe", L"excel.exe", L"powerpnt.exe", L"onenote.exe", L"code.exe", L"cursor.exe",
    L"devenv.exe", L"rider64.exe", L"idea64.exe", L"notepad++.exe", L"obs64.exe", L"obs32.exe", L"vlc.exe", L"mpc-hc64.exe", L"mpc-be64.exe",
    L"mpv.exe", L"potplayermini64.exe", L"wmplayer.exe", L"music.ui.exe", L"video.ui.exe", L"claude.exe", L"chatgpt.exe",
    L"blender.exe", L"photoshop.exe", L"afterfx.exe", L"premiere.exe", L"resolve.exe", L"figma.exe", L"whatsapp.exe", L"telegram.exe",
    // overlays, tools, wallpapers
    L"msiafterburner.exe", L"rtss.exe", L"nvcontainer.exe", L"nvidia app.exe", L"nvidia overlay.exe", L"wallpaper32.exe", L"wallpaper64.exe",
    L"lively.exe", L"livelywpf.exe", L"rainmeter.exe", L"vrserver.exe", L"vrmonitor.exe", L"oculusclient.exe",
    // the larosa023 suite
    L"reticle.exe", L"hangar.exe", L"ember.exe", L"foyer.exe", L"strata.exe", L"breaker.exe", L"switch.exe", L"isle.exe", L"faderly.exe",
    L"tonely.exe", L"marq.exe", L"mantel.exe", L"lumora.exe", L"savor.exe", L"driftcord.exe", L"amplite.exe", L"cinder.exe", L"relic.exe",
    L"prism.exe", L"ripple.exe", L"downtime.exe", L"drift.exe", L"gridline.exe", L"fullcourt.exe", L"rigcard.exe", L"loupe.exe",
};

bool Blocklisted(const std::wstring& exe) {
    for (auto* b : kBlock) if (exe == b) return true;
    return false;
}

bool InGameLibrary(const std::wstring& path) {
    static const wchar_t* kDirs[] = {
        L"\\steamapps\\common\\", L"\\epic games\\", L"\\gog galaxy\\games\\", L"\\gog games\\", L"\\xboxgames\\",
        L"\\ea games\\", L"\\origin games\\", L"\\ubisoft game launcher\\games\\", L"\\riot games\\", L"\\rockstar games\\",
        L"\\amazon games\\library\\", L"\\itch\\apps\\", L"\\battle.net\\games\\", L"\\games\\",
    };
    std::wstring p = Lower(path);
    for (auto* d : kDirs) if (p.find(d) != std::wstring::npos) return true;
    return false;
}

bool ClientRect(HWND hwnd, RECT& out, RECT* monitor, UINT* dpi) {
    RECT rc;
    if (!GetClientRect(hwnd, &rc)) return false;
    POINT tl{rc.left, rc.top}, br{rc.right, rc.bottom};
    ClientToScreen(hwnd, &tl);
    ClientToScreen(hwnd, &br);
    out = {tl.x, tl.y, br.x, br.y};
    if (monitor) {
        MONITORINFO mi{sizeof(mi)};
        if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) *monitor = mi.rcMonitor;
    }
    if (dpi) *dpi = GetDpiForWindow(hwnd) ? GetDpiForWindow(hwnd) : 96;
    return true;
}

bool Describe(HWND hwnd, Window& w) {
    w = Window{};
    if (!hwnd || !IsWindowVisible(hwnd) || IsIconic(hwnd)) return false;
    if (hwnd == GetShellWindow() || hwnd == GetDesktopWindow()) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid || pid == GetCurrentProcessId()) return false;
    w.hwnd = hwnd;
    w.pid = pid;
    if (!ClientRect(hwnd, w.client, &w.monitor, &w.dpi)) return false;
    int cw = w.client.right - w.client.left, ch = w.client.bottom - w.client.top;
    if (cw < 640 || ch < 360) return false;
    int mw = w.monitor.right - w.monitor.left, mh = w.monitor.bottom - w.monitor.top;
    w.coversMonitor = cw >= mw && ch >= mh && w.client.left <= w.monitor.left && w.client.top <= w.monitor.top;
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[MAX_PATH * 2];
        DWORD n = MAX_PATH * 2;
        if (QueryFullProcessImageNameW(p, 0, buf, &n)) w.path.assign(buf, n);
        CloseHandle(p);
    }
    if (w.path.empty()) return false;   // protected system process
    w.exe = Lower(PathName(w.path));
    wchar_t t[256];
    int n = GetWindowTextW(hwnd, t, 256);
    w.title.assign(t, n > 0 ? n : 0);
    return true;
}

std::wstring FriendlyName(const Window& w) {
    std::wstring company, product, desc;
    ExeStrings(w.path, company, product, desc);
    auto ok = [](const std::wstring& s) {
        // engine boilerplate isn't a game name
        return !s.empty() && !IContains(s, L"Unreal Engine") && !IContains(s, L"BootstrapPackagedGame") && !IEquals(s, L"Unity") &&
               !IContains(s, L"Launcher");
    };
    if (ok(product)) return Trim(product);
    if (ok(desc)) return Trim(desc);
    if (!w.title.empty() && w.title.size() < 60) return w.title;
    std::wstring n = PathName(w.path);
    size_t dot = n.rfind(L'.');
    return dot == std::wstring::npos ? n : n.substr(0, dot);
}

bool ProcessAlive(DWORD pid) {
    if (!pid) return false;
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return GetLastError() == ERROR_ACCESS_DENIED;   // exists, just protected
    bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
}

bool ExclusiveFullscreen() {
    QUERY_USER_NOTIFICATION_STATE st;
    return SUCCEEDED(SHQueryUserNotificationState(&st)) && st == QUNS_RUNNING_D3D_FULL_SCREEN;
}

} // namespace games
