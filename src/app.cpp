// Reticle - the controller: tray icon, shortcuts, game detection, timers, benchmarks and updates.
//
// Idle cost: with no game in front Reticle runs no timers at all. It waits on a WinEvent hook for the
// foreground window to change, and the frame-capture session is stopped 20 seconds after the last
// possible game left the foreground.
#include "app.h"
#include "config.h"
#include "overlay.h"
#include "themes.h"
#include "system.h"
#include "ui.h"
#include "util.h"
#include <shellapi.h>
#include <cmath>
#include <ctime>

AppState g_app;

namespace app {

enum : UINT_PTR {
    TIMER_TICK = 1,       // overlay redraw while a game is in front (refreshMs)
    TIMER_SENSORS,        // hardware readings, 1 s, while a game is in front / benchmarking / previewing
    TIMER_DETECT,         // 500 ms while a possible game is in front and not decided yet
    TIMER_ALIVE,          // 5 s while a game process is running in the background
    TIMER_CAPTURE_IDLE,   // stops frame capture after 20 s with no game
    TIMER_UPDATE,         // update checks (first after 30 s, then every 3 h)
    TIMER_HEALTHY,        // after an update: tell the old copy we're running
    TIMER_INSTALL,        // retries a downloaded update when it couldn't install (game running)
    TIMER_PERMISSION,     // makes sure frame capture is allowed (a few seconds after start)
};

static NOTIFYICONDATAW s_nid{};
static UINT s_taskbarCreated = 0;
static HWINEVENTHOOK s_hook = nullptr, s_hookMin = nullptr;
static updater::AfterUpdate s_after;
static bool s_preview = false;
static DWORD s_detectPid = 0;
static int s_detectHits = 0, s_detectChecks = 0;
static ULONGLONG s_lastPlayTick = 0;
static std::vector<float> s_sampleGraph;
static bool s_handedOver = false;          // started by the admin task / an admin restart
static ULONGLONG s_gameSince = 0, s_lastAltCheck = 0;
static bool s_framesLogged = false, s_noFramesLogged = false;

static void Notify(const std::wstring& title, const std::wstring& text) {
    // never while a game is running
    if (GameRunning()) return;
    NOTIFYICONDATAW n = s_nid;
    n.uFlags = NIF_INFO;
    wcsncpy(n.szInfoTitle, title.c_str(), 63);
    wcsncpy(n.szInfo, text.c_str(), 255);
    n.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

// ---------------------------------------------------------------- tray
static void AddTray() {
    s_nid = NOTIFYICONDATAW{};
    s_nid.cbSize = sizeof(s_nid);
    s_nid.hWnd = g_app.ctl;
    s_nid.uID = 1;
    s_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    s_nid.uCallbackMessage = WM_RETICLE_TRAY;
    s_nid.hIcon = (HICON)LoadImageW(g_app.inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wcscpy(s_nid.szTip, L"Reticle");
    Shell_NotifyIconW(NIM_ADD, &s_nid);
    s_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &s_nid);
}
static void RemoveTray() { Shell_NotifyIconW(NIM_DELETE, &s_nid); }

static void UpdateTip() {
    std::wstring tip = L"Reticle";
    if (g_app.gamePid && !g_app.gameName.empty()) tip += L" · " + g_app.gameName;
    if (g_app.benching) tip += L" · recording";
    wcsncpy(s_nid.szTip, tip.c_str(), 127);
    s_nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    Shell_NotifyIconW(NIM_MODIFY, &s_nid);
}

// ---------------------------------------------------------------- game state
std::wstring CurrentGameExe() { return g_app.gamePid ? g_app.game.exe : L""; }

bool GameRunning() {
    if (g_app.gamePid && games::ProcessAlive(g_app.gamePid)) return true;
    QUERY_USER_NOTIFICATION_STATE st;
    if (SUCCEEDED(SHQueryUserNotificationState(&st)) && (st == QUNS_RUNNING_D3D_FULL_SCREEN || st == QUNS_BUSY)) return true;
    return false;
}

static GameProfile* Profile() { return g_app.gamePid ? FindGame(g_app.game.exe) : nullptr; }

static bool OverlayWanted() {
    GameProfile* p = Profile();
    return g_cfg.overlayOn && p && p->overlay;
}
static bool CrosshairWanted() {
    GameProfile* p = Profile();
    return p && p->crosshair;
}

void ThemeChanged() {
    ThemeDef def = FindTheme(g_cfg.theme, LoadCustomThemes());
    gfx::Theme t = MakeTheme(def, g_cfg.accent);
    // the overlay sits over games: keep its accent bright
    D2D1_COLOR_F a = t.accent;
    if (gfx::Luma(a) < 0.35f) a = gfx::Mix(a, gfx::Rgb(0xFFFFFF), 0.35f);
    g_app.accent = a;
    OverlayChanged();
}

static void FillSample() {
    // sample numbers so the layout can be judged with no game running
    if (s_sampleGraph.empty()) {
        srand(7);
        for (int i = 0; i < 240; ++i) s_sampleGraph.push_back(6.9f + (rand() % 100) / 100.0f - 0.5f + (i % 61 == 0 ? 9.0f : 0.0f));
    }
    g_app.live.valid = true;
    g_app.live.fps = 144;
    g_app.live.frameMs = 6.9;
    g_app.live.low1 = 118;
    g_app.live.low01 = 96;
    g_app.live.recent = s_sampleGraph;
}

static void Render();

void OverlayChanged() {
    if (overlay::Moving()) return;
    if (s_preview) overlay::SetStyle(StyleFor(ui::EditScope()));
    else if (g_app.gamePid) overlay::SetStyle(StyleFor(g_app.game.exe));
    Render();
}

void CrosshairChanged() {
    overlay::CrosshairChanged();
    Render();
}

static void PreviewRect(RECT& r, UINT& dpi) {
    // over the game if one is running, else the monitor Settings is on
    if (g_app.gamePid && games::ProcessAlive(g_app.gamePid) && IsWindow(g_app.game.hwnd) &&
        games::ClientRect(g_app.game.hwnd, r, nullptr, &dpi)) return;
    HWND w = ui::Hwnd() ? ui::Hwnd() : GetDesktopWindow();
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTOPRIMARY), &mi);
    r = mi.rcWork;
    dpi = ui::Hwnd() ? GetDpiForWindow(ui::Hwnd()) : GetDpiForSystem();
}

