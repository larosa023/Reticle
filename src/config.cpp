// Reticle - settings and per-game profiles
#include "config.h"
#include "util.h"
#include "json.h"
#include <algorithm>

Config g_cfg;

Config::Config() {
    hotkeys[HkToggle] = {MOD_ALT | MOD_SHIFT, 'O'};
    hotkeys[HkLayout] = {MOD_ALT | MOD_SHIFT, 'L'};
    hotkeys[HkBench] = {MOD_ALT | MOD_SHIFT, 'B'};
    hotkeys[HkCrosshair] = {MOD_ALT | MOD_SHIFT, 'X'};
}

static const wchar_t* kStatNames[kStatCount] = {
    L"FPS", L"Frame time", L"Frame-time graph", L"1% and 0.1% lows", L"CPU usage", L"CPU temperature",
    L"GPU usage", L"GPU temperature", L"GPU clock", L"GPU power", L"VRAM", L"RAM", L"Clock", L"Session length",
};
static const wchar_t* kStatHints[kStatCount] = {
    L"Frames per second, averaged over the last second",
    L"Milliseconds between frames",
    L"The last few seconds of frame times (full panel)",
    L"Average FPS of the slowest 1% and 0.1% of frames, last 30 seconds",
    L"All cores", L"Needs admin and the PawnIO driver",
    L"NVIDIA", L"NVIDIA", L"NVIDIA, core clock", L"NVIDIA, board power", L"NVIDIA, used and total",
    L"Used and total system memory", L"The time of day", L"How long you've been playing this session",
};
const wchar_t* StatName(int bit) { return bit >= 0 && bit < kStatCount ? kStatNames[bit] : L""; }
const wchar_t* StatHint(int bit) { return bit >= 0 && bit < kStatCount ? kStatHints[bit] : L""; }

std::wstring HexColor(uint32_t c) {
    wchar_t b[16];
    swprintf(b, 16, L"#%06X", c & 0xFFFFFF);
    return b;
}

bool ParseHexColor(const std::wstring& s0, uint32_t& out) {
    std::wstring s = Trim(s0);
    if (!s.empty() && s[0] == L'#') s.erase(0, 1);
    if (s.size() != 6 || s.find_first_not_of(L"0123456789abcdefABCDEF") != std::wstring::npos) return false;
    out = (uint32_t)wcstoul(s.c_str(), nullptr, 16);
    return true;
}

static uint32_t GetColor(const json::Value& v, const wchar_t* key, uint32_t def) {
    uint32_t c;
    return ParseHexColor(v.GetStr(key), c) ? c : def;
}

template <class T> static T Clamp(double v, T lo, T hi) { return (T)std::min<double>(hi, std::max<double>(lo, v)); }

static OverlayStyle ReadStyle(const json::Value* v, const OverlayStyle& def) {
    OverlayStyle s = def;
    if (!v || v->type != json::Value::Object) return s;
    s.layout = (Layout)Clamp<int>(v->GetNum(L"layout", (int)s.layout), 0, 2);
    s.stats = (uint32_t)v->GetNum(L"stats", s.stats);
    s.anchor = (Anchor)Clamp<int>(v->GetNum(L"anchor", (int)s.anchor), 0, 4);
    s.customX = Clamp<float>(v->GetNum(L"x", s.customX), 0.0f, 1.0f);
    s.customY = Clamp<float>(v->GetNum(L"y", s.customY), 0.0f, 1.0f);
    s.textSize = Clamp<float>(v->GetNum(L"textSize", s.textSize), 9.0f, 32.0f);
    s.opacity = Clamp<float>(v->GetNum(L"opacity", s.opacity), 0.0f, 1.0f);
    s.followAccent = v->GetBool(L"followAccent", s.followAccent);
    s.accentColor = GetColor(*v, L"accentColor", s.accentColor);
    s.textColor = GetColor(*v, L"textColor", s.textColor);
    s.bgColor = GetColor(*v, L"bgColor", s.bgColor);
    return s;
}

static json::Value WriteStyle(const OverlayStyle& s) {
    json::Value v = json::Value::MakeObject();
    v.Set(L"layout", json::Value::Num((int)s.layout));
    v.Set(L"stats", json::Value::Num(s.stats));
    v.Set(L"anchor", json::Value::Num((int)s.anchor));
    v.Set(L"x", json::Value::Num(s.customX));
    v.Set(L"y", json::Value::Num(s.customY));
    v.Set(L"textSize", json::Value::Num(s.textSize));
    v.Set(L"opacity", json::Value::Num(s.opacity));
    v.Set(L"followAccent", json::Value::Boolean(s.followAccent));
    v.Set(L"accentColor", json::Value::Str(HexColor(s.accentColor)));
    v.Set(L"textColor", json::Value::Str(HexColor(s.textColor)));
    v.Set(L"bgColor", json::Value::Str(HexColor(s.bgColor)));
    return v;
}

