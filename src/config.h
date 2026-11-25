// Reticle - settings and per-game profiles (DataDir()\settings.json)
#pragma once
#include "common.h"
#include <map>

// Stats the overlay can show (bit flags)
enum Stat : uint32_t {
    StatFps        = 1u << 0,
    StatFrameTime  = 1u << 1,
    StatGraph      = 1u << 2,
    StatLows       = 1u << 3,
    StatCpuUse     = 1u << 4,
    StatCpuTemp    = 1u << 5,
    StatGpuUse     = 1u << 6,
    StatGpuTemp    = 1u << 7,
    StatGpuClock   = 1u << 8,
    StatGpuPower   = 1u << 9,
    StatVram       = 1u << 10,
    StatRam        = 1u << 11,
    StatClock      = 1u << 12,
    StatSession    = 1u << 13,
};
constexpr int kStatCount = 14;
constexpr uint32_t kDefaultStats = StatFps | StatFrameTime | StatGraph | StatLows | StatCpuUse | StatCpuTemp | StatGpuUse |
                                   StatGpuTemp | StatVram | StatRam;
const wchar_t* StatName(int bit);         // "FPS", "Frame time"...
const wchar_t* StatHint(int bit);

enum class Layout : int { Full = 0, Compact = 1, Minimal = 2 };
enum class Anchor : int { TopLeft = 0, TopRight = 1, BottomLeft = 2, BottomRight = 3, Custom = 4 };

struct OverlayStyle {
    Layout layout = Layout::Full;
    uint32_t stats = kDefaultStats;
    Anchor anchor = Anchor::TopLeft;
    float customX = 0.02f, customY = 0.03f;   // top-left as a fraction of the game window (Custom)
    float textSize = 13.0f;                   // DIPs
    float opacity = 0.72f;                    // panel background opacity
    bool followAccent = true;                 // label color from the theme accent
    uint32_t accentColor = 0x7AA2FF;          // own label color when !followAccent
    uint32_t textColor = 0xF5F5F7;
    uint32_t bgColor = 0x0E0E12;
};

enum class CrossShape : int { Dot = 0, Cross = 1, Circle = 2, T = 3, Image = 4 };
struct CrosshairStyle {
    CrossShape shape = CrossShape::Cross;
    float size = 10;           // arm length / radius, px
    float gap = 4;             // px
    float thickness = 2;       // px
    bool centerDot = false;
    bool outline = true;
    float outlineWidth = 1;    // px
    uint32_t color = 0x3CFF6A;
    uint32_t outlineColor = 0x000000;
    float opacity = 1.0f;
    std::wstring image;        // PNG path (Image)
    float imageScale = 1.0f;
};

enum class Detect : int { Auto = 0, Always = 1, Never = 2 };

struct GameProfile {
    std::wstring exe;          // lower-case exe name: the key
    std::wstring name;         // shown name (from the exe's version info, else the window title)
    std::wstring path;
    Detect detect = Detect::Auto;
    bool overlay = true;       // show the overlay in this game
    bool crosshair = false;    // show the crosshair in this game
    bool ownStyle = false;     // this game has its own overlay preset
    OverlayStyle style;
    long long lastSeen = 0;    // unix seconds
    double playSeconds = 0;
};

struct Hotkey { UINT mods = 0; UINT vk = 0; };   // MOD_* | MOD_NOREPEAT; vk 0 = none
enum HotkeyId { HkToggle = 1, HkLayout = 2, HkBench = 3, HkCrosshair = 4, HkCount = 5 };

struct Config {
    // look
    std::wstring theme = L"dark";
    std::wstring accent = L"theme";
    // overlay
    bool overlayOn = true;                 // master switch (hotkey)
    OverlayStyle style;                    // default for all games
    int refreshMs = 250;                   // overlay redraw interval
    bool tempF = false;
    bool clock24 = false;
    bool showWithoutFrames = true;         // show the overlay even if frame capture isn't available
    // crosshair
    CrosshairStyle cross;
    bool crosshairDefault = false;         // new games get the crosshair on
    // games
    std::map<std::wstring, GameProfile> games;
    // benchmark
    int benchSeconds = 60;                 // 0 = until stopped
    // shortcuts
    Hotkey hotkeys[HkCount];
    // app
    bool startWithWindows = true;
    bool firstRun = true;
    int permissionAsks = 0;                // automatic frame-capture permission prompts so far (max 2)
    bool autoUpdate = true;
    bool beta = false;
    std::wstring skipVersion;
    int windowX = CW_USEDEFAULT, windowY = CW_USEDEFAULT, windowW = 0, windowH = 0;
    Config();
};

extern Config g_cfg;
void ConfigLoad();
void ConfigSave();
void ConfigSaveSoon();                     // coalesced save (called while dragging sliders etc.)
void ConfigFlush();                        // writes a pending save now

GameProfile* FindGame(const std::wstring& exe);
GameProfile& EnsureGame(const std::wstring& exe, const std::wstring& name, const std::wstring& path);
OverlayStyle& StyleFor(const std::wstring& exe);   // the game's own preset, or the default

bool IsSkippedVersion(const std::wstring& v);
void SkipVersion(const std::wstring& v);
void PruneSkippedVersions(const std::wstring& current);

std::wstring HotkeyText(const Hotkey& h);
std::wstring HexColor(uint32_t c);
bool ParseHexColor(const std::wstring& s, uint32_t& out);