// Draw (or hide) the overlay and crosshair for the current state
static void Render() {
    if (s_preview) {
        RECT r; UINT dpi = 96;
        PreviewRect(r, dpi);
        if (!g_app.gameForeground && !(g_app.live.valid && g_app.gamePid)) FillSample();
        overlay::Show(r, dpi);
        return;
    }
    if (!g_app.gameForeground || !IsWindow(g_app.game.hwnd)) {
        overlay::Hide();
        overlay::HideCrosshair();
        return;
    }
    RECT r; UINT dpi = 96;
    if (!games::ClientRect(g_app.game.hwnd, r, nullptr, &dpi)) return;
    bool capture = frames::GetStatus() == frames::Status::Running;
    if (OverlayWanted() && (capture || g_cfg.showWithoutFrames)) overlay::Show(r, dpi);
    else overlay::Hide();
    if (CrosshairWanted()) overlay::ShowCrosshair(r, dpi);
    else overlay::HideCrosshair();
}

static void StopTimers() {
    KillTimer(g_app.ctl, TIMER_TICK);
    if (!g_app.benching && !s_preview && !ui::IsOpen()) KillTimer(g_app.ctl, TIMER_SENSORS);
}

static void StartTimers() {
    SetTimer(g_app.ctl, TIMER_TICK, g_cfg.refreshMs, nullptr);
    SetTimer(g_app.ctl, TIMER_SENSORS, 1000, nullptr);
}

void RefreshCapture() {
    static ULONGLONG lastFail = 0;
    frames::Status st = frames::GetStatus();
    bool failedRecently = (st == frames::Status::NoPermission || st == frames::Status::Failed) && GetTickCount64() - lastFail < 60000;
    if (st != frames::Status::Running && !failedRecently) {
        if (!frames::Start()) lastFail = GetTickCount64();
    }
    KillTimer(g_app.ctl, TIMER_CAPTURE_IDLE);
    ui::Invalidate();
}

static void EndSession() {
    if (!g_app.gamePid) return;
    if (g_app.benching) StopBenchmark(true);
    Log(L"game session ended: %ls", g_app.game.exe.c_str());
    g_app.gamePid = 0;
    g_app.gameForeground = false;
    g_app.sessionStart = 0;
    frames::SetTarget(0);
    KillTimer(g_app.ctl, TIMER_ALIVE);
    overlay::Hide();
    overlay::HideCrosshair();
    ConfigSaveSoon();
    UpdateTip();
    // an update may have been waiting for the game to close
    if (g_app.upd.kind == SelfUpdateState::Ready && g_cfg.autoUpdate) SetTimer(g_app.ctl, TIMER_INSTALL, 15000, nullptr);
    ui::Invalidate();
}