static std::wstring ConfigPath() { return DataDir() + L"\\settings.json"; }

void ConfigLoad() {
    std::wstring text;
    if (!ReadTextFile(ConfigPath(), text)) return;
    json::Value v;
    if (!json::Parser(text).Parse(v) || v.type != json::Value::Object) { Log(L"settings.json is damaged; using defaults"); return; }
    Config c;
    c.firstRun = false;
    c.theme = v.GetStr(L"theme", c.theme);
    c.accent = v.GetStr(L"accent", c.accent);
    c.overlayOn = v.GetBool(L"overlayOn", c.overlayOn);
    c.style = ReadStyle(v.Get(L"overlay"), c.style);
    c.refreshMs = Clamp<int>(v.GetNum(L"refreshMs", c.refreshMs), 100, 1000);
    c.tempF = v.GetBool(L"tempF", c.tempF);
    c.clock24 = v.GetBool(L"clock24", c.clock24);
    c.showWithoutFrames = v.GetBool(L"showWithoutFrames", c.showWithoutFrames);
    if (const json::Value* x = v.Get(L"crosshair"); x && x->type == json::Value::Object) {
        CrosshairStyle& s = c.cross;
        s.shape = (CrossShape)Clamp<int>(x->GetNum(L"shape", (int)s.shape), 0, 4);
        s.size = Clamp<float>(x->GetNum(L"size", s.size), 1, 64);
        s.gap = Clamp<float>(x->GetNum(L"gap", s.gap), 0, 40);
        s.thickness = Clamp<float>(x->GetNum(L"thickness", s.thickness), 1, 12);
        s.centerDot = x->GetBool(L"centerDot", s.centerDot);
        s.outline = x->GetBool(L"outline", s.outline);
        s.outlineWidth = Clamp<float>(x->GetNum(L"outlineWidth", s.outlineWidth), 0.5f, 4);
        s.color = GetColor(*x, L"color", s.color);
        s.outlineColor = GetColor(*x, L"outlineColor", s.outlineColor);
        s.opacity = Clamp<float>(x->GetNum(L"opacity", s.opacity), 0.05f, 1);
        s.image = x->GetStr(L"image");
        s.imageScale = Clamp<float>(x->GetNum(L"imageScale", s.imageScale), 0.1f, 4);
    }
    c.crosshairDefault = v.GetBool(L"crosshairDefault", c.crosshairDefault);
    if (const json::Value* g = v.Get(L"games"); g && g->type == json::Value::Array) {
        for (const json::Value& e : g->arr) {
            GameProfile p;
            p.exe = Lower(e.GetStr(L"exe"));
            if (p.exe.empty()) continue;
            p.name = e.GetStr(L"name", p.exe);
            p.path = e.GetStr(L"path");
            p.detect = (Detect)Clamp<int>(e.GetNum(L"detect", 0), 0, 2);
            p.overlay = e.GetBool(L"overlay", true);
            p.crosshair = e.GetBool(L"crosshair", false);
            p.ownStyle = e.GetBool(L"ownStyle", false);
            p.style = ReadStyle(e.Get(L"style"), c.style);
            p.lastSeen = (long long)e.GetNum(L"lastSeen", 0);
            p.playSeconds = e.GetNum(L"playSeconds", 0);
            c.games[p.exe] = p;
        }
    }
    c.benchSeconds = Clamp<int>(v.GetNum(L"benchSeconds", c.benchSeconds), 0, 600);
    if (const json::Value* h = v.Get(L"hotkeys"); h && h->type == json::Value::Object) {
        const wchar_t* names[HkCount] = {L"", L"toggle", L"layout", L"benchmark", L"crosshair"};
        for (int i = 1; i < HkCount; ++i) {
            if (const json::Value* k = h->Get(names[i]); k && k->type == json::Value::Object) {
                c.hotkeys[i].mods = (UINT)k->GetNum(L"mods", c.hotkeys[i].mods);
                c.hotkeys[i].vk = (UINT)k->GetNum(L"vk", c.hotkeys[i].vk);
            }
        }
    }
    c.startWithWindows = v.GetBool(L"startWithWindows", c.startWithWindows);
    c.autoUpdate = v.GetBool(L"autoUpdate", c.autoUpdate);
    c.beta = v.GetBool(L"beta", c.beta);
    c.skipVersion = v.GetStr(L"skipVersion");
    if (const json::Value* w = v.Get(L"window"); w && w->type == json::Value::Object) {
        c.windowX = (int)w->GetNum(L"x", CW_USEDEFAULT);
        c.windowY = (int)w->GetNum(L"y", CW_USEDEFAULT);
        c.windowW = (int)w->GetNum(L"w", 0);
        c.windowH = (int)w->GetNum(L"h", 0);
    }
    g_cfg = c;
}