static void BecomeGame(const games::Window& w, const wchar_t* why) {
    RefreshCapture();   // capture stops 20 s after a game loses focus; back in the game it must restart
    s_gameSince = GetTickCount64();
    if (g_app.gamePid != w.pid) {
        if (g_app.gamePid) EndSession();
        g_app.gamePid = w.pid;
        g_app.sessionStart = GetTickCount64();
        s_framesLogged = s_noFramesLogged = false;
        GameProfile& p = EnsureGame(w.exe, games::FriendlyName(w), w.path);
        g_app.gameName = p.name;
        Log(L"game: %ls (%ls, pid %lu, %ls)", p.name.c_str(), w.exe.c_str(), w.pid, why);
        UpdateTip();
    }
    g_app.game = w;
    g_app.gameForeground = true;
    if (GameProfile* p = FindGame(w.exe)) p->lastSeen = (long long)time(nullptr);
    frames::SetTarget(w.pid);
    overlay::SetStyle(StyleFor(w.exe));
    s_lastPlayTick = GetTickCount64();
    KillTimer(g_app.ctl, TIMER_DETECT);
    KillTimer(g_app.ctl, TIMER_CAPTURE_IDLE);
    SetTimer(g_app.ctl, TIMER_ALIVE, 5000, nullptr);
    g_app.exclusiveFullscreen = games::ExclusiveFullscreen();
    StartTimers();
    sensors::Sample(g_app.sens);
    Render();
    ui::Invalidate();
}

static void NotAGame(const wchar_t* why = L"") {
    if (g_app.gameForeground) Log(L"game left the foreground: %ls", why);
    g_app.gameForeground = false;
    KillTimer(g_app.ctl, TIMER_DETECT);
    StopTimers();
    Render();
    // keep capturing for a while (alt-tab back is common); stop when nothing needs it
    if (frames::GetStatus() == frames::Status::Running && !g_app.benching)
        SetTimer(g_app.ctl, TIMER_CAPTURE_IDLE, 20000, nullptr);
}

static void OnForeground() {
    HWND fg = GetForegroundWindow();
    if (!fg) return;
    if (fg == ui::Hwnd() || fg == g_app.ctl) {   // our own Settings: keep the session, hide the overlay
        if (g_app.gameForeground) NotAGame(L"Reticle's own window");
        return;
    }
    games::Window w;
    if (!games::Describe(fg, w)) {
        wchar_t cls[64] = {};
        GetClassNameW(fg, cls, 64);
        NotAGame(cls);
        return;
    }
    // the same game again (alt-tab back)
    if (w.pid == g_app.gamePid) { BecomeGame(w, L"refocused"); return; }
    GameProfile* p = FindGame(w.exe);
    Detect mode = p ? p->detect : Detect::Auto;
    if (mode == Detect::Never || (mode == Detect::Auto && games::Blocklisted(w.exe))) { NotAGame(w.exe.c_str()); return; }
    if (mode == Detect::Always) { RefreshCapture(); BecomeGame(w, L"always"); return; }
    RefreshCapture();
    if (games::InGameLibrary(w.path)) { BecomeGame(w, L"game library folder"); return; }
    // otherwise: is it rendering frames like a game?
    if (g_app.gameForeground) Log(L"game left the foreground: %ls", w.exe.c_str());
    g_app.gameForeground = false;
    overlay::Hide();
    overlay::HideCrosshair();
    s_detectPid = w.pid;
    s_detectHits = s_detectChecks = 0;
    SetTimer(g_app.ctl, TIMER_DETECT, 500, nullptr);
}

static void OnDetect() {
    HWND fg = GetForegroundWindow();
    games::Window w;
    if (!games::Describe(fg, w) || w.pid != s_detectPid) { KillTimer(g_app.ctl, TIMER_DETECT); return; }
    ++s_detectChecks;
    bool capture = frames::GetStatus() == frames::Status::Running;
    double rate = capture ? frames::PresentRate(w.pid) : 0;
    bool known = FindGame(w.exe) != nullptr;
    if (capture) {
        // a fullscreen-sized window drawing 20+ frames a second for 2 seconds, a game we've seen before,
        // or a big window that keeps rendering for 3 seconds
        int cw = w.client.right - w.client.left, ch = w.client.bottom - w.client.top;
        int mw = w.monitor.right - w.monitor.left, mh = w.monitor.bottom - w.monitor.top;
        bool big = mw > 0 && mh > 0 && (double)cw * ch >= 0.6 * (double)mw * mh;
        if (rate >= 20 && (w.coversMonitor || known || big)) ++s_detectHits; else s_detectHits = 0;
        int need = (w.coversMonitor || known) ? 4 : 6;
        if (s_detectHits >= need) { BecomeGame(w, L"rendering"); return; }
    } else if (w.coversMonitor && s_detectChecks >= 2) {
        BecomeGame(w, L"fullscreen window");   // no frame data: borderless fullscreen is the best signal
        return;
    }
    if (s_detectChecks >= 40) KillTimer(g_app.ctl, TIMER_DETECT);   // 20 s: it's not a game
}

static void OnTick() {
    if (!g_app.gameForeground) { KillTimer(g_app.ctl, TIMER_TICK); return; }
    if (!IsWindow(g_app.game.hwnd)) { NotAGame(L"window closed"); return; }
    frames::Snapshot(g_app.live, 240);
    // one line per session in reticle.log, so "no FPS" reports can be diagnosed
    if (g_app.live.valid && !s_framesLogged) {
        s_framesLogged = true;
        Log(L"frames: %ls at %.0f fps (%ls, pid %lu)", g_app.game.exe.c_str(), g_app.live.fps, g_app.live.api, frames::Target());
    } else if (!g_app.live.valid && !s_framesLogged && !s_noFramesLogged && GetTickCount64() - s_gameSince > 6000) {
        s_noFramesLogged = true;
        Log(L"frames: none from %ls after 6 s (capture %d, error %lu, admin %d)", g_app.game.exe.c_str(), (int)frames::GetStatus(), frames::LastError(),
            sys::IsElevated() ? 1 : 0);
    }
    // No frames from the window's process? Some games render from a helper process (same folder).
    ULONGLONG now = GetTickCount64();
    if (!g_app.live.valid && frames::GetStatus() == frames::Status::Running && now - s_gameSince > 3000 && now - s_lastAltCheck > 2000) {
        s_lastAltCheck = now;
        std::wstring dir = Lower(g_app.game.path.substr(0, g_app.game.path.find_last_of(L'\\') + 1));
        DWORD best = 0;
        double bestRate = 10;
        for (auto& pr : frames::ActiveProcesses()) {
            if (pr.second < bestRate || pr.first == frames::Target()) continue;
            std::wstring path;
            if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pr.first)) {
                wchar_t buf[MAX_PATH * 2];
                DWORD n = MAX_PATH * 2;
                if (QueryFullProcessImageNameW(h, 0, buf, &n)) path.assign(buf, n);
                CloseHandle(h);
            }
            if (!dir.empty() && IStartsWith(Lower(path), dir)) { best = pr.first; bestRate = pr.second; }
        }
        if (best) {
            Log(L"frames: %ls renders from pid %lu; following it", g_app.game.exe.c_str(), best);
            frames::SetTarget(best);
        }
    }
    Render();
}

static void OnSensors() {
    sensors::Sample(g_app.sens);
    if (g_app.benching) {
        if (g_app.sens.cpuUse >= 0) { g_app.cpuSum += g_app.sens.cpuUse; ++g_app.cpuN; }
        if (g_app.sens.gpuUse >= 0) { g_app.gpuSum += g_app.sens.gpuUse; ++g_app.gpuN; }
        g_app.cpuTempMax = std::max(g_app.cpuTempMax, g_app.sens.cpuTemp);
        g_app.gpuTempMax = std::max(g_app.gpuTempMax, g_app.sens.gpuTemp);
        if (g_cfg.benchSeconds > 0 && frames::BenchElapsed() >= g_cfg.benchSeconds) StopBenchmark(true);
    }
    if (g_app.gameForeground) {
        ULONGLONG now = GetTickCount64();
        if (GameProfile* p = Profile()) p->playSeconds += (now - s_lastPlayTick) / 1000.0;
        s_lastPlayTick = now;
        static int n = 0;
        if (++n % 5 == 0) g_app.exclusiveFullscreen = games::ExclusiveFullscreen();
    }
    if (s_preview) {
        if (!(g_app.gamePid && g_app.live.valid)) FillSample();
        overlay::Redraw();
    }
    if (ui::IsOpen()) ui::Invalidate();
    if (!g_app.gameForeground && !g_app.benching && !s_preview && !ui::IsOpen()) KillTimer(g_app.ctl, TIMER_SENSORS);
}

// ---------------------------------------------------------------- benchmark
void StartBenchmark() {
    if (g_app.benching) return;
    if (!g_app.gamePid) { overlay::Toast(L"Start a game first"); return; }
    if (frames::GetStatus() != frames::Status::Running) {
        overlay::Toast(L"Benchmarks need frame capture (see Settings)");
        return;
    }
    g_app.benching = true;
    g_app.benchExe = g_app.game.exe;
    g_app.benchGame = g_app.gameName;
    g_app.cpuSum = g_app.gpuSum = 0;
    g_app.cpuN = g_app.gpuN = 0;
    g_app.cpuTempMax = g_app.gpuTempMax = -1;
    frames::BenchStart();
    SetTimer(g_app.ctl, TIMER_SENSORS, 1000, nullptr);
    KillTimer(g_app.ctl, TIMER_CAPTURE_IDLE);
    overlay::Toast(L"Benchmark started");
    UpdateTip();
    ui::Invalidate();
}