void ConfigSave() {
    json::Value v = json::Value::MakeObject();
    v.Set(L"schema", json::Value::Num(1));
    v.Set(L"theme", json::Value::Str(g_cfg.theme));
    v.Set(L"accent", json::Value::Str(g_cfg.accent));
    v.Set(L"overlayOn", json::Value::Boolean(g_cfg.overlayOn));
    v.Set(L"overlay", WriteStyle(g_cfg.style));
    v.Set(L"refreshMs", json::Value::Num(g_cfg.refreshMs));
    v.Set(L"tempF", json::Value::Boolean(g_cfg.tempF));
    v.Set(L"clock24", json::Value::Boolean(g_cfg.clock24));
    v.Set(L"showWithoutFrames", json::Value::Boolean(g_cfg.showWithoutFrames));
    json::Value x = json::Value::MakeObject();
    const CrosshairStyle& s = g_cfg.cross;
    x.Set(L"shape", json::Value::Num((int)s.shape));
    x.Set(L"size", json::Value::Num(s.size));
    x.Set(L"gap", json::Value::Num(s.gap));
    x.Set(L"thickness", json::Value::Num(s.thickness));
    x.Set(L"centerDot", json::Value::Boolean(s.centerDot));
    x.Set(L"outline", json::Value::Boolean(s.outline));
    x.Set(L"outlineWidth", json::Value::Num(s.outlineWidth));
    x.Set(L"color", json::Value::Str(HexColor(s.color)));
    x.Set(L"outlineColor", json::Value::Str(HexColor(s.outlineColor)));
    x.Set(L"opacity", json::Value::Num(s.opacity));
    x.Set(L"image", json::Value::Str(s.image));
    x.Set(L"imageScale", json::Value::Num(s.imageScale));
    v.Set(L"crosshair", x);
    v.Set(L"crosshairDefault", json::Value::Boolean(g_cfg.crosshairDefault));
    json::Value games = json::Value::MakeArray();
    for (auto& kv : g_cfg.games) {
        const GameProfile& p = kv.second;
        json::Value e = json::Value::MakeObject();
        e.Set(L"exe", json::Value::Str(p.exe));
        e.Set(L"name", json::Value::Str(p.name));
        e.Set(L"path", json::Value::Str(p.path));
        e.Set(L"detect", json::Value::Num((int)p.detect));
        e.Set(L"overlay", json::Value::Boolean(p.overlay));
        e.Set(L"crosshair", json::Value::Boolean(p.crosshair));
        e.Set(L"ownStyle", json::Value::Boolean(p.ownStyle));
        if (p.ownStyle) e.Set(L"style", WriteStyle(p.style));
        e.Set(L"lastSeen", json::Value::Num((double)p.lastSeen));
        e.Set(L"playSeconds", json::Value::Num(p.playSeconds));
        games.arr.push_back(e);
    }
    v.Set(L"games", games);
    v.Set(L"benchSeconds", json::Value::Num(g_cfg.benchSeconds));
    json::Value hk = json::Value::MakeObject();
    const wchar_t* names[HkCount] = {L"", L"toggle", L"layout", L"benchmark", L"crosshair"};
    for (int i = 1; i < HkCount; ++i) {
        json::Value k = json::Value::MakeObject();
        k.Set(L"mods", json::Value::Num(g_cfg.hotkeys[i].mods));
        k.Set(L"vk", json::Value::Num(g_cfg.hotkeys[i].vk));
        hk.Set(names[i], k);
    }
    v.Set(L"hotkeys", hk);
    v.Set(L"startWithWindows", json::Value::Boolean(g_cfg.startWithWindows));
    v.Set(L"autoUpdate", json::Value::Boolean(g_cfg.autoUpdate));
    v.Set(L"beta", json::Value::Boolean(g_cfg.beta));
    v.Set(L"skipVersion", json::Value::Str(g_cfg.skipVersion));
    json::Value w = json::Value::MakeObject();
    w.Set(L"x", json::Value::Num(g_cfg.windowX));
    w.Set(L"y", json::Value::Num(g_cfg.windowY));
    w.Set(L"w", json::Value::Num(g_cfg.windowW));
    w.Set(L"h", json::Value::Num(g_cfg.windowH));
    v.Set(L"window", w);
    std::wstring out;
    json::Write(v, out);
    EnsureDir(DataDir());
    if (!WriteTextFileAtomic(ConfigPath(), out)) Log(L"couldn't save settings.json");
}