void StopBenchmark(bool save) {
    if (!g_app.benching) return;
    g_app.benching = false;
    std::vector<float> f = frames::BenchStop();
    UpdateTip();
    if (!save) { overlay::Toast(L"Benchmark canceled"); return; }
    if (f.size() < 30) { overlay::Toast(L"Too short to save"); return; }
    bench::Run run;
    run.exe = g_app.benchExe;
    run.game = g_app.benchGame;
    run.avgCpu = g_app.cpuN ? (float)(g_app.cpuSum / g_app.cpuN) : -1;
    run.avgGpu = g_app.gpuN ? (float)(g_app.gpuSum / g_app.gpuN) : -1;
    run.maxCpuTemp = g_app.cpuTempMax;
    run.maxGpuTemp = g_app.gpuTempMax;
    run.cpu = sensors::CpuName();
    run.gpu = sensors::GpuName();
    if (bench::Save(run, f)) overlay::ShowResult(run, f);
    else overlay::Toast(L"Couldn't save the benchmark");
    ui::OnBenchmarksChanged();
}

// ---------------------------------------------------------------- hotkeys
void ApplyHotkeys() {
    g_app.hotkeyErrors.clear();
    for (int i = 1; i < HkCount; ++i) {
        UnregisterHotKey(g_app.ctl, i);
        const Hotkey& h = g_cfg.hotkeys[i];
        if (!h.vk) continue;
        if (!RegisterHotKey(g_app.ctl, i, h.mods | MOD_NOREPEAT, h.vk)) {
            g_app.hotkeyErrors.push_back(HotkeyText(h));
            Log(L"shortcut %ls is taken", HotkeyText(h).c_str());
        }
    }
}

static OverlayStyle& OwnStyle(GameProfile& p) {
    if (!p.ownStyle) { p.ownStyle = true; p.style = g_cfg.style; }
    return p.style;
}

static void OnHotkey(int id) {
    GameProfile* p = Profile();
    bool inGame = g_app.gameForeground && p;
    switch (id) {
        case HkToggle:
            if (inGame) {
                if (!g_cfg.overlayOn) { g_cfg.overlayOn = true; p->overlay = true; }
                else p->overlay = !p->overlay;
            } else {
                g_cfg.overlayOn = !g_cfg.overlayOn;
            }
            ConfigSaveSoon();
            Render();
            break;
        case HkLayout: {
            if (!inGame) break;
            OverlayStyle& s = OwnStyle(*p);
            s.layout = (Layout)(((int)s.layout + 1) % 3);
            ConfigSaveSoon();
            overlay::SetStyle(s);
            static const wchar_t* names[] = {L"Full panel", L"Compact", L"FPS only"};
            if (!p->overlay || !g_cfg.overlayOn) { p->overlay = true; g_cfg.overlayOn = true; }
            Render();
            overlay::Toast(names[(int)s.layout]);
            break;
        }
        case HkBench:
            if (g_app.benching) StopBenchmark(true);
            else StartBenchmark();
            Render();
            break;
        case HkCrosshair:
            if (!inGame) break;
            p->crosshair = !p->crosshair;
            ConfigSaveSoon();
            Render();
            overlay::Toast(p->crosshair ? L"Crosshair on" : L"Crosshair off");
            break;
    }
    ui::Invalidate();
}

// ---------------------------------------------------------------- preview and move
void PreviewOverlay(bool on) {
    if (on == s_preview) return;
    s_preview = on;
    if (on) {
        overlay::SetStyle(StyleFor(ui::EditScope()));
        SetTimer(g_app.ctl, TIMER_SENSORS, 1000, nullptr);
        sensors::Sample(g_app.sens);
    } else {
        overlay::EndMove();
        if (!g_app.gameForeground) { overlay::Hide(); g_app.live = frames::Live{}; }
    }
    OverlayChanged();
}
bool PreviewingOverlay() { return s_preview; }

void BeginMoveOverlay() {
    PreviewOverlay(true);
    std::wstring scope = ui::EditScope();
    overlay::BeginMove([scope](Anchor a, float x, float y) {
        OverlayStyle* s = &g_cfg.style;
        if (!scope.empty()) if (GameProfile* p = FindGame(scope)) s = &OwnStyle(*p);
        s->anchor = a;
        s->customX = x;
        s->customY = y;
        ConfigSaveSoon();
        ui::Invalidate();
    });
}

// ---------------------------------------------------------------- updates
void CheckUpdates(bool force) { updater::Check(g_app.ctl, force, g_cfg.autoUpdate || force); }

static void TryInstall(bool userAsked) {
    if (g_app.upd.kind != SelfUpdateState::Ready) return;
    if (GameRunning()) {
        // never update while a game is running; try again once it closes
        g_app.updateDeferredForGame = true;
        SetTimer(g_app.ctl, TIMER_INSTALL, 10 * 60 * 1000, nullptr);
        ui::Invalidate();
        return;
    }
    if (!userAsked && ui::IsOpen() && GetForegroundWindow() == ui::Hwnd()) {
        SetTimer(g_app.ctl, TIMER_INSTALL, 60 * 1000, nullptr);   // don't restart under someone using Settings
        return;
    }
    KillTimer(g_app.ctl, TIMER_INSTALL);
    g_app.updateDeferredForGame = false;
    Log(L"installing Reticle %ls", g_app.upd.version.c_str());
    ConfigFlush();
    // hand everything over: the new copy starts its own capture session and tray icon
    bool settingsOpen = ui::IsOpen();
    if (settingsOpen) {
        std::wstring p = DataDir() + L"\\restore.json";
        WriteTextFileAtomic(p, L"{\"settings\":true}");
        ui::Close();
    }
    overlay::Hide();
    overlay::HideCrosshair();
    frames::Stop();
    RemoveTray();
    for (int i = 1; i < HkCount; ++i) UnregisterHotKey(g_app.ctl, i);   // the new copy registers them
    std::wstring err;
    if (updater::Apply(g_app.upd.staged, g_app.upd.version, &err)) {
        Quit();
        return;
    }
    Log(L"update failed: %ls", err.c_str());
    SkipVersion(g_app.upd.version);
    ConfigSave();
    AddTray();
    ApplyHotkeys();
    g_app.upd.kind = SelfUpdateState::Error;
    g_app.upd.message = err;
    if (settingsOpen) ui::Open(L"settings");
    Notify(L"Reticle", err);
}

void InstallUpdateNow() {
    if (g_app.upd.kind == SelfUpdateState::Ready) TryInstall(true);
    else updater::Check(g_app.ctl, true, true);
}

// ---------------------------------------------------------------- frame capture permission
static void HandOverToAdmin() {
    Log(L"switching to the admin copy through the startup task");
    ConfigFlush();
    bool settingsOpen = ui::IsOpen();
    if (settingsOpen) { WriteTextFileAtomic(DataDir() + L"\\restore.json", L"{\"settings\":true}"); ui::Close(); }
    overlay::Hide();
    overlay::HideCrosshair();
    frames::Stop();
    RemoveTray();
    for (int i = 1; i < HkCount; ++i) UnregisterHotKey(g_app.ctl, i);
    ReleaseInstanceMutex();
    if (sys::RunAdminTask()) { Quit(); return; }
    Log(L"couldn't start the admin task");
    DeleteFileW((DataDir() + L"\\restore.json").c_str());
    ReacquireInstanceMutex();
    AddTray();
    ApplyHotkeys();
    if (settingsOpen) ui::Open(L"home");
}

void FixPermissions(bool userAsked) {
    if (sys::IsElevated() || sys::InPerfLogUsers()) return;
    if (GameRunning()) {
        // never prompt while a game is running; try again later
        if (userAsked) overlay::Toast(L"Close the game first");
        SetTimer(g_app.ctl, TIMER_PERMISSION, 60 * 1000, nullptr);
        return;
    }
    if (sys::StartupIsAdmin()) { HandOverToAdmin(); return; }   // set up already: no prompt needed
    if (!userAsked) { ++g_cfg.permissionAsks; ConfigSave(); }
    Log(L"asking Windows for frame-capture permission (%ls)", userAsked ? L"from Settings" : L"automatic");
    if (!sys::SetupAdmin()) {
        Log(L"permission setup declined or failed");
        ui::Invalidate();
        return;
    }
    g_cfg.startWithWindows = true;
    std::wstring err;
    sys::SetStartup(true, true, &err);   // the task replaces the Run entry
    ConfigSave();
    HandOverToAdmin();
}

static void CheckPermissions() {
    if (sys::IsElevated()) {
        // already admin: make sure sign-in starts it that way too
        if (g_cfg.startWithWindows && !sys::StartupIsAdmin()) { std::wstring e; sys::SetStartup(true, true, &e); }
        return;
    }
    if (sys::InPerfLogUsers()) return;
    if (!s_handedOver && sys::StartupIsAdmin()) { HandOverToAdmin(); return; }
    if (s_handedOver || g_cfg.permissionAsks >= 2) return;   // the Home page explains what to do
    FixPermissions(false);
}