// Saves are coalesced: dragging a slider changes settings many times a second.
static bool s_pending = false;
static VOID CALLBACK SaveTimer(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    if (s_pending) { s_pending = false; ConfigSave(); }
}
static UINT_PTR s_timer = 0;
void ConfigSaveSoon() {
    s_pending = true;
    if (s_timer) KillTimer(nullptr, s_timer);
    s_timer = SetTimer(nullptr, 0, 800, SaveTimer);
}
void ConfigFlush() {
    if (s_timer) { KillTimer(nullptr, s_timer); s_timer = 0; }
    if (s_pending) { s_pending = false; ConfigSave(); }
}

GameProfile* FindGame(const std::wstring& exe) {
    auto it = g_cfg.games.find(Lower(exe));
    return it == g_cfg.games.end() ? nullptr : &it->second;
}

GameProfile& EnsureGame(const std::wstring& exe, const std::wstring& name, const std::wstring& path) {
    std::wstring key = Lower(exe);
    auto it = g_cfg.games.find(key);
    if (it != g_cfg.games.end()) {
        if (!path.empty()) it->second.path = path;
        return it->second;
    }
    GameProfile p;
    p.exe = key;
    p.name = name.empty() ? exe : name;
    p.path = path;
    p.crosshair = g_cfg.crosshairDefault;
    p.style = g_cfg.style;
    auto& ref = g_cfg.games[key] = p;
    ConfigSaveSoon();
    return ref;
}

OverlayStyle& StyleFor(const std::wstring& exe) {
    if (GameProfile* g = FindGame(exe); g && g->ownStyle) return g->style;
    return g_cfg.style;
}

// ---------------------------------------------------------------- skipped versions
static std::vector<std::wstring> SkipList() {
    std::vector<std::wstring> out;
    size_t i = 0;
    const std::wstring& s = g_cfg.skipVersion;
    while (i < s.size()) {
        size_t e = s.find(L',', i);
        if (e == std::wstring::npos) e = s.size();
        std::wstring v = Trim(s.substr(i, e - i));
        if (!v.empty()) out.push_back(v);
        i = e + 1;
    }
    return out;
}
static void SetSkipList(const std::vector<std::wstring>& list) {
    g_cfg.skipVersion.clear();
    for (auto& v : list) g_cfg.skipVersion += (g_cfg.skipVersion.empty() ? L"" : L",") + v;
}
bool IsSkippedVersion(const std::wstring& v) {
    for (auto& s : SkipList()) if (CompareVersions(s, v) == 0) return true;
    return false;
}
void SkipVersion(const std::wstring& v) {
    if (IsSkippedVersion(v)) return;
    auto list = SkipList();
    list.push_back(NormalizeVersion(v));
    SetSkipList(list);
}
void PruneSkippedVersions(const std::wstring& current) {
    std::vector<std::wstring> keep;
    for (auto& v : SkipList()) if (CompareVersions(v, current) > 0) keep.push_back(v);
    SetSkipList(keep);
}

// ---------------------------------------------------------------- hotkeys
static std::wstring KeyName(UINT vk) {
    if (vk >= 'A' && vk <= 'Z') return std::wstring(1, (wchar_t)vk);
    if (vk >= '0' && vk <= '9') return std::wstring(1, (wchar_t)vk);
    if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return L"Num " + std::to_wstring(vk - VK_NUMPAD0);
    switch (vk) {
        case VK_HOME: return L"Home"; case VK_END: return L"End"; case VK_INSERT: return L"Insert"; case VK_DELETE: return L"Delete";
        case VK_PRIOR: return L"Page Up"; case VK_NEXT: return L"Page Down"; case VK_PAUSE: return L"Pause"; case VK_SCROLL: return L"Scroll Lock";
        case VK_SNAPSHOT: return L"Print Screen"; case VK_UP: return L"Up"; case VK_DOWN: return L"Down"; case VK_LEFT: return L"Left"; case VK_RIGHT: return L"Right";
        case VK_OEM_3: return L"`"; case VK_OEM_MINUS: return L"-"; case VK_OEM_PLUS: return L"="; case VK_OEM_COMMA: return L","; case VK_OEM_PERIOD: return L".";
        case VK_OEM_2: return L"/"; case VK_OEM_1: return L";"; case VK_OEM_7: return L"'"; case VK_OEM_4: return L"["; case VK_OEM_6: return L"]"; case VK_OEM_5: return L"\\";
        case VK_SPACE: return L"Space";
    }
    wchar_t buf[64] = {};
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    if (sc && GetKeyNameTextW((LONG)(sc << 16), buf, 64) > 0) return buf;
    return L"Key " + std::to_wstring(vk);
}

std::wstring HotkeyText(const Hotkey& h) {
    if (!h.vk) return L"None";
    std::wstring s;
    if (h.mods & MOD_CONTROL) s += L"Ctrl+";
    if (h.mods & MOD_ALT) s += L"Alt+";
    if (h.mods & MOD_SHIFT) s += L"Shift+";
    if (h.mods & MOD_WIN) s += L"Win+";
    return s + KeyName(h.vk);
}