// ---------------------------------------------------------------- tray menu
enum : UINT { CMD_SETTINGS = 100, CMD_OVERLAY, CMD_CROSSHAIR, CMD_BENCH, CMD_MOVE, CMD_UPDATE, CMD_QUIT, CMD_INSTALL };

static void TrayMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, CMD_SETTINGS, L"Open Reticle\tCtrl+,");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_cfg.overlayOn ? MF_CHECKED : 0), CMD_OVERLAY, (L"Show overlay in games\t" + HotkeyText(g_cfg.hotkeys[HkToggle])).c_str());
    GameProfile* p = g_app.gamePid ? FindGame(g_app.game.exe) : nullptr;
    AppendMenuW(m, MF_STRING | (p ? 0 : MF_GRAYED) | (p && p->crosshair ? MF_CHECKED : 0), CMD_CROSSHAIR,
                (p ? L"Crosshair in " + p->name : std::wstring(L"Crosshair (start a game)")).c_str());
    AppendMenuW(m, MF_STRING | (g_app.gamePid ? 0 : MF_GRAYED), CMD_BENCH, g_app.benching ? L"Stop benchmark" : L"Start benchmark");
    AppendMenuW(m, MF_STRING, CMD_MOVE, L"Move overlay…");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    if (g_app.upd.kind == SelfUpdateState::Ready) AppendMenuW(m, MF_STRING, CMD_INSTALL, (L"Restart to update to " + g_app.upd.version).c_str());
    else AppendMenuW(m, MF_STRING, CMD_UPDATE, L"Check for updates");
    AppendMenuW(m, MF_STRING, CMD_QUIT, L"Quit Reticle");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_app.ctl);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g_app.ctl, nullptr);
    DestroyMenu(m);
    switch (cmd) {
        case CMD_SETTINGS: OpenSettings(); break;
        case CMD_OVERLAY: g_cfg.overlayOn = !g_cfg.overlayOn; ConfigSaveSoon(); Render(); ui::Invalidate(); break;
        case CMD_CROSSHAIR: if (p) { p->crosshair = !p->crosshair; ConfigSaveSoon(); Render(); ui::Invalidate(); } break;
        case CMD_BENCH: OnHotkey(HkBench); break;
        case CMD_MOVE: OpenSettings(L"overlay"); BeginMoveOverlay(); break;
        case CMD_UPDATE: CheckUpdates(true); OpenSettings(L"settings"); break;
        case CMD_INSTALL: TryInstall(true); break;
        case CMD_QUIT: Quit(); break;
    }
}

void OpenSettings(const wchar_t* page) {
    ui::Open(page);
    SetTimer(g_app.ctl, TIMER_SENSORS, 1000, nullptr);
    sensors::Sample(g_app.sens);
}

void Quit() {
    if (g_app.benching) StopBenchmark(true);
    ConfigFlush();
    ui::Close();
    DestroyWindow(g_app.ctl);
}

// ---------------------------------------------------------------- window
static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD, HWND, LONG idObject, LONG, DWORD, DWORD) {
    if (idObject == OBJID_WINDOW || idObject == 0) PostMessageW(g_app.ctl, WM_RETICLE_FOREGROUND, 0, 0);
}

static LRESULT CALLBACK CtlProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == s_taskbarCreated && s_taskbarCreated) { AddTray(); return 0; }
    switch (msg) {
        case WM_RETICLE_TRAY:
            switch (LOWORD(lp)) {
                case NIN_SELECT: case NIN_KEYSELECT: case WM_LBUTTONUP: OpenSettings(); break;
                case WM_CONTEXTMENU: case WM_RBUTTONUP: TrayMenu(); break;
            }
            return 0;
        case WM_HOTKEY: OnHotkey((int)wp); return 0;
        case WM_RETICLE_FOREGROUND: OnForeground(); return 0;
        case WM_RETICLE_ACTIVATE: OpenSettings(); return 0;
        case WM_RETICLE_QUIT: Quit(); return 0;
        case WM_RETICLE_SELF: {
            std::unique_ptr<SelfUpdateState> s((SelfUpdateState*)lp);
            g_app.upd = *s;
            if (s->kind == SelfUpdateState::Ready && g_cfg.autoUpdate) TryInstall(false);
            ui::Invalidate();
            return 0;
        }
        case WM_TIMER:
            switch (wp) {
                case TIMER_TICK: OnTick(); break;
                case TIMER_SENSORS: OnSensors(); break;
                case TIMER_DETECT: OnDetect(); break;
                case TIMER_ALIVE:
                    if (!games::ProcessAlive(g_app.gamePid)) EndSession();
                    break;
                case TIMER_CAPTURE_IDLE:
                    KillTimer(hwnd, TIMER_CAPTURE_IDLE);
                    if (!g_app.gameForeground && !g_app.benching) frames::Stop();
                    ui::Invalidate();
                    break;
                case TIMER_UPDATE:
                    SetTimer(hwnd, TIMER_UPDATE, 3 * 3600 * 1000, nullptr);
                    CheckUpdates(false);
                    break;
                case TIMER_HEALTHY:
                    KillTimer(hwnd, TIMER_HEALTHY);
                    updater::SignalHealthy(s_after);
                    updater::FinishAfterUpdate(s_after);
                    break;
                case TIMER_INSTALL:
                    KillTimer(hwnd, TIMER_INSTALL);
                    TryInstall(false);
                    break;
                case TIMER_PERMISSION:
                    KillTimer(hwnd, TIMER_PERMISSION);
                    CheckPermissions();
                    break;
            }
            return 0;
        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
            OverlayChanged();
            return 0;
        case WM_SETTINGCHANGE:
            if (lp && !wcscmp((const wchar_t*)lp, L"ImmersiveColorSet")) { gfx::InvalidateSystemColors(); ThemeChanged(); ui::Invalidate(); }
            return 0;
        case WM_DWMCOLORIZATIONCOLORCHANGED:
            gfx::InvalidateSystemColors();
            ThemeChanged();
            ui::Invalidate();
            return 0;
        case WM_POWERBROADCAST:
            if (wp == PBT_APMRESUMEAUTOMATIC) PostMessageW(hwnd, WM_RETICLE_FOREGROUND, 0, 0);
            return TRUE;
        case WM_QUERYENDSESSION:
            ConfigFlush();
            return TRUE;
        case WM_DESTROY:
            if (s_hook) UnhookWinEvent(s_hook);
            if (s_hookMin) UnhookWinEvent(s_hookMin);
            for (int i = 1; i < HkCount; ++i) UnregisterHotKey(hwnd, i);
            RemoveTray();
            overlay::Shutdown();
            frames::Stop();
            sensors::Shutdown();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND Create(HINSTANCE inst, bool openSettings, const updater::AfterUpdate& after, bool handedOver) {
    g_app.inst = inst;
    s_handedOver = handedOver;
    s_after = after;
    if (!gfx::Init()) return nullptr;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = CtlProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = RETICLE_MAIN_CLASS;
    RegisterClassExW(&wc);
    // a hidden top-level window (not message-only) so it gets TaskbarCreated and can be found
    g_app.ctl = CreateWindowExW(WS_EX_TOOLWINDOW, RETICLE_MAIN_CLASS, L"Reticle", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!g_app.ctl) return nullptr;
    s_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(g_app.ctl, s_taskbarCreated, MSGFLT_ALLOW, nullptr);
    // an elevated Reticle still accepts "open Settings" / "quit" from a normal one
    ChangeWindowMessageFilterEx(g_app.ctl, WM_RETICLE_ACTIVATE, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(g_app.ctl, WM_RETICLE_QUIT, MSGFLT_ALLOW, nullptr);

    PruneSkippedVersions(RETICLE_VERSION);
    sensors::Init();
    overlay::Init(inst);
    ThemeChanged();
    AddTray();
    ApplyHotkeys();
    s_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    s_hookMin = SetWinEventHook(EVENT_SYSTEM_MINIMIZEEND, EVENT_SYSTEM_MINIMIZEEND, nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);

    if (g_cfg.firstRun) {
        g_cfg.firstRun = false;
        std::wstring err;
        if (g_cfg.startWithWindows) sys::SetStartup(true, false, &err);
        ConfigSave();
        openSettings = true;
    }
    {
        std::wstring p = DataDir() + L"\\restore.json", text;
        if (ReadTextFile(p, text)) { DeleteFileW(p.c_str()); if (text.find(L"\"settings\":true") != std::wstring::npos) openSettings = true; }
    }
    SetTimer(g_app.ctl, TIMER_PERMISSION, after.active ? 30000 : 3000, nullptr);
    if (after.active) {
        SetTimer(g_app.ctl, TIMER_HEALTHY, 300, nullptr);
        Log(L"started after an update from %ls", after.fromVersion.c_str());
    }
    if (openSettings) OpenSettings(after.active ? L"settings" : nullptr);
    SetTimer(g_app.ctl, TIMER_UPDATE, after.active ? 10 * 60 * 1000 : 30 * 1000, nullptr);
    PostMessageW(g_app.ctl, WM_RETICLE_FOREGROUND, 0, 0);   // a game may already be in front
    return g_app.ctl;
}

} // namespace app
