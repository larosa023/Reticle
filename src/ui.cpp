// Reticle - the Settings window.
//
// Drawn with Direct2D in the suite's style (Hangar's sidebar, cards and controls). It exists only while
// open: closing it destroys the window and frees its memory; Reticle keeps running in the tray.
#include "ui.h"
#include "app.h"
#include "config.h"
#include "overlay.h"
#include "system.h"
#include "themes.h"
#include "gfx.h"
#include "util.h"
#include <windowsx.h>
#include <shellapi.h>
#include <commdlg.h>
#include <wincodec.h>
#include <functional>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <map>

using namespace gfx;

namespace ui {

enum class Page { Home, Overlay, Crosshair, Games, Benchmarks, Shortcuts, Themes, Settings };

struct Hot {
    D2D1_RECT_F r;
    uint64_t key;
    std::function<void()> click;
    std::function<void(float)> drag;   // sliders: 0..1 across [r.left+dl, r.right-dr]
    bool enabled = true;
};

static HWND g_hwnd = nullptr;
static ID2D1HwndRenderTarget* g_rt = nullptr;
static ID2D1SolidColorBrush* g_br = nullptr;
static Theme g_t;
static bool g_rtOpaque = false;
static std::vector<ThemeDef> g_customThemes;
static Canvas g_c;
static float g_scale = 1, g_W = 0, g_H = 0;
static Page g_page = Page::Home;
static std::map<Page, float> g_scrollFor;
static float g_scroll = 0, g_contentH = 0;
static std::vector<Hot> g_hot;
static uint64_t g_hover = 0, g_pressed = 0, g_dragKey = 0;
static std::function<void(float)> g_dragFn;
static float g_dragL = 0, g_dragR = 1;
static bool g_tracking = false;
static std::wstring g_scope;                 // overlay preset being edited: "" = default
static int g_capture = 0;                    // shortcut being recorded (HotkeyId)
static std::vector<bench::Run> g_runs;
static bool g_runsLoaded = false;
static int g_sel = -1, g_cmp = -1;           // selected / compared run
static std::vector<float> g_selFrames, g_cmpFrames;
static std::wstring g_toast;
static ULONGLONG g_toastUntil = 0;
static ID2D1Bitmap* g_crossImg = nullptr;
static std::wstring g_crossImgPath;
static bool g_previewWanted = true;          // live overlay preview while on the Overlay page
static bool g_startup = false, g_startupAdmin = false, g_grantPending = false;   // read when Settings opens, not every frame
static void ReadSystemState(bool task = true) {
    if (task) sys::RefreshStartupState();   // runs schtasks: only when the Settings page needs it
    g_startup = sys::StartupEnabled();
    g_startupAdmin = sys::StartupIsAdmin();
    g_grantPending = sys::PerfLogGrantedPendingSignIn();
}

static const float kSidebar = 224;

const std::wstring& EditScope() { return g_scope; }
HWND Hwnd() { return g_hwnd; }
bool IsOpen() { return g_hwnd != nullptr; }
void Invalidate() { if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE); }

static void Toast(const std::wstring& t) { g_toast = t; g_toastUntil = GetTickCount64() + 3500; Invalidate(); SetTimer(g_hwnd, 2, 3600, nullptr); }

static uint64_t K(const char* name, int arg = 0) {
    uint64_t h = 1469598103934665603ull;
    for (const char* p = name; *p; ++p) { h ^= (unsigned char)*p; h *= 1099511628211ull; }
    return h ^ ((uint64_t)(arg + 1) * 0x9E3779B97F4A7C15ull);
}

static void AddHot(const D2D1_RECT_F& r, uint64_t key, std::function<void()> click, bool enabled = true) {
    g_hot.push_back({r, key, std::move(click), nullptr, enabled});
}
static bool IsHover(uint64_t key) { return g_hover == key; }

// ---------------------------------------------------------------- theme and device
static void DiscardDevice() {
    SafeRelease(g_crossImg);
    SafeRelease(g_br);
    SafeRelease(g_rt);
}

static void ApplyTheme() {
    g_customThemes = LoadCustomThemes();
    g_t = MakeTheme(FindTheme(g_cfg.theme, g_customThemes), g_cfg.accent);
    if (g_hwnd) {
        ApplyWindowTheme(g_hwnd, g_t);
        if (g_rt && g_rtOpaque != !g_t.backdrop) DiscardDevice();
    }
    Invalidate();
}

static bool EnsureDevice() {
    if (g_rt) return true;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    g_rtOpaque = !g_t.backdrop;
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, g_rtOpaque ? D2D1_ALPHA_MODE_IGNORE : D2D1_ALPHA_MODE_PREMULTIPLIED));
    UINT dpi = GetDpiForWindow(g_hwnd);
    props.dpiX = props.dpiY = (float)dpi;
    if (FAILED(d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(g_hwnd, D2D1::SizeU(std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom))), &g_rt)))
        return false;
    g_rt->SetTextAntialiasMode(g_rtOpaque ? D2D1_TEXT_ANTIALIAS_MODE_DEFAULT : D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &g_br);
    return true;
}

// ---------------------------------------------------------------- controls
enum class Btn { Primary, Secondary, Subtle, Danger };

static float ButtonWidth(const std::wstring& label, wchar_t glyph) {
    float w = MeasureWidth(label, Format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 28;
    if (glyph) w += label.empty() ? 6 : 22;
    return std::max(w, label.empty() ? 34.0f : 76.0f);
}

static void Button(const D2D1_RECT_F& r, const std::wstring& label, wchar_t glyph, Btn style, uint64_t key, std::function<void()> click, bool enabled = true) {
    bool hot = enabled && IsHover(key);
    bool down = hot && g_pressed == key;
    D2D1_COLOR_F bg, fg;
    switch (style) {
        case Btn::Primary: bg = hot ? g_t.accentHover : g_t.accent; fg = g_t.onAccent; break;
        case Btn::Danger: bg = hot ? Alpha(g_t.bad, 0.22f) : Alpha(g_t.bad, 0.12f); fg = g_t.bad; break;
        case Btn::Subtle: bg = hot ? g_t.surfaceHover : Alpha(g_t.surface, 0.0f); fg = g_t.text; break;
        default: bg = hot ? g_t.surfaceStrong : Mix(g_t.surface, g_t.surfaceStrong, 0.55f); fg = g_t.text; break;
    }
    if (!enabled) { bg = Alpha(g_t.surfaceStrong, 0.5f); fg = g_t.faint; }
    if (down) bg = Mix(bg, g_t.dark ? Rgb(0x000000) : Rgb(0xFFFFFF), 0.12f);
    g_c.FillRound(r, 6, bg);
    if (style == Btn::Secondary && enabled) g_c.StrokeRound(r, 6, g_t.divider);
    auto* tf = Format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    if (glyph && label.empty()) {
        g_c.Glyph(glyph, Format(14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true), r, fg);
    } else if (glyph) {
        float tw = MeasureWidth(label, tf) + 22;
        float x = r.left + (r.right - r.left - tw) / 2;
        g_c.Glyph(glyph, Format(13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, false, true), D2D1::RectF(x, r.top, x + 18, r.bottom), fg);
        g_c.Text(label, Format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(x + 22, r.top, r.right, r.bottom), fg);
    } else {
        g_c.Text(label, tf, r, fg);
    }
    AddHot(r, key, std::move(click), enabled);
}

// a right-aligned button ending at `right`; returns its left edge
static float ButtonR(float right, float y, const std::wstring& label, wchar_t glyph, Btn style, uint64_t key, std::function<void()> click, bool enabled = true) {
    float w = ButtonWidth(label, glyph);
    Button(D2D1::RectF(right - w, y, right, y + 32), label, glyph, style, key, std::move(click), enabled);
    return right - w;
}

static void Toggle(float right, float y, bool on, uint64_t key, std::function<void()> click, bool enabled = true) {
    D2D1_RECT_F r = D2D1::RectF(right - 44, y, right, y + 22);
    bool hot = enabled && IsHover(key);
    D2D1_COLOR_F track = on ? (hot ? g_t.accentHover : g_t.accent) : (hot ? g_t.surfaceStrong : Mix(g_t.surface, g_t.surfaceStrong, 0.6f));
    if (!enabled) track = Alpha(track, 0.45f);
    g_c.FillRound(r, 11, track);
    if (!on) g_c.StrokeRound(r, 11, Alpha(g_t.subtext, 0.6f));
    float cx = on ? r.right - 11 : r.left + 11;
    g_c.Circle(cx, y + 11, on ? 7 : 6, on ? g_t.onAccent : g_t.subtext);
    AddHot(D2D1::RectF(r.left - 6, r.top - 6, r.right + 4, r.bottom + 6), key, std::move(click), enabled);
}

static float SegmentedWidth(const std::vector<std::wstring>& opts) {
    float w = 0;
    for (auto& o : opts) w += MeasureWidth(o, Format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER)) + 26 + 6;
    return w - 6;
}

static void Segmented(float right, float y, const std::vector<std::wstring>& opts, int sel, const char* name, std::function<void(int)> pick) {
    float x = right - SegmentedWidth(opts);
    for (size_t k = 0; k < opts.size(); ++k) {
        float w = MeasureWidth(opts[k], Format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER)) + 26;
        D2D1_RECT_F r = D2D1::RectF(x, y, x + w, y + 32);
        bool s = (int)k == sel;
        uint64_t key = K(name, (int)k);
        g_c.FillRound(r, 6, s ? Alpha(g_t.accent, 0.18f) : (IsHover(key) ? g_t.surfaceHover : Mix(g_t.surface, g_t.surfaceStrong, 0.3f)));
        if (s) g_c.StrokeRound(r, 6, Alpha(g_t.accent, 0.6f));
        g_c.Text(opts[k], Format(13, s ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER), r, s ? g_t.text : g_t.subtext);
        int idx = (int)k;
        AddHot(r, key, [pick, idx] { pick(idx); });
        x += w + 6;
    }
}

// slider: track between left and right; label shows the value
static void Slider(float left, float right, float y, float value, float mn, float mx, const std::wstring& text, uint64_t key, std::function<void(float)> set) {
    float t = std::clamp((value - mn) / (mx - mn), 0.0f, 1.0f);
    float tl = left, tr = right - 60;
    float cy = y + 16;
    g_c.FillRound(D2D1::RectF(tl, cy - 2, tr, cy + 2), 2, g_t.surfaceStrong);
    float x = tl + (tr - tl) * t;
    g_c.FillRound(D2D1::RectF(tl, cy - 2, x, cy + 2), 2, g_t.accent);
    bool hot = IsHover(key) || g_dragKey == key;
    g_c.Circle(x, cy, hot ? 9 : 8, g_t.dark ? Rgb(0x2A2A31) : Rgb(0xFFFFFF));
    g_c.Circle(x, cy, hot ? 6 : 5, g_t.accent);
    g_c.Text(text, Format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING), D2D1::RectF(tr + 8, y, right, y + 32), g_t.text);
    Hot h{D2D1::RectF(tl - 10, y, tr + 10, y + 32), key, nullptr, nullptr, true};
    h.drag = [set, mn, mx](float tt) { set(mn + (mx - mn) * tt); };
    g_hot.push_back(h);
    // remember the track ends for dragging
    if (g_dragKey == key) { g_dragL = tl; g_dragR = tr; }
    g_hot.back().r.left = tl - 10;
    g_hot.back().r.right = tr + 10;
}

static void Swatch(float cx, float cy, uint32_t color, bool sel, uint64_t key, std::function<void()> click) {
    if (sel) g_c.Circle(cx, cy, 14, g_t.text);
    else if (IsHover(key)) g_c.Circle(cx, cy, 14, Alpha(g_t.text, 0.3f));
    g_c.Circle(cx, cy, sel ? 11.5f : 12, Rgb(color));
    if ((color & 0xFFFFFF) < 0x202020 && !sel) g_c.Circle(cx, cy, 12, Alpha(g_t.text, 0.15f));
    AddHot(D2D1::RectF(cx - 15, cy - 15, cx + 15, cy + 15), key, std::move(click));
}

static void Check(const D2D1_RECT_F& r, const std::wstring& label, const std::wstring& sub, bool on, uint64_t key, std::function<void()> click) {
    bool hot = IsHover(key);
    if (hot) g_c.FillRound(r, 6, g_t.surfaceHover);
    D2D1_RECT_F b = D2D1::RectF(r.left + 10, r.top + 12, r.left + 30, r.top + 32);
    if (on) {
        g_c.FillRound(b, 4, g_t.accent);
        g_c.Glyph(icon::Check, Format(11, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true), b, g_t.onAccent);
    } else {
        g_c.StrokeRound(b, 4, g_t.subtext, 1.5f);
    }
    g_c.Text(label, Format(13.5f), D2D1::RectF(r.left + 40, r.top + 6, r.right - 6, r.top + 24), g_t.text);
    g_c.Text(sub, Format(11.5f), D2D1::RectF(r.left + 40, r.top + 23, r.right - 6, r.top + 40), g_t.subtext);
    AddHot(r, key, std::move(click));
}

static float Pill(float x, float y, const std::wstring& text, D2D1_COLOR_F color, bool rightAlign = false) {
    if (text.empty()) return 0;
    auto* f = Format(11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    float w = MeasureWidth(text, f) + 16;
    if (rightAlign) x -= w;
    D2D1_RECT_F r = D2D1::RectF(x, y, x + w, y + 20);
    g_c.FillRound(r, 10, Alpha(color, g_t.dark ? 0.16f : 0.12f));
    g_c.Text(text, f, r, color);
    return w;
}

// ---------------------------------------------------------------- layout helpers
struct Ctx { float left, right, y; };
static Ctx g_x;

static void Title(const std::wstring& title, const std::wstring& sub) {
    g_c.Text(title, Format(22, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(g_x.left, g_x.y, g_x.right, g_x.y + 32), g_t.text);
    if (!sub.empty()) {
        float h = std::max(18.0f, MeasureHeight(sub, Format(12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true), g_x.right - g_x.left));
        g_c.Text(sub, Format(12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true), D2D1::RectF(g_x.left, g_x.y + 34, g_x.right, g_x.y + 34 + h + 2), g_t.subtext);
        g_x.y += 34 + h + 22;
    } else {
        g_x.y += 52;
    }
}

static D2D1_RECT_F Section(const std::wstring& title, float h) {
    if (!title.empty()) {
        g_c.Text(title, Format(12, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(g_x.left, g_x.y, g_x.right, g_x.y + 20), g_t.subtext);
        g_x.y += 26;
    }
    D2D1_RECT_F r = D2D1::RectF(g_x.left, g_x.y, g_x.right, g_x.y + h);
    g_c.FillRound(r, 10, g_t.surface);
    g_x.y += h + 26;
    return r;
}

static void RowLabel(float top, const std::wstring& t, const std::wstring& sub, float rightReserve = 380, float h = 64) {
    if (sub.empty()) {
        g_c.Text(t, Format(14), D2D1::RectF(g_x.left + 18, top, g_x.right - rightReserve, top + h), g_t.text);
        return;
    }
    g_c.Text(t, Format(14), D2D1::RectF(g_x.left + 18, top + 12, g_x.right - rightReserve, top + 32), g_t.text);
    g_c.Text(sub, Format(12), D2D1::RectF(g_x.left + 18, top + 32, g_x.right - rightReserve, top + 50), g_t.subtext);
}

static void Divider(float y) { g_c.Line(g_x.left + 18, y, g_x.right - 18, y, g_t.divider); }

static std::wstring Fmt(double v, int d) {
    wchar_t b[32];
    swprintf(b, 32, L"%.*f", d, v);
    return b;
}

static std::wstring TempText(float c) {
    if (c < 0) return L"—";
    return Fmt(g_cfg.tempF ? c * 9.0 / 5.0 + 32 : c, 0) + (g_cfg.tempF ? L"°F" : L"°C");
}

static std::wstring PlayTime(double s) {
    long long m = (long long)(s / 60);
    if (m < 1) return L"under a minute";
    if (m < 60) return std::to_wstring(m) + L" min";
    return Fmt(s / 3600.0, s < 36000 ? 1 : 0) + L" h";
}

static std::wstring DateText(long long t) {
    if (!t) return L"never";
    time_t tt = (time_t)t;
    tm lt{};
    localtime_s(&lt, &tt);
    static const wchar_t* mon[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
    wchar_t b[64];
    int h = lt.tm_hour % 12 ? lt.tm_hour % 12 : 12;
    swprintf(b, 64, L"%ls %d, %d:%02d %ls", mon[lt.tm_mon], lt.tm_mday, h, lt.tm_min, lt.tm_hour < 12 ? L"AM" : L"PM");
    return b;
}

static bool PickColor(uint32_t& c) {
    static COLORREF custom[16] = {};
    CHOOSECOLORW cc{sizeof(cc)};
    cc.hwndOwner = g_hwnd;
    cc.rgbResult = RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
    cc.lpCustColors = custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&cc)) return false;
    c = (GetRValue(cc.rgbResult) << 16) | (GetGValue(cc.rgbResult) << 8) | GetBValue(cc.rgbResult);
    return true;
}

static OverlayStyle& ScopeStyle() {
    if (!g_scope.empty()) {
        if (GameProfile* p = FindGame(g_scope)) {
            if (!p->ownStyle) { p->ownStyle = true; p->style = g_cfg.style; }
            return p->style;
        }
        g_scope.clear();
    }
    return g_cfg.style;
}
static const OverlayStyle& ScopeStyleView() {
    if (!g_scope.empty()) if (GameProfile* p = FindGame(g_scope)) return p->ownStyle ? p->style : g_cfg.style;
    return g_cfg.style;
}
static void StyleEdited() { ConfigSaveSoon(); app::OverlayChanged(); Invalidate(); }

static void Navigate(Page p);

// ---------------------------------------------------------------- sidebar
static void NavItem(float y, Page p, const std::wstring& label, wchar_t glyph) {
    D2D1_RECT_F r = D2D1::RectF(10, y, kSidebar - 10, y + 36);
    bool sel = g_page == p;
    uint64_t key = K("nav", (int)p);
    bool hot = IsHover(key);
    if (sel) g_c.FillRound(r, 6, g_t.dark ? Rgb(0xFFFFFF, 0.08f) : Rgb(0x000000, 0.06f));
    else if (hot) g_c.FillRound(r, 6, g_t.dark ? Rgb(0xFFFFFF, 0.04f) : Rgb(0x000000, 0.035f));
    if (sel) g_c.FillRound(D2D1::RectF(r.left, y + 10, r.left + 3, y + 26), 1.5f, g_t.accent);
    g_c.Glyph(glyph, Format(15, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true), D2D1::RectF(r.left + 10, y, r.left + 34, y + 36),
              sel ? g_t.accent : g_t.subtext);
    g_c.Text(label, Format(13.5f, sel ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL), D2D1::RectF(r.left + 42, y, r.right - 8, y + 36), g_t.text);
    AddHot(r, key, [p] { Navigate(p); });
}

static void DrawLogo(float x, float y, float s) {
    D2D1_RECT_F logo = D2D1::RectF(x, y, x + s, y + s);
    g_c.FillRound(logo, s * 0.28f, g_t.accent);
    float cx = x + s / 2, cy = y + s / 2, r = s * 0.27f, w = std::max(1.5f, s * 0.07f);
    g_br->SetColor(g_t.onAccent);
    g_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), g_br, w);
    float a = s * 0.36f, g = s * 0.1f;
    g_c.Line(cx - a, cy, cx - g, cy, g_t.onAccent, w);
    g_c.Line(cx + g, cy, cx + a, cy, g_t.onAccent, w);
    g_c.Line(cx, cy - a, cx, cy - g, g_t.onAccent, w);
    g_c.Line(cx, cy + g, cx, cy + a, g_t.onAccent, w);
    g_c.Circle(cx, cy, w * 0.75f, g_t.onAccent);
}

static void DrawSidebar() {
    g_c.Fill(D2D1::RectF(0, 0, kSidebar, g_H), g_t.sidebar);
    g_c.Line(kSidebar - 0.5f, 0, kSidebar - 0.5f, g_H, g_t.divider);
    DrawLogo(22, 22, 28);
    g_c.Text(L"Reticle", Format(17, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(60, 20, kSidebar, 40), g_t.text);
    g_c.Text(L"Overlay & crosshair", Format(11.5f), D2D1::RectF(60, 38, kSidebar, 54), g_t.subtext);
    float y = 76;
    struct { Page p; const wchar_t* label; wchar_t glyph; } items[] = {
        {Page::Home, L"Home", 0xE80F}, {Page::Overlay, L"Overlay", 0xE7F4}, {Page::Crosshair, L"Crosshair", 0xE710},
        {Page::Games, L"Games", icon::Game}, {Page::Benchmarks, L"Benchmarks", 0xE9D9}, {Page::Shortcuts, L"Shortcuts", 0xE765},
    };
    for (auto& it : items) { NavItem(y, it.p, it.label, it.glyph); y += 38; }
    float by = g_H - 82;
    if (y + 48 < by - 38) NavItem(by - 38, Page::Themes, L"Themes", icon::Color);
    if (y + 10 < by) NavItem(by, Page::Settings, L"Settings", icon::Settings);
    std::wstring status;
    if (g_app.upd.kind == SelfUpdateState::Ready) status = L"Update " + g_app.upd.version + L" ready";
    else status = std::wstring(L"Version ") + RETICLE_VERSION + (IsPortable() ? L" · portable" : L"");
    g_c.Text(status, Format(11.5f), D2D1::RectF(24, g_H - 40, kSidebar - 12, g_H - 18), g_t.faint);
}

// ---------------------------------------------------------------- frame capture banner
static std::wstring CaptureStatusText() {
    switch (frames::GetStatus()) {
        case frames::Status::Running: return L"Frame capture is on";
        case frames::Status::NoPermission: return L"Frame capture needs permission";
        case frames::Status::Failed: return L"Frame capture couldn't start (error " + std::to_wstring(frames::LastError()) + L")";
        default: return sys::IsElevated() || sys::InPerfLogUsers() ? L"Frame capture starts when a game is in front" : L"Frame capture needs permission";
    }
}

static bool CaptureNeedsPermission() {
    return !sys::IsElevated() && !sys::InPerfLogUsers();
}

static void PermissionCard() {
    bool pending = g_grantPending;
    float h = 150;
    D2D1_RECT_F r = Section(L"", h);
    g_c.FillRound(r, 10, Alpha(g_t.warn, 0.10f));
    g_c.StrokeRound(r, 10, Alpha(g_t.warn, 0.35f));
    g_c.Glyph(icon::Warning, Format(18, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true), D2D1::RectF(r.left + 14, r.top + 14, r.left + 40, r.top + 40), g_t.warn);
    g_c.Text(pending ? L"Sign out and back in to finish" : L"FPS and frame times need one permission",
             Format(14.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(r.left + 48, r.top + 14, r.right - 16, r.top + 36), g_t.text);
    std::wstring why = pending
        ? L"Your account was added to Performance Log Users. Windows applies it at your next sign-in; until then, frame capture needs admin."
        : L"Reticle reads frame timing from Windows' own graphics events (ETW), the way PresentMon does, so it never touches the game. "
          L"Windows only lets administrators read them. One prompt sets Reticle to start with admin rights at sign-in (no more prompts after that).";
    g_c.Text(why, Format(12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true), D2D1::RectF(r.left + 48, r.top + 40, r.right - 16, r.top + 96), g_t.subtext);
    float x = r.left + 48;
    Button(D2D1::RectF(x, r.bottom - 46, x + 230, r.bottom - 14), pending ? L"Use admin until then" : L"Allow (one prompt)", 0xE7EF, Btn::Primary, K("grant"), [] {
        app::FixPermissions(true);
        ReadSystemState();
        Invalidate();
    });
    x += 240;
    Button(D2D1::RectF(x, r.bottom - 46, x + 210, r.bottom - 14), L"Restart as administrator", 0xE7EF, Btn::Secondary, K("elevate"), [] {
        ConfigFlush();
        if (sys::RestartElevated(L"")) app::Quit();
        else Toast(L"Restart as administrator was canceled.");
    });
}

// ---------------------------------------------------------------- Home
static void StatTile(const D2D1_RECT_F& r, const std::wstring& label, const std::wstring& value, const std::wstring& sub) {
    g_c.FillRound(r, 10, g_t.surface);
    g_c.Text(label, Format(11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(r.left + 16, r.top + 12, r.right - 10, r.top + 28), g_t.subtext);
    g_c.Text(value, Format(24, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(r.left + 16, r.top + 30, r.right - 10, r.top + 62), g_t.text);
    g_c.Text(sub, Format(11.5f), D2D1::RectF(r.left + 16, r.top + 62, r.right - 10, r.bottom - 8), g_t.faint);
}

static void DrawHome() {
    Title(L"Home", L"Reticle shows your overlay and crosshair automatically when a game is in front, and hides them when it isn't.");
    if (CaptureNeedsPermission()) PermissionCard();

    // now playing
    D2D1_RECT_F r = Section(L"NOW", 96);
    GameProfile* p = g_app.gamePid ? FindGame(g_app.game.exe) : nullptr;
    if (p) {
        double secs = g_app.sessionStart ? (GetTickCount64() - g_app.sessionStart) / 1000.0 : 0;
        g_c.Glyph(icon::Game, Format(22, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true), D2D1::RectF(r.left + 16, r.top, r.left + 52, r.bottom), g_t.accent);
        g_c.Text(p->name, Format(16, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(r.left + 62, r.top + 20, r.right - 330, r.top + 44), g_t.text);
        std::wstring sub = L"Playing for " + PlayTime(secs) + L" · " + p->exe;
        if (g_app.exclusiveFullscreen) sub = L"Exclusive fullscreen: switch the game to borderless to see the overlay";
        g_c.Text(sub, Format(12), D2D1::RectF(r.left + 62, r.top + 46, r.right - 330, r.top + 66), g_app.exclusiveFullscreen ? g_t.warn : g_t.subtext);
        float bx = r.right - 18;
        bx = ButtonR(bx, r.top + 32, g_app.benching ? L"Stop benchmark" : L"Benchmark", g_app.benching ? 0xE71A : 0xE768, g_app.benching ? Btn::Danger : Btn::Secondary, K("homebench"),
                     [] { if (g_app.benching) app::StopBenchmark(true); else app::StartBenchmark(); Invalidate(); }) - 8;
        std::wstring exe = p->exe;
        ButtonR(bx, r.top + 32, L"Game settings", icon::Settings, Btn::Subtle, K("homegame"), [] { Navigate(Page::Games); });
    } else {
        g_c.Glyph(icon::Game, Format(22, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true), D2D1::RectF(r.left + 16, r.top, r.left + 52, r.bottom), g_t.faint);
        g_c.Text(L"No game running", Format(16, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(r.left + 62, r.top + 20, r.right - 20, r.top + 44), g_t.text);
        g_c.Text(L"Start a game in borderless or windowed mode. Reticle detects it on its own; you can also add one on the Games page.",
                 Format(12), D2D1::RectF(r.left + 62, r.top + 46, r.right - 20, r.top + 66), g_t.subtext);
    }

    // live readings
    float gap = 12, w = (g_x.right - g_x.left - gap * 3) / 4, y = g_x.y;
    const frames::Live& L = g_app.live;
    const sensors::Reading& S = g_app.sens;
    bool live = g_app.gameForeground && L.valid;
    StatTile(D2D1::RectF(g_x.left, y, g_x.left + w, y + 92), L"FPS", live ? Fmt(L.fps, 0) : L"—",
             live ? L"1% low " + Fmt(L.low1, 0) + L" · " + L.api : (g_app.gamePid ? L"while the game is in front" : L"no game"));
    StatTile(D2D1::RectF(g_x.left + (w + gap), y, g_x.left + w * 2 + gap, y + 92), L"CPU", S.cpuUse < 0 ? L"—" : Fmt(S.cpuUse, 0) + L"%",
             TempText(S.cpuTemp));
    StatTile(D2D1::RectF(g_x.left + (w + gap) * 2, y, g_x.left + w * 3 + gap * 2, y + 92), L"GPU", S.gpuUse < 0 ? L"—" : Fmt(S.gpuUse, 0) + L"%",
             sensors::HasNvidia() ? TempText(S.gpuTemp) + L" · " + (S.gpuPower < 0 ? L"—" : Fmt(S.gpuPower, 0) + L" W") : L"NVIDIA only");
    StatTile(D2D1::RectF(g_x.left + (w + gap) * 3, y, g_x.right, y + 92), L"MEMORY", S.ramUsed < 0 ? L"—" : Fmt(S.ramUsed / 1024, 1) + L" GB",
             S.vramUsed < 0 ? L"of " + Fmt(S.ramTotal / 1024, 0) + L" GB" : L"VRAM " + Fmt(S.vramUsed / 1024, 1) + L" / " + Fmt(S.vramTotal / 1024, 0) + L" GB");
    g_x.y += 92 + 26;

    // quick switches
    D2D1_RECT_F q = Section(L"QUICK SETTINGS", 64 * 3);
    RowLabel(q.top, L"Show the overlay in games", L"Shortcut: " + HotkeyText(g_cfg.hotkeys[HkToggle]) + L". Each game also has its own switch.");
    Toggle(q.right - 18, q.top + 21, g_cfg.overlayOn, K("qoverlay"), [] { g_cfg.overlayOn = !g_cfg.overlayOn; ConfigSaveSoon(); app::OverlayChanged(); Invalidate(); });
    Divider(q.top + 64);
    RowLabel(q.top + 64, p ? L"Crosshair in " + p->name : L"Crosshair", L"Shortcut: " + HotkeyText(g_cfg.hotkeys[HkCrosshair]) + L". On or off per game.");
    if (p) Toggle(q.right - 18, q.top + 64 + 21, p->crosshair, K("qcross"), [] { if (GameProfile* g = FindGame(g_app.game.exe)) { g->crosshair = !g->crosshair; ConfigSaveSoon(); app::CrosshairChanged(); } Invalidate(); });
    else ButtonR(q.right - 18, q.top + 64 + 16, L"Design", 0xE710, Btn::Secondary, K("qcrossd"), [] { Navigate(Page::Crosshair); });
    Divider(q.top + 128);
    RowLabel(q.top + 128, CaptureStatusText(), sys::IsElevated() ? L"Running as administrator" : sys::InPerfLogUsers() ? L"Allowed through Performance Log Users" : L"See the note above");
    Pill(q.right - 18, q.top + 128 + 22, frames::GetStatus() == frames::Status::Running ? L"Active" : CaptureNeedsPermission() ? L"Needs permission" : L"Standing by",
         frames::GetStatus() == frames::Status::Running ? g_t.good : CaptureNeedsPermission() ? g_t.warn : g_t.info, true);

    g_c.Text(L"Works over borderless and windowed games (most modern games). Exclusive fullscreen hides any overlay window, so pick borderless or \"windowed fullscreen\" in the game's settings. "
             L"Reticle never injects into games or hooks DirectX/Vulkan, so it's safe with anti-cheat.",
             Format(12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true), D2D1::RectF(g_x.left, g_x.y - 10, g_x.right, g_x.y + 36), g_t.faint);
    g_x.y += 40;
}

// ---------------------------------------------------------------- Overlay
static std::wstring ScopeName() {
    if (g_scope.empty()) return L"All games (default)";
    GameProfile* p = FindGame(g_scope);
    return p ? p->name : g_scope;
}

static void ScopeMenu(float x, float y) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (g_scope.empty() ? MF_CHECKED : 0), 1, L"All games (default)");
    std::vector<std::wstring> keys;
    std::vector<GameProfile*> list;
    for (auto& kv : g_cfg.games) list.push_back(&kv.second);
    std::sort(list.begin(), list.end(), [](GameProfile* a, GameProfile* b) { return a->lastSeen > b->lastSeen; });
    if (!list.empty()) AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    for (auto* g : list) {
        keys.push_back(g->exe);
        std::wstring label = g->name + (g->ownStyle ? L"  ·  own preset" : L"");
        AppendMenuW(m, MF_STRING | (g_scope == g->exe ? MF_CHECKED : 0), 100 + keys.size() - 1, label.c_str());
    }
    POINT pt{(LONG)(x * g_scale), (LONG)(y * g_scale)};
    ClientToScreen(g_hwnd, &pt);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(m);
    if (cmd == 1) g_scope.clear();
    else if (cmd >= 100 && cmd - 100 < keys.size()) g_scope = keys[cmd - 100];
    else return;
    app::OverlayChanged();
    Invalidate();
}

static void ColorRow(float top, const std::wstring& label, const std::wstring& sub, uint32_t value, const char* name, std::function<void(uint32_t)> set,
                     const std::vector<uint32_t>& presets) {
    RowLabel(top, label, sub, 470);
    float x = g_x.right - 18 - 34;
    uint64_t k = K(name, 999);
    bool custom = std::find(presets.begin(), presets.end(), value) == presets.end();
    Button(D2D1::RectF(x, top + 16, x + 34, top + 48), L"", 0xE790, custom ? Btn::Primary : Btn::Secondary, k, [value, set] {
        uint32_t c = value;
        if (PickColor(c)) set(c);
    });
    x -= 22;
    for (int i = (int)presets.size() - 1; i >= 0; --i) {
        uint32_t c = presets[i];
        Swatch(x, top + 32, c, value == c, K(name, i), [set, c] { set(c); });
        x -= 32;
    }
}

static void DrawOverlay() {
    Title(L"Overlay", L"Your stats in a small panel over the game. Changes show live on screen while this page is open.");
    if (CaptureNeedsPermission()) PermissionCard();
    OverlayStyle st = ScopeStyleView();

    // which preset
    D2D1_RECT_F s0 = Section(L"PRESET", 64 * 2);
    RowLabel(s0.top, L"Editing", g_scope.empty() ? L"Games without their own preset use this" : L"Only this game. Changes you make in-game (like cycling layouts) are saved here too.", 300);
    {
        auto* f = Format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        float w = std::min(280.0f, MeasureWidth(ScopeName(), f) + 60);
        D2D1_RECT_F b = D2D1::RectF(s0.right - 18 - w, s0.top + 16, s0.right - 18, s0.top + 48);
        uint64_t k = K("scope");
        g_c.FillRound(b, 6, IsHover(k) ? g_t.surfaceStrong : Mix(g_t.surface, g_t.surfaceStrong, 0.55f));
        g_c.StrokeRound(b, 6, g_t.divider);
        g_c.Text(ScopeName(), f, D2D1::RectF(b.left + 14, b.top, b.right - 30, b.bottom), g_t.text);
        g_c.Glyph(0xE70D, Format(11, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true), D2D1::RectF(b.right - 30, b.top, b.right - 6, b.bottom), g_t.subtext);
        float bx = b.left, by = b.bottom;
        AddHot(b, k, [bx, by] { ScopeMenu(bx, by + 4); });
    }
    Divider(s0.top + 64);
    RowLabel(s0.top + 64, L"Preview on screen", L"Shows the overlay over this monitor (or your game) while you edit", 420);
    float bx = s0.right - 18 - 44 - 16;
    Toggle(s0.right - 18, s0.top + 64 + 21, g_previewWanted, K("preview"), [] { g_previewWanted = !g_previewWanted; app::PreviewOverlay(g_previewWanted); Invalidate(); });
    bx = ButtonR(bx, s0.top + 64 + 16, overlay::Moving() ? L"Done moving" : L"Move overlay", 0xE7C2, overlay::Moving() ? Btn::Primary : Btn::Secondary, K("move"), [] {
        if (overlay::Moving()) overlay::EndMove();
        else { g_previewWanted = true; app::BeginMoveOverlay(); }
        Invalidate();
    }) - 8;
    if (!g_scope.empty()) {
        GameProfile* gp = FindGame(g_scope);
        if (gp && gp->ownStyle)
            ButtonR(bx, s0.top + 64 + 16, L"Use default", icon::Refresh, Btn::Subtle, K("resetscope"), [] {
                if (GameProfile* g = FindGame(g_scope)) { g->ownStyle = false; StyleEdited(); }
            });
    }

    // layout
    D2D1_RECT_F s1 = Section(L"LAYOUT", 64 * 5);
    RowLabel(s1.top, L"Style", L"Shortcut: " + HotkeyText(g_cfg.hotkeys[HkLayout]) + L" cycles them in-game", 400);
    Segmented(s1.right - 18, s1.top + 16, {L"Full panel", L"Compact line", L"FPS only"}, (int)st.layout, "layout", [](int i) { ScopeStyle().layout = (::Layout)i; StyleEdited(); });
    Divider(s1.top + 64);
    RowLabel(s1.top + 64, L"Position", st.anchor == Anchor::Custom ? L"Where you dragged it" : L"Or drag it anywhere with Move overlay", 470);
    Segmented(s1.right - 18, s1.top + 64 + 16, {L"Top left", L"Top right", L"Bottom left", L"Bottom right", L"Custom"}, (int)st.anchor, "anchor",
              [](int i) { if (i == 4) { g_previewWanted = true; app::BeginMoveOverlay(); Invalidate(); return; } ScopeStyle().anchor = (Anchor)i; StyleEdited(); });
    Divider(s1.top + 128);
    RowLabel(s1.top + 128, L"Text size", L"", 400);
    Slider(s1.right - 380, s1.right - 18, s1.top + 128 + 16, st.textSize, 9, 28, Fmt(st.textSize, 0) + L" pt", K("textsize"),
           [](float v) { ScopeStyle().textSize = std::round(v); StyleEdited(); });
    Divider(s1.top + 192);
    RowLabel(s1.top + 192, L"Background opacity", L"0% is text only, with a soft shadow", 400);
    Slider(s1.right - 380, s1.right - 18, s1.top + 192 + 16, st.opacity, 0, 1, Fmt(st.opacity * 100, 0) + L"%", K("opacity"),
           [](float v) { ScopeStyle().opacity = std::round(v * 20) / 20; StyleEdited(); });
    Divider(s1.top + 256);
    RowLabel(s1.top + 256, L"Update every", L"Faster is smoother; every option stays far under 1% CPU", 400);
    {
        static const int ms[] = {100, 250, 500, 1000};
        int sel = 1;
        for (int i = 0; i < 4; ++i) if (ms[i] == g_cfg.refreshMs) sel = i;
        Segmented(s1.right - 18, s1.top + 256 + 16, {L"0.1 s", L"0.25 s", L"0.5 s", L"1 s"}, sel, "refresh", [](int i) {
            static const int v[] = {100, 250, 500, 1000};
            g_cfg.refreshMs = v[i];
            ConfigSaveSoon();
            if (g_app.gameForeground) SetTimer(g_app.ctl, 1, g_cfg.refreshMs, nullptr);
            Invalidate();
        });
    }

    // stats
    float rowsH = (float)((kStatCount + 1) / 2) * 48 + 16;
    D2D1_RECT_F s2 = Section(L"STATS", rowsH);
    float cw = (s2.right - s2.left - 24) / 2;
    for (int i = 0; i < kStatCount; ++i) {
        int col = i % 2, row = i / 2;
        D2D1_RECT_F r = D2D1::RectF(s2.left + 8 + col * (cw + 8), s2.top + 8 + row * 48, s2.left + 8 + col * (cw + 8) + cw, s2.top + 8 + row * 48 + 46);
        uint32_t bit = 1u << i;
        std::wstring hint = StatHint(i);
        if ((bit & (StatGpuUse | StatGpuTemp | StatGpuClock | StatGpuPower | StatVram)) && !sensors::HasNvidia()) hint += L" · no NVIDIA GPU found";
        if (bit == StatCpuTemp && sensors::CpuTemp() != sensors::CpuTempState::Ok) hint = L"Needs admin and the PawnIO driver (see Settings)";
        Check(r, StatName(i), hint, (st.stats & bit) != 0, K("stat", i), [bit] { ScopeStyle().stats ^= bit; StyleEdited(); });
    }

    // colors
    D2D1_RECT_F s3 = Section(L"COLORS", 64 * 3);
    RowLabel(s3.top, L"Labels", st.followAccent ? L"Following the theme's accent color" : L"Your own color", 470);
    {
        float x = s3.right - 18;
        x = ButtonR(x, s3.top + 16, L"", 0xE790, st.followAccent ? Btn::Secondary : Btn::Primary, K("labelcustom"), [] {
            uint32_t c = ScopeStyle().accentColor;
            if (PickColor(c)) { ScopeStyle().accentColor = c; ScopeStyle().followAccent = false; StyleEdited(); }
        }) - 10;
        static const uint32_t kLabel[] = {0x7AA2FF, 0x5FD08B, 0xFFB020, 0xFF7AA8, 0x3CC8C8};
        for (int i = 4; i >= 0; --i) {
            uint32_t c = kLabel[i];
            x -= 16;
            Swatch(x, s3.top + 32, c, !st.followAccent && st.accentColor == c, K("labelsw", i), [c] { ScopeStyle().accentColor = c; ScopeStyle().followAccent = false; StyleEdited(); });
            x -= 18;
        }
        x -= 8;
        auto* f = Format(12.5f, st.followAccent ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL);
        float w = MeasureWidth(L"Theme accent", f) + 42;
        D2D1_RECT_F chip = D2D1::RectF(x - w, s3.top + 16, x, s3.top + 48);
        uint64_t k = K("labeltheme");
        g_c.FillRound(chip, 16, st.followAccent ? Alpha(g_t.accent, 0.18f) : (IsHover(k) ? g_t.surfaceHover : Mix(g_t.surface, g_t.surfaceStrong, 0.4f)));
        if (st.followAccent) g_c.StrokeRound(chip, 16, g_t.accent, 1.5f);
        g_c.Circle(chip.left + 16, s3.top + 32, 7, g_app.accent);
        g_c.Text(L"Theme accent", f, D2D1::RectF(chip.left + 29, chip.top, chip.right - 8, chip.bottom), g_t.text);
        AddHot(chip, k, [] { ScopeStyle().followAccent = true; StyleEdited(); });
    }
    Divider(s3.top + 64);
    ColorRow(s3.top + 64, L"Numbers", L"", st.textColor, "textcol", [](uint32_t c) { ScopeStyle().textColor = c; StyleEdited(); },
             {0xF5F5F7, 0xFFE8A3, 0xB8F5C8, 0xA8D8FF});
    Divider(s3.top + 128);
    ColorRow(s3.top + 128, L"Background", L"", st.bgColor, "bgcol", [](uint32_t c) { ScopeStyle().bgColor = c; StyleEdited(); },
             {0x0E0E12, 0x000000, 0x1B2230, 0x24182A});

    // units
    D2D1_RECT_F s4 = Section(L"UNITS AND BEHAVIOR", 64 * 3);
    RowLabel(s4.top, L"Temperature", L"");
    Segmented(s4.right - 18, s4.top + 16, {L"°C", L"°F"}, g_cfg.tempF ? 1 : 0, "tempunit", [](int i) { g_cfg.tempF = i == 1; ConfigSaveSoon(); app::OverlayChanged(); Invalidate(); });
    Divider(s4.top + 64);
    RowLabel(s4.top + 64, L"Clock", L"");
    Segmented(s4.right - 18, s4.top + 64 + 16, {L"12-hour", L"24-hour"}, g_cfg.clock24 ? 1 : 0, "clock", [](int i) { g_cfg.clock24 = i == 1; ConfigSaveSoon(); app::OverlayChanged(); Invalidate(); });
    Divider(s4.top + 128);
    RowLabel(s4.top + 128, L"Show the overlay without frame data", L"When frame capture isn't allowed, still show CPU, GPU and the rest");
    Toggle(s4.right - 18, s4.top + 128 + 21, g_cfg.showWithoutFrames, K("noframes"), [] { g_cfg.showWithoutFrames = !g_cfg.showWithoutFrames; ConfigSaveSoon(); Invalidate(); });

    g_c.Text(L"The overlay is a separate see-through window on top of the game. It works over borderless and windowed games; exclusive fullscreen hides it.",
             Format(12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true), D2D1::RectF(g_x.left, g_x.y - 10, g_x.right, g_x.y + 24), g_t.faint);
    g_x.y += 28;
}

// ---------------------------------------------------------------- Crosshair
static void LoadPreviewImage() {
    const std::wstring& path = g_cfg.cross.image;
    if (g_crossImg && path == g_crossImgPath) return;
    SafeRelease(g_crossImg);
    g_crossImgPath = path;
    if (path.empty() || !g_rt) return;
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    if (SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec)) && SUCCEEDED(dec->GetFrame(0, &frame)) &&
        SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        g_rt->CreateBitmapFromWicBitmap(conv, nullptr, &g_crossImg);
    SafeRelease(conv); SafeRelease(frame); SafeRelease(dec); SafeRelease(wic);
}

static void DrawCrosshairPreview(const D2D1_RECT_F& r) {
    // a game-ish backdrop: sky, horizon, ground
    ID2D1GradientStopCollection* stops = nullptr;
    D2D1_GRADIENT_STOP gs[3] = {{0, Rgb(0x6C8FB8)}, {0.55f, Rgb(0xC9B79A)}, {0.56f, Rgb(0x5B5A4C)}};
    g_rt->CreateGradientStopCollection(gs, 3, &stops);
    ID2D1LinearGradientBrush* lg = nullptr;
    if (stops) g_rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.left, r.top), D2D1::Point2F(r.left, r.bottom)), stops, &lg);
    if (lg) { g_rt->FillRoundedRectangle(D2D1::RoundedRect(r, 10, 10), lg); lg->Release(); }
    if (stops) stops->Release();
    const CrosshairStyle& x = g_cfg.cross;
    float cx = std::floor((r.left + r.right) / 2) + 0.5f, cy = std::floor((r.top + r.bottom) / 2) + 0.5f;
    float k = 2.0f;   // shown at 2x so the details are visible
    float t = std::max(1.0f, x.thickness) * k, len = x.size * k, gap = x.gap * k, ow = x.outline ? x.outlineWidth * k : 0;
    D2D1_COLOR_F col = Rgb(x.color, x.opacity), oc = Rgb(x.outlineColor, x.opacity);
    auto bar = [&](float l, float tp, float rr, float b) {
        if (ow > 0) g_c.Fill(D2D1::RectF(l - ow, tp - ow, rr + ow, b + ow), oc);
        g_c.Fill(D2D1::RectF(l, tp, rr, b), col);
    };
    float h0 = cx - t / 2, h1 = cx + t / 2, v0 = cy - t / 2, v1 = cy + t / 2;
    switch (x.shape) {
        case CrossShape::Dot: {
            float rad = std::max(1.0f, x.size * 0.3f) * k;
            if (ow > 0) g_c.Circle(cx, cy, rad + ow, oc);
            g_c.Circle(cx, cy, rad, col);
            break;
        }
        case CrossShape::Cross:
        case CrossShape::T:
            bar(h0 - gap - len, v0, h0 - gap, v1);
            bar(h1 + gap, v0, h1 + gap + len, v1);
            bar(h0, v1 + gap, h1, v1 + gap + len);
            if (x.shape == CrossShape::Cross) bar(h0, v0 - gap - len, h1, v0 - gap);
            break;
        case CrossShape::Circle: {
            float rad = len + gap;
            if (ow > 0) { g_br->SetColor(oc); g_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad), g_br, t + ow * 2); }
            g_br->SetColor(col);
            g_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad), g_br, t);
            break;
        }
        case CrossShape::Image:
            LoadPreviewImage();
            if (g_crossImg) {
                D2D1_SIZE_F bs = g_crossImg->GetSize();
                float w = bs.width * x.imageScale * k, h = bs.height * x.imageScale * k;
                g_rt->DrawBitmap(g_crossImg, D2D1::RectF(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2), x.opacity);
            } else {
                g_c.Text(L"Import a PNG", Format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER), r, Rgb(0xFFFFFF, 0.8f));
            }
            break;
    }
    if (x.centerDot && x.shape != CrossShape::Dot && x.shape != CrossShape::Image) bar(cx - t / 2, cy - t / 2, cx + t / 2, cy + t / 2);
    g_c.Text(L"2× preview", Format(11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING), D2D1::RectF(r.left, r.bottom - 26, r.right - 12, r.bottom - 6), Rgb(0xFFFFFF, 0.75f));
}

static void CrossEdited() { ConfigSaveSoon(); app::CrosshairChanged(); Invalidate(); }

static void DrawCrosshair() {
    Title(L"Crosshair", L"An optional crosshair centered on the game window. Turn it on per game on the Games page, or with " +
                       HotkeyText(g_cfg.hotkeys[HkCrosshair]) + L" while playing.");
    CrosshairStyle& x = g_cfg.cross;
    D2D1_RECT_F pv = D2D1::RectF(g_x.left, g_x.y, g_x.right, g_x.y + 200);
    DrawCrosshairPreview(pv);
    g_x.y += 200 + 26;

    D2D1_RECT_F s1 = Section(L"SHAPE", 64);
    RowLabel(s1.top, L"Style", L"");
    Segmented(s1.right - 18, s1.top + 16, {L"Dot", L"Cross", L"Circle", L"T", L"Image"}, (int)x.shape, "shape", [](int i) {
        if (i == 4 && g_cfg.cross.image.empty()) {
            wchar_t file[MAX_PATH] = {};
            OPENFILENAMEW of{sizeof(of)};
            of.hwndOwner = g_hwnd;
            of.lpstrFilter = L"PNG images\0*.png\0";
            of.lpstrFile = file;
            of.nMaxFile = MAX_PATH;
            of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (!GetOpenFileNameW(&of)) return;
            g_cfg.cross.image = file;
        }
        g_cfg.cross.shape = (CrossShape)i;
        CrossEdited();
    });

    bool img = x.shape == CrossShape::Image;
    D2D1_RECT_F s2 = Section(L"SIZE", img ? 64 * 2 : 64 * 4);
    if (img) {
        RowLabel(s2.top, L"Image", PathName(x.image), 300);
        ButtonR(s2.right - 18, s2.top + 16, L"Choose PNG…", icon::Folder, Btn::Secondary, K("png"), [] {
            wchar_t file[MAX_PATH] = {};
            OPENFILENAMEW of{sizeof(of)};
            of.hwndOwner = g_hwnd;
            of.lpstrFilter = L"PNG images\0*.png\0";
            of.lpstrFile = file;
            of.nMaxFile = MAX_PATH;
            of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameW(&of)) { g_cfg.cross.image = file; CrossEdited(); }
        });
        Divider(s2.top + 64);
        RowLabel(s2.top + 64, L"Scale", L"", 400);
        Slider(s2.right - 380, s2.right - 18, s2.top + 64 + 16, x.imageScale, 0.25f, 3, Fmt(x.imageScale * 100, 0) + L"%", K("imgscale"),
               [](float v) { g_cfg.cross.imageScale = std::round(v * 20) / 20; CrossEdited(); });
    } else {
        struct { const wchar_t* label; float* v; float mn, mx; const char* key; } rows[] = {
            {x.shape == CrossShape::Dot ? L"Dot size" : x.shape == CrossShape::Circle ? L"Radius" : L"Length", &x.size, 1, 40, "csize"},
            {L"Gap", &x.gap, 0, 20, "cgap"},
            {L"Thickness", &x.thickness, 1, 8, "cthick"},
        };
        for (int i = 0; i < 3; ++i) {
            float top = s2.top + i * 64;
            if (i) Divider(top);
            RowLabel(top, rows[i].label, L"", 400);
            float* v = rows[i].v;
            Slider(s2.right - 380, s2.right - 18, top + 16, *v, rows[i].mn, rows[i].mx, Fmt(*v, 0) + L" px", K(rows[i].key),
                   [v](float nv) { *v = std::round(nv); CrossEdited(); });
        }
        Divider(s2.top + 192);
        RowLabel(s2.top + 192, L"Center dot", L"");
        Toggle(s2.right - 18, s2.top + 192 + 21, x.centerDot, K("cdot"), [] { g_cfg.cross.centerDot = !g_cfg.cross.centerDot; CrossEdited(); });
    }

    D2D1_RECT_F s3 = Section(L"LOOK", 64 * (img ? 1 : 4));
    float top = s3.top;
    if (!img) {
        ColorRow(top, L"Color", L"", x.color, "ccol", [](uint32_t c) { g_cfg.cross.color = c; CrossEdited(); },
                 {0x3CFF6A, 0x00FFFF, 0xFF3CF0, 0xFFFF3C, 0xFF3C3C, 0xFFFFFF});
        Divider(top + 64);
        RowLabel(top + 64, L"Outline", L"Keeps it visible on bright and dark scenes");
        Toggle(s3.right - 18, top + 64 + 21, x.outline, K("coutline"), [] { g_cfg.cross.outline = !g_cfg.cross.outline; CrossEdited(); });
        Divider(top + 128);
        RowLabel(top + 128, L"Outline thickness", L"", 400);
        Slider(s3.right - 380, s3.right - 18, top + 128 + 16, x.outlineWidth, 0.5f, 3, Fmt(x.outlineWidth, 1) + L" px", K("cow"),
               [](float v) { g_cfg.cross.outlineWidth = std::round(v * 2) / 2; CrossEdited(); });
        top += 192;
        Divider(top);
    }
    RowLabel(top, L"Opacity", L"", 400);
    Slider(s3.right - 380, s3.right - 18, top + 16, x.opacity, 0.1f, 1, Fmt(x.opacity * 100, 0) + L"%", K("copac"),
           [](float v) { g_cfg.cross.opacity = std::round(v * 20) / 20; CrossEdited(); });

    D2D1_RECT_F s4 = Section(L"GAMES", 64);
    RowLabel(s4.top, L"Turn it on for new games", L"Games Reticle finds from now on start with the crosshair on");
    Toggle(s4.right - 18, s4.top + 21, g_cfg.crosshairDefault, K("cdefault"), [] { g_cfg.crosshairDefault = !g_cfg.crosshairDefault; ConfigSaveSoon(); Invalidate(); });
}

// ---------------------------------------------------------------- Games
static void AddGameFromFile() {
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW of{sizeof(of)};
    of.hwndOwner = g_hwnd;
    of.lpstrFilter = L"Programs\0*.exe\0";
    of.lpstrFile = file;
    of.nMaxFile = MAX_PATH;
    of.lpstrTitle = L"Choose the game's .exe";
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&of)) return;
    games::Window w;
    w.path = file;
    w.exe = Lower(PathName(file));
    GameProfile& p = EnsureGame(w.exe, games::FriendlyName(w), w.path);
    p.detect = Detect::Always;
    ConfigSaveSoon();
    Toast(p.name + L" added. Reticle will treat it as a game.");
}

static void DrawGames() {
    Title(L"Games", L"Games Reticle has found. Each one remembers its overlay, crosshair and preset.");
    std::vector<GameProfile*> list;
    for (auto& kv : g_cfg.games) list.push_back(&kv.second);
    std::sort(list.begin(), list.end(), [](GameProfile* a, GameProfile* b) { return a->lastSeen > b->lastSeen; });
    ButtonR(g_x.right, g_x.y - 54, L"Add game…", 0xE710, Btn::Secondary, K("addgame"), AddGameFromFile);
    if (list.empty()) {
        D2D1_RECT_F r = Section(L"", 96);
        g_c.Text(L"No games yet", Format(15, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(r.left + 20, r.top + 22, r.right - 20, r.top + 44), g_t.text);
        g_c.Text(L"Start any game and it shows up here. If Reticle misses one, use Add game.", Format(12.5f), D2D1::RectF(r.left + 20, r.top + 46, r.right - 20, r.top + 70), g_t.subtext);
        return;
    }
    float rowH = 112;
    D2D1_RECT_F card = Section(L"", rowH * list.size());
    for (size_t i = 0; i < list.size(); ++i) {
        GameProfile* p = list[i];
        float top = card.top + i * rowH;
        if (i) Divider(top);
        bool running = g_app.gamePid && p->exe == g_app.game.exe;
        g_c.Text(p->name, Format(14.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(card.left + 18, top + 12, card.right - 420, top + 34), g_t.text);
        float px = card.left + 18 + std::min(MeasureWidth(p->name, Format(14.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD)), card.right - card.left - 460) + 10;
        if (running) px += Pill(px, top + 13, L"Playing now", g_t.good) + 6;
        if (p->ownStyle) Pill(px, top + 13, L"Own preset", g_t.info);
        g_c.Text(p->exe + L" · last played " + DateText(p->lastSeen) + L" · " + PlayTime(p->playSeconds) + L" in total",
                 Format(12), D2D1::RectF(card.left + 18, top + 34, card.right - 420, top + 52), g_t.subtext);
        std::wstring exe = p->exe;
        // toggles
        float tx = card.right - 18;
        g_c.Text(L"Overlay", Format(12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING), D2D1::RectF(tx - 200, top + 12, tx - 52, top + 36), g_t.subtext);
        Toggle(tx, top + 13, p->overlay, K("gov", (int)i), [exe] { if (GameProfile* g = FindGame(exe)) { g->overlay = !g->overlay; ConfigSaveSoon(); app::OverlayChanged(); } Invalidate(); });
        g_c.Text(L"Crosshair", Format(12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING), D2D1::RectF(tx - 200, top + 42, tx - 52, top + 66), g_t.subtext);
        Toggle(tx, top + 43, p->crosshair, K("gcr", (int)i), [exe] { if (GameProfile* g = FindGame(exe)) { g->crosshair = !g->crosshair; ConfigSaveSoon(); app::CrosshairChanged(); } Invalidate(); });
        // detection + actions
        float by = top + rowH - 44;
        g_c.Text(L"Treat as a game", Format(12.5f), D2D1::RectF(card.left + 18, by, card.left + 140, by + 32), g_t.subtext);
        float segR = card.left + 140 + SegmentedWidth({L"Auto", L"Always", L"Never"});
        Segmented(segR, by, {L"Auto", L"Always", L"Never"}, (int)p->detect, ("gdet" + std::to_string(i)).c_str(), [exe](int d) {
            if (GameProfile* g = FindGame(exe)) { g->detect = (Detect)d; ConfigSaveSoon(); PostMessageW(g_app.ctl, WM_RETICLE_FOREGROUND, 0, 0); }
            Invalidate();
        });
        float bx = card.right - 18;
        bx = ButtonR(bx, by, L"", icon::Delete, Btn::Subtle, K("gdel", (int)i), [exe] {
            if (g_app.gamePid && g_app.game.exe == exe) { Toast(L"Close the game first."); return; }
            if (g_scope == exe) g_scope.clear();
            g_cfg.games.erase(exe);
            ConfigSaveSoon();
            Invalidate();
        }) - 6;
        ButtonR(bx, by, L"Overlay preset", 0xE7F4, Btn::Subtle, K("gpreset", (int)i), [exe] { g_scope = exe; Navigate(Page::Overlay); });
    }
}

// ---------------------------------------------------------------- Benchmarks
static void LoadRuns() {
    g_runs = bench::List();
    g_runsLoaded = true;
    if (g_sel >= (int)g_runs.size()) g_sel = -1;
    if (g_cmp >= (int)g_runs.size()) g_cmp = -1;
    if (g_sel < 0 && !g_runs.empty()) g_sel = 0;
    g_selFrames.clear();
    g_cmpFrames.clear();
    if (g_sel >= 0) bench::LoadFrames(g_runs[g_sel], g_selFrames);
    if (g_cmp >= 0) bench::LoadFrames(g_runs[g_cmp], g_cmpFrames);
}

void OnBenchmarksChanged() {
    g_runsLoaded = false;
    g_sel = 0;
    g_cmp = -1;
    Invalidate();
}

static void DrawRunGraph(const D2D1_RECT_F& r, const std::vector<float>& a, const std::vector<float>& b) {
    g_c.FillRound(r, 8, Mix(g_t.surface, g_t.bg, 0.5f));
    if (a.empty()) return;
    // plot FPS over time, bucketed to the width (worst frame per bucket keeps stutter visible)
    auto series = [&](const std::vector<float>& f, size_t buckets, std::vector<float>& out) {
        out.clear();
        size_t n = f.size();
        for (size_t k = 0; k < buckets; ++k) {
            size_t s = k * n / buckets, e = std::max(s + 1, (k + 1) * n / buckets);
            float mx = 0;
            for (size_t i = s; i < e && i < n; ++i) mx = std::max(mx, f[i]);
            out.push_back(mx);
        }
    };
    size_t buckets = (size_t)std::max(20.0f, (r.right - r.left - 60) / 2);
    std::vector<float> sa, sb;
    series(a, std::min(buckets, a.size()), sa);
    if (!b.empty()) series(b, std::min(buckets, b.size()), sb);
    std::vector<float> all(sa);
    all.insert(all.end(), sb.begin(), sb.end());
    std::sort(all.begin(), all.end());
    float top = std::max(8.0f, all[(size_t)(all.size() * 0.995)] * 1.15f);
    float l = r.left + 50, rt = r.right - 12, t = r.top + 14, bt = r.bottom - 24;
    // grid: frame time labels
    for (int g = 0; g <= 2; ++g) {
        float v = top * g / 2, y = bt - (bt - t) * g / 2;
        g_c.Line(l, y, rt, y, g_t.divider);
        g_c.Text(Fmt(v, 0) + L" ms", Format(11, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING), D2D1::RectF(r.left, y - 9, l - 8, y + 9), g_t.faint);
    }
    auto plot = [&](const std::vector<float>& s, D2D1_COLOR_F col) {
        if (s.size() < 2) return;
        ID2D1PathGeometry* geo = nullptr;
        if (FAILED(d2d->CreatePathGeometry(&geo))) return;
        ID2D1GeometrySink* sink = nullptr;
        if (SUCCEEDED(geo->Open(&sink))) {
            float step = (rt - l) / (s.size() - 1);
            sink->BeginFigure(D2D1::Point2F(l, bt - std::min(1.0f, s[0] / top) * (bt - t)), D2D1_FIGURE_BEGIN_HOLLOW);
            for (size_t i = 1; i < s.size(); ++i) sink->AddLine(D2D1::Point2F(l + i * step, bt - std::min(1.0f, s[i] / top) * (bt - t)));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            sink->Release();
            g_br->SetColor(col);
            g_rt->DrawGeometry(geo, g_br, 1.5f);
        }
        geo->Release();
    };
    if (!sb.empty()) plot(sb, Rgb(0xF2B84B, 0.9f));
    plot(sa, g_t.accent);
    g_c.Text(L"Frame time over the run (worst frame per slice)", Format(11), D2D1::RectF(l, bt + 4, rt, r.bottom - 2), g_t.faint);
}

static void DrawBenchmarks() {
    Title(L"Benchmarks", L"Press " + HotkeyText(g_cfg.hotkeys[HkBench]) + L" in a game to record a run, and again to stop. Runs are saved per game.");
    if (!g_runsLoaded) LoadRuns();
    D2D1_RECT_F s0 = Section(L"RECORDING", 64);
    RowLabel(s0.top, L"Stop automatically after", L"Or whenever you press the shortcut again", 470);
    {
        static const int secs[] = {30, 60, 120, 300, 0};
        int sel = 1;
        for (int i = 0; i < 5; ++i) if (secs[i] == g_cfg.benchSeconds) sel = i;
        Segmented(s0.right - 18, s0.top + 16, {L"30 s", L"60 s", L"2 min", L"5 min", L"Manual"}, sel, "benchlen", [](int i) {
            static const int v[] = {30, 60, 120, 300, 0};
            g_cfg.benchSeconds = v[i];
            ConfigSaveSoon();
            Invalidate();
        });
    }
    if (g_runs.empty()) {
        D2D1_RECT_F r = Section(L"RUNS", 90);
        g_c.Text(L"No runs yet", Format(15, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(r.left + 20, r.top + 20, r.right - 20, r.top + 42), g_t.text);
        g_c.Text(L"Start a game, press " + HotkeyText(g_cfg.hotkeys[HkBench]) + L", play for a bit, then press it again.", Format(12.5f),
                 D2D1::RectF(r.left + 20, r.top + 44, r.right - 20, r.top + 66), g_t.subtext);
        return;
    }
    // selected run (and comparison)
    if (g_sel >= 0) {
        const bench::Run& a = g_runs[g_sel];
        const bench::Run* b = g_cmp >= 0 && g_cmp != g_sel ? &g_runs[g_cmp] : nullptr;
        float h = 56 + 4 * 30 + 16 + 220 + 56;
        D2D1_RECT_F r = Section(b ? L"COMPARE" : L"RUN", h);
        g_c.Text(a.game, Format(16, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(r.left + 18, r.top + 14, r.right - 300, r.top + 38), g_t.text);
        g_c.Text(DateText(a.when) + (a.gpu.empty() ? L"" : L" · " + a.gpu), Format(12), D2D1::RectF(r.left + 18, r.top + 36, r.right - 300, r.top + 54), g_t.subtext);
        // table: metric | A | B | difference
        float tx = r.left + 18, ty = r.top + 64;
        float c1 = tx + 150, c2 = c1 + 120, c3 = c2 + 120;
        auto hdr = Format(11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        g_c.Circle(c1 + 6, ty + 9, 5, g_t.accent);
        g_c.Text(L"This run", hdr, D2D1::RectF(c1 + 16, ty, c2, ty + 18), g_t.subtext);
        if (b) {
            g_c.Circle(c2 + 6, ty + 9, 5, Rgb(0xF2B84B));
            g_c.Text(DateText(b->when), hdr, D2D1::RectF(c2 + 16, ty, c3, ty + 18), g_t.subtext);
            g_c.Text(L"Difference", hdr, D2D1::RectF(c3, ty, c3 + 120, ty + 18), g_t.subtext);
        }
        ty += 24;
        struct { const wchar_t* name; double va, vb; int d; bool higherBetter; } m[] = {
            {L"Average FPS", a.s.avgFps, b ? b->s.avgFps : 0, 1, true},
            {L"1% low", a.s.low1, b ? b->s.low1 : 0, 1, true},
            {L"0.1% low", a.s.low01, b ? b->s.low01 : 0, 1, true},
            {L"Length", a.s.seconds, b ? b->s.seconds : 0, 0, true},
        };
        for (int i = 0; i < 4; ++i) {
            g_c.Text(m[i].name, Format(13), D2D1::RectF(tx, ty, c1, ty + 26), g_t.subtext);
            std::wstring va = i == 3 ? Fmt(m[i].va, 0) + L" s" : Fmt(m[i].va, m[i].d);
            g_c.Text(va, Format(15, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(c1 + 16, ty, c2, ty + 26), g_t.text);
            if (b) {
                std::wstring vb = i == 3 ? Fmt(m[i].vb, 0) + L" s" : Fmt(m[i].vb, m[i].d);
                g_c.Text(vb, Format(15, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(c2 + 16, ty, c3, ty + 26), g_t.text);
                if (i < 3 && m[i].vb > 0) {
                    double pct = (m[i].va - m[i].vb) / m[i].vb * 100;
                    bool better = pct >= 0;
                    g_c.Text((pct >= 0 ? L"+" : L"") + Fmt(pct, 1) + L"%", Format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(c3, ty, c3 + 120, ty + 26),
                             std::fabs(pct) < 1 ? g_t.subtext : better ? g_t.good : g_t.bad);
                }
            }
            ty += 30;
        }
        // side stats (beside the table when there's room)
        float sx = b ? std::max(r.right - 260, c3 + 140) : std::max(r.right - 260, c2 + 40);
        auto side = [&](float y, const std::wstring& k, const std::wstring& v) {
            g_c.Text(k, Format(12), D2D1::RectF(sx, y, sx + 120, y + 20), g_t.subtext);
            g_c.Text(v, Format(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(sx + 120, y, r.right - 18, y + 20), g_t.text);
        };
        side(r.top + 88, L"Frames", std::to_wstring(a.s.frames));
        side(r.top + 110, L"Min / max FPS", Fmt(a.s.minFps, 0) + L" / " + Fmt(a.s.maxFps, 0));
        side(r.top + 132, L"Avg CPU / GPU", (a.avgCpu < 0 ? L"—" : Fmt(a.avgCpu, 0) + L"%") + L" / " + (a.avgGpu < 0 ? L"—" : Fmt(a.avgGpu, 0) + L"%"));
        side(r.top + 154, L"Peak temps", TempText(a.maxCpuTemp) + L" / " + TempText(a.maxGpuTemp));
        D2D1_RECT_F gr = D2D1::RectF(r.left + 18, ty + 12, r.right - 18, ty + 12 + 220);
        DrawRunGraph(gr, g_selFrames, b ? g_cmpFrames : std::vector<float>());
        float by = r.bottom - 46, bx = r.right - 18;
        bench::Run run = a;
        bx = ButtonR(bx, by, L"Delete", icon::Delete, Btn::Danger, K("bdel"), [run] {
            if (MessageBoxW(g_hwnd, L"Delete this benchmark run?", L"Reticle", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
            bench::Delete(run);
            OnBenchmarksChanged();
        }) - 8;
        bx = ButtonR(bx, by, L"Export CSV", icon::Download, Btn::Secondary, K("bcsv"), [run] {
            wchar_t file[MAX_PATH];
            std::wstring name = run.exe + L" " + run.id + L".csv";
            wcsncpy(file, name.c_str(), MAX_PATH - 1);
            file[MAX_PATH - 1] = 0;
            OPENFILENAMEW of{sizeof(of)};
            of.hwndOwner = g_hwnd;
            of.lpstrFilter = L"CSV files\0*.csv\0";
            of.lpstrFile = file;
            of.nMaxFile = MAX_PATH;
            of.lpstrDefExt = L"csv";
            of.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
            if (GetSaveFileNameW(&of)) Toast(bench::ExportCsv(run, file) ? L"Saved " + PathName(file) : L"Couldn't save the CSV.");
        }) - 8;
        bx = ButtonR(bx, by, L"Open folder", icon::Folder, Btn::Subtle, K("bfolder"), [run] {
            EnsureDir(run.dir);
            ShellExecuteW(g_hwnd, L"open", run.dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }) - 8;
        if (b) ButtonR(bx, by, L"Stop comparing", icon::Cancel, Btn::Subtle, K("bnocmp"), [] { g_cmp = -1; g_cmpFrames.clear(); Invalidate(); });
        else g_c.Text(L"Tip: click Compare on another run to put them side by side.", Format(12), D2D1::RectF(r.left + 18, by, bx - 10, by + 32), g_t.faint);
    }
    // list
    float rowH = 56;
    D2D1_RECT_F list = Section(L"ALL RUNS", rowH * g_runs.size());
    for (size_t i = 0; i < g_runs.size(); ++i) {
        const bench::Run& r = g_runs[i];
        float top = list.top + i * rowH;
        D2D1_RECT_F row = D2D1::RectF(list.left + 4, top + 4, list.right - 4, top + rowH - 4);
        uint64_t k = K("brow", (int)i);
        bool sel = (int)i == g_sel, cmp = (int)i == g_cmp;
        if (sel) g_c.FillRound(row, 8, Alpha(g_t.accent, 0.14f));
        else if (cmp) g_c.FillRound(row, 8, Rgb(0xF2B84B, 0.12f));
        else if (IsHover(k)) g_c.FillRound(row, 8, g_t.surfaceHover);
        float cx = std::max(row.left + 200, row.right - 130 - 300);
        g_c.Text(r.game, Format(13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(row.left + 14, row.top + 6, cx - 16, row.top + 26), g_t.text);
        g_c.Text(DateText(r.when) + L" · " + Fmt(r.s.seconds, 0) + L" s", Format(12), D2D1::RectF(row.left + 14, row.top + 26, cx - 16, row.top + 44), g_t.subtext);
        auto col = [&](const std::wstring& label, const std::wstring& v) {
            g_c.Text(label, Format(11), D2D1::RectF(cx, row.top + 6, cx + 100, row.top + 22), g_t.faint);
            g_c.Text(v, Format(14, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(cx, row.top + 21, cx + 100, row.top + 44), g_t.text);
            cx += 100;
        };
        col(L"AVG FPS", Fmt(r.s.avgFps, 0));
        col(L"1% LOW", Fmt(r.s.low1, 0));
        col(L"0.1% LOW", Fmt(r.s.low01, 0));
        int idx = (int)i;
        AddHot(row, k, [idx] {
            g_sel = idx;
            if (g_cmp == idx) g_cmp = -1;
            g_selFrames.clear();
            bench::LoadFrames(g_runs[idx], g_selFrames);
            Invalidate();
        });
        if (!sel) {
            ButtonR(row.right - 10, row.top + 8, cmp ? L"Comparing" : L"Compare", 0xE8F9, cmp ? Btn::Primary : Btn::Secondary, K("bcmp", idx), [idx] {
                g_cmp = g_cmp == idx ? -1 : idx;
                g_cmpFrames.clear();
                if (g_cmp >= 0) bench::LoadFrames(g_runs[g_cmp], g_cmpFrames);
                Invalidate();
            });
        }
    }
}

// ---------------------------------------------------------------- Shortcuts
static void DrawShortcuts() {
    Title(L"Shortcuts", L"System-wide shortcuts that work while you play. Click Change, then press the new combination.");
    struct { HotkeyId id; const wchar_t* name; const wchar_t* sub; } items[] = {
        {HkToggle, L"Show or hide the overlay", L"Remembered per game"},
        {HkLayout, L"Cycle layouts", L"Full panel, compact line, FPS only"},
        {HkBench, L"Start or stop a benchmark", L"Results appear on the overlay and on the Benchmarks page"},
        {HkCrosshair, L"Crosshair on or off", L"For the game in front"},
    };
    D2D1_RECT_F r = Section(L"", 64 * 4);
    for (int i = 0; i < 4; ++i) {
        float top = r.top + i * 64;
        if (i) Divider(top);
        HotkeyId id = items[i].id;
        bool taken = false;
        for (auto& e : g_app.hotkeyErrors) taken |= e == HotkeyText(g_cfg.hotkeys[id]);
        RowLabel(top, items[i].name, taken ? L"Another app already uses this shortcut. Pick a different one." : items[i].sub, 420);
        float x = r.right - 18;
        x = ButtonR(x, top + 16, L"", icon::Cancel, Btn::Subtle, K("hkclear", i), [id] { g_cfg.hotkeys[id] = {}; ConfigSaveSoon(); app::ApplyHotkeys(); Invalidate(); }) - 8;
        bool rec = g_capture == id;
        x = ButtonR(x, top + 16, rec ? L"Press keys… (Esc cancels)" : L"Change", 0, rec ? Btn::Primary : Btn::Secondary, K("hkset", i), [id] {
            g_capture = g_capture == id ? 0 : id;
            if (g_capture) for (int k = 1; k < HkCount; ++k) UnregisterHotKey(g_app.ctl, k);   // so the keys reach this window
            else app::ApplyHotkeys();
            SetFocus(g_hwnd);
            Invalidate();
        }) - 12;
        auto* f = Format(13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
        std::wstring t = HotkeyText(g_cfg.hotkeys[id]);
        float w = MeasureWidth(t, f) + 24;
        D2D1_RECT_F kb = D2D1::RectF(x - w, top + 18, x, top + 46);
        g_c.FillRound(kb, 6, Mix(g_t.surface, g_t.surfaceStrong, 0.7f));
        g_c.StrokeRound(kb, 6, taken ? g_t.bad : g_t.divider);
        g_c.Text(t, f, kb, taken ? g_t.bad : g_t.text);
    }
    g_c.Text(L"Ctrl+, opens Settings while Reticle's window is in front.", Format(12), D2D1::RectF(g_x.left, g_x.y - 10, g_x.right, g_x.y + 10), g_t.faint);
    g_x.y += 20;
}

static void OnCaptureKey(UINT vk) {
    if (!g_capture) return;
    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN || vk == VK_LSHIFT || vk == VK_RSHIFT ||
        vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LMENU || vk == VK_RMENU) return;
    if (vk == VK_ESCAPE) { g_capture = 0; app::ApplyHotkeys(); Invalidate(); return; }
    UINT mods = 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
    if (GetKeyState(VK_MENU) & 0x8000) mods |= MOD_ALT;
    if (GetKeyState(VK_SHIFT) & 0x8000) mods |= MOD_SHIFT;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) mods |= MOD_WIN;
    bool fkey = vk >= VK_F1 && vk <= VK_F24;
    if (!mods && !fkey && vk != VK_PAUSE && vk != VK_SCROLL && vk != VK_INSERT && vk != VK_HOME && vk != VK_END && vk != VK_PRIOR && vk != VK_NEXT) {
        Toast(L"Use Ctrl, Alt or Shift with that key so it doesn't clash with your games.");
        return;
    }
    Hotkey h{mods, vk};
    for (int k = 1; k < HkCount; ++k)
        if (k != g_capture && g_cfg.hotkeys[k].vk == vk && g_cfg.hotkeys[k].mods == mods) g_cfg.hotkeys[k] = {};
    g_cfg.hotkeys[g_capture] = h;
    g_capture = 0;
    ConfigSaveSoon();
    app::ApplyHotkeys();
    Invalidate();
}

// ---------------------------------------------------------------- Themes
static std::vector<ThemeDef> AllThemes() {
    std::vector<ThemeDef> all = BuiltinThemes();
    all.insert(all.end(), g_customThemes.begin(), g_customThemes.end());
    return all;
}

static void ThemeEdited() { ConfigSaveSoon(); ApplyTheme(); app::ThemeChanged(); }

static void DrawThemePreview(const ThemeDef& def0, const D2D1_RECT_F& r, int index, bool selected) {
    ThemeDef def = def0;
    if (def.followWindows) def = BuiltinThemes()[SystemUsesLightTheme() ? 1 : 0];
    Theme pt = MakeTheme(def, selected ? g_cfg.accent : L"theme");
    uint64_t key = K("theme", index);
    bool hot = IsHover(key);
    g_c.FillRound(r, 10, hot ? g_t.surfaceHover : g_t.surface);
    D2D1_RECT_F pv = D2D1::RectF(r.left + 10, r.top + 10, r.right - 10, r.bottom - 50);
    if (def.backdrop == L"acrylic" || def.surfaceOpacity < 1.0f) {
        ID2D1GradientStopCollection* stops = nullptr;
        D2D1_GRADIENT_STOP gs[3] = {{0, Rgb(0x3A5BD9)}, {0.55f, Rgb(0x8E4FD6)}, {1, Rgb(0xE0679A)}};
        g_rt->CreateGradientStopCollection(gs, 3, &stops);
        ID2D1LinearGradientBrush* lg = nullptr;
        if (stops) g_rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(pv.left, pv.top), D2D1::Point2F(pv.right, pv.bottom)), stops, &lg);
        if (lg) { g_rt->FillRoundedRectangle(D2D1::RoundedRect(pv, 7, 7), lg); lg->Release(); }
        if (stops) stops->Release();
        g_c.FillRound(pv, 7, Rgb(def.bg, 1.0f - def.transparency));
    } else {
        g_c.FillRound(pv, 7, Rgb(def.bg));
    }
    float w = pv.right - pv.left;
    D2D1_RECT_F sb = D2D1::RectF(pv.left, pv.top, pv.left + w * 0.27f, pv.bottom);
    g_c.FillRound(sb, 7, def.backdrop == L"acrylic" ? Rgb(def.sidebar, 0.35f) : Rgb(def.sidebar));
    g_c.FillRound(D2D1::RectF(sb.left + 6, pv.top + 7, sb.left + 14, pv.top + 15), 2, pt.accent);
    for (int k = 0; k < 4; ++k) {
        float ly = pv.top + 24 + k * 11;
        g_c.FillRound(D2D1::RectF(sb.left + 8, ly, sb.left + 8 + (sb.right - sb.left - 18) * (k == 0 ? 0.8f : 0.55f), ly + 3), 1.5f, k == 0 ? pt.text : pt.subtext);
    }
    float cx = sb.right + 8, cw = pv.right - cx - 8;
    g_c.FillRound(D2D1::RectF(cx, pv.top + 8, cx + w * 0.22f, pv.top + 12), 2, pt.text);
    D2D1_RECT_F card = D2D1::RectF(cx, pv.top + 20, cx + cw, pv.bottom - 8);
    g_c.FillRound(card, 4, def.surfaceOpacity < 1.0f ? Rgb(def.surface, def.surfaceOpacity * 1.8f) : pt.surface);
    g_c.FillRound(D2D1::RectF(card.left + 6, card.top + 6, card.left + 6 + cw * 0.4f, card.top + 9), 1.5f, pt.text);
    g_c.FillRound(D2D1::RectF(card.right - 30, card.top + 5, card.right - 8, card.top + 13), 4, pt.accent);
    g_c.FillRound(D2D1::RectF(card.left + 6, card.top + 18, card.right - 8, card.top + 20.5f), 1.2f, Alpha(pt.subtext, 0.7f));
    g_c.StrokeRound(pv, 7, Alpha(g_t.text, 0.10f));
    g_c.Text(def0.name, Format(13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(r.left + 14, r.bottom - 44, r.right - 40, r.bottom - 24), g_t.text);
    g_c.Text(def0.description, Format(11.5f), D2D1::RectF(r.left + 14, r.bottom - 25, r.right - 12, r.bottom - 8), g_t.subtext);
    if (def0.custom) Pill(r.right - 12, r.bottom - 43, L"Custom", g_t.info, true);
    if (selected) {
        g_c.StrokeRound(r, 10, g_t.accent, 2);
        g_c.Circle(r.right - 22, r.top + 22, 10, g_t.accent);
        g_c.Glyph(icon::Check, Format(10, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true), D2D1::RectF(r.right - 32, r.top + 12, r.right - 12, r.top + 32), g_t.onAccent);
    } else if (hot) {
        g_c.StrokeRound(r, 10, Alpha(g_t.accent, 0.5f));
    }
    std::wstring id = def0.id;
    AddHot(r, key, [id] { g_cfg.theme = id; ThemeEdited(); });
}

static void DrawThemes() {
    Title(L"Themes", L"The suite's themes. Click one to try it; it changes right away. The overlay can follow the accent color (Overlay → Colors).");
    auto all = AllThemes();
    float gap = 14, minW = 190, avail = g_x.right - g_x.left;
    int cols = std::max(1, (int)((avail + gap) / (minW + gap)));
    float cw = (avail - gap * (cols - 1)) / cols, ch = std::min(180.0f, cw * 0.6f + 50);
    for (size_t k = 0; k < all.size(); ++k) {
        int c = (int)(k % cols), row = (int)(k / cols);
        D2D1_RECT_F r = D2D1::RectF(g_x.left + c * (cw + gap), g_x.y + row * (ch + gap), g_x.left + c * (cw + gap) + cw, g_x.y + row * (ch + gap) + ch);
        if (r.bottom > 0 && r.top < g_H) DrawThemePreview(all[k], r, (int)k, all[k].id == g_cfg.theme);
        else AddHot(r, K("theme", (int)k), [id = all[k].id] { g_cfg.theme = id; ThemeEdited(); });
    }
    g_x.y += ((int)all.size() + cols - 1) / cols * (ch + gap) + 18;

    D2D1_RECT_F box = Section(L"ACCENT COLOR", 64);
    float ax = box.left + 16, ay = box.top + 16;
    ThemeDef cur = FindTheme(g_cfg.theme, g_customThemes);
    auto chip = [&](const std::wstring& label, uint32_t color, const std::wstring& value, bool sel, const char* name) {
        auto* f = Format(12.5f, sel ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL);
        float w = MeasureWidth(label, f) + 42;
        D2D1_RECT_F r = D2D1::RectF(ax, ay, ax + w, ay + 32);
        uint64_t k = K(name);
        g_c.FillRound(r, 16, sel ? Alpha(g_t.accent, 0.18f) : (IsHover(k) ? g_t.surfaceHover : Mix(g_t.surface, g_t.surfaceStrong, 0.4f)));
        if (sel) g_c.StrokeRound(r, 16, g_t.accent, 1.5f);
        g_c.Circle(r.left + 16, ay + 16, 7, Rgb(color));
        g_c.Text(label, f, D2D1::RectF(r.left + 29, ay, r.right - 8, ay + 32), g_t.text);
        AddHot(r, k, [value] {
            if (value == L"custom") {
                uint32_t c = ResolveAccent(FindTheme(g_cfg.theme, g_customThemes), g_cfg.accent);
                if (!PickColor(c)) return;
                g_cfg.accent = HexColor(c);
            } else {
                g_cfg.accent = value;
            }
            ThemeEdited();
        });
        ax += w + 8;
    };
    chip(L"Theme's own", ResolveAccent(cur, L"theme"), L"theme", g_cfg.accent == L"theme", "acctheme");
    chip(L"Windows", SystemAccent(), L"windows", g_cfg.accent == L"windows", "accwin");
    ax += 6;
    bool customSel = g_cfg.accent.size() == 7 && g_cfg.accent[0] == L'#';
    for (int k = 0; k < kAccentSwatchCount; ++k) {
        bool sel = IEquals(g_cfg.accent, HexColor(kAccentSwatches[k]));
        if (sel) customSel = false;
        if (ax + 32 > box.right - 110) break;
        std::wstring hex = HexColor(kAccentSwatches[k]);
        Swatch(ax + 16, ay + 16, kAccentSwatches[k], sel, K("accsw", k), [hex] { g_cfg.accent = hex; ThemeEdited(); });
        ax += 38;
    }
    ax += 4;
    chip(customSel ? g_cfg.accent : L"Custom…", customSel ? ResolveAccent(cur, g_cfg.accent) : 0x888888, L"custom", customSel, "acccustom");

    D2D1_RECT_F b2 = Section(L"YOUR THEMES", 64);
    g_c.Text(L"Theme files", Format(14), D2D1::RectF(b2.left + 18, b2.top + 12, b2.right - 420, b2.top + 32), g_t.text);
    g_c.Text(L"Same format as Hangar's. Put .json files in the themes folder, then Reload.", Format(12), D2D1::RectF(b2.left + 18, b2.top + 32, b2.right - 420, b2.top + 50), g_t.subtext);
    float bx = b2.right - 18;
    bx = ButtonR(bx, b2.top + 16, L"Reload", icon::Refresh, Btn::Secondary, K("treload"), [] { ApplyTheme(); app::ThemeChanged(); Toast(L"Themes reloaded."); }) - 8;
    bx = ButtonR(bx, b2.top + 16, L"Open folder", icon::Folder, Btn::Secondary, K("tfolder"), [] {
        EnsureDir(ThemesDir());
        ShellExecuteW(g_hwnd, L"open", ThemesDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }) - 8;
    ButtonR(bx, b2.top + 16, L"Save as file", icon::Download, Btn::Secondary, K("texport"), [] {
        ThemeDef cur = FindTheme(g_cfg.theme, g_customThemes);
        if (cur.followWindows) cur = BuiltinThemes()[SystemUsesLightTheme() ? 1 : 0];
        std::wstring base = cur.custom ? cur.name : cur.name + L" (copy)";
        for (auto& ch : base) if (wcschr(L"\\/:*?\"<>|", ch)) ch = L'-';
        EnsureDir(ThemesDir());
        std::wstring path = ThemesDir() + L"\\" + base + L".json";
        if (WriteTextFileAtomic(path, ThemeToJson(cur, g_cfg.accent))) {
            g_customThemes = LoadCustomThemes();
            Toast(L"Saved " + PathName(path) + L". Edit it, then press Reload.");
            ShellExecuteW(g_hwnd, L"open", L"explorer.exe", (L"/select,\"" + path + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        } else {
            Toast(L"Couldn't save the theme file.");
        }
    });
}

// ---------------------------------------------------------------- Settings
static void DrawSettings() {
    Title(L"Settings", L"");
    // startup
    D2D1_RECT_F s1 = Section(L"STARTUP AND PERMISSIONS", 64 * 3);
    bool startup = g_startup, asAdmin = g_startupAdmin;
    RowLabel(s1.top, L"Start Reticle when I sign in", L"Opens quietly in the tray");
    Toggle(s1.right - 18, s1.top + 21, startup, K("startup"), [startup, asAdmin] {
        std::wstring err;
        g_cfg.startWithWindows = !startup;
        if (!sys::SetStartup(!startup, asAdmin, &err)) Toast(err);
        ReadSystemState();
        ConfigSaveSoon();
        Invalidate();
    });
    Divider(s1.top + 64);
    RowLabel(s1.top + 64, L"Start with admin rights", L"Frame capture and CPU temperature from sign-in, with no UAC prompt. Turn on from an admin Reticle.");
    Toggle(s1.right - 18, s1.top + 64 + 21, asAdmin, K("startadmin"), [asAdmin] {
        std::wstring err;
        if (!sys::SetStartup(true, !asAdmin, &err)) Toast(err);
        else g_cfg.startWithWindows = true;
        ReadSystemState();
        ConfigSaveSoon();
        Invalidate();
    }, sys::IsElevated() || asAdmin);
    Divider(s1.top + 128);
    {
        std::wstring sub = sys::IsElevated() ? L"Reticle is running as administrator"
                         : sys::InPerfLogUsers() ? L"Allowed: your account is in Performance Log Users (no admin needed)"
                         : g_grantPending ? L"Allowed after you sign out and back in"
                         : L"Not allowed yet: Windows only lets admins and Performance Log Users read graphics events";
        RowLabel(s1.top + 128, L"Frame capture (FPS, frame times, lows, benchmarks)", sub, 300);
        if (CaptureNeedsPermission() && !g_grantPending)
            ButtonR(s1.right - 18, s1.top + 128 + 16, L"Allow…", 0xE7EF, Btn::Primary, K("allow2"), [] { Navigate(Page::Home); });
        else
            Pill(s1.right - 18, s1.top + 128 + 22, frames::GetStatus() == frames::Status::Running ? L"Active" : L"Ready", g_t.good, true);
    }

    // sensors
    D2D1_RECT_F s2 = Section(L"SENSORS", 64 * 2);
    {
        std::wstring sub;
        bool needDriver = false;
        switch (sensors::CpuTemp()) {
            case sensors::CpuTempState::Ok: sub = L"Reading " + sensors::CpuName() + L" through PawnIO"; break;
            case sensors::CpuTempState::NeedsAdmin: sub = L"Needs Reticle running as administrator (the PawnIO driver only talks to admin programs)"; break;
            case sensors::CpuTempState::NeedsDriver: sub = L"Needs the free, signed PawnIO driver (the same one Ember and LibreHardwareMonitor use)"; needDriver = true; break;
            default: sub = L"Not supported on this CPU"; break;
        }
        RowLabel(s2.top, L"CPU temperature", sub, 220);
        if (needDriver) ButtonR(s2.right - 18, s2.top + 16, L"Get PawnIO", 0xE71B, Btn::Secondary, K("pawnio"), [] {
            ShellExecuteW(g_hwnd, L"open", L"https://github.com/namazso/PawnIO.Setup/releases/latest", nullptr, nullptr, SW_SHOWNORMAL);
        });
        else Pill(s2.right - 18, s2.top + 22, sensors::CpuTemp() == sensors::CpuTempState::Ok ? L"Working" : L"Off",
                  sensors::CpuTemp() == sensors::CpuTempState::Ok ? g_t.good : g_t.faint, true);
        Divider(s2.top + 64);
        RowLabel(s2.top + 64, L"GPU", sensors::HasNvidia() ? sensors::GpuName() + L" through NVIDIA's NVML" : L"GPU stats need an NVIDIA card and driver (NVML)", 220);
        Pill(s2.right - 18, s2.top + 64 + 22, sensors::HasNvidia() ? L"Working" : L"Not found", sensors::HasNvidia() ? g_t.good : g_t.faint, true);
    }

    // updates
    D2D1_RECT_F s3 = Section(L"UPDATES", 64 * 3);
    {
        const SelfUpdateState& u = g_app.upd;
        std::wstring status;
        switch (u.kind) {
            case SelfUpdateState::Checking: status = L"Checking…"; break;
            case SelfUpdateState::UpToDate: status = std::wstring(L"Up to date"); break;
            case SelfUpdateState::Available: status = L"Reticle " + u.version + L" is available"; break;
            case SelfUpdateState::Downloading: status = L"Downloading " + u.version + (u.percent >= 0 ? L" · " + std::to_wstring(u.percent) + L"%" : L""); break;
            case SelfUpdateState::Ready:
                status = L"Reticle " + u.version + L" is downloaded and verified" + (g_app.updateDeferredForGame ? L" · installs after your game closes" : L"");
                break;
            case SelfUpdateState::Error: status = u.message; break;
            default: status = L"Checks GitHub on launch and every few hours"; break;
        }
        RowLabel(s3.top, std::wstring(L"Reticle ") + RETICLE_VERSION, status, 230);
        if (u.kind == SelfUpdateState::Ready || u.kind == SelfUpdateState::Available)
            ButtonR(s3.right - 18, s3.top + 16, u.kind == SelfUpdateState::Ready ? L"Restart to update" : L"Install", icon::Update, Btn::Primary, K("updnow"),
                    [] { app::InstallUpdateNow(); Invalidate(); }, !app::GameRunning());
        else
            ButtonR(s3.right - 18, s3.top + 16, L"Check for updates now", icon::Refresh, Btn::Secondary, K("updcheck"), [] { app::CheckUpdates(true); Invalidate(); },
                    u.kind != SelfUpdateState::Checking && u.kind != SelfUpdateState::Downloading);
        Divider(s3.top + 64);
        RowLabel(s3.top + 64, L"Update automatically", L"Downloads in the background and verifies SHA-256. Never updates or asks while a game is running.");
        Toggle(s3.right - 18, s3.top + 64 + 21, g_cfg.autoUpdate, K("autoupd"), [] { g_cfg.autoUpdate = !g_cfg.autoUpdate; ConfigSaveSoon(); Invalidate(); });
        Divider(s3.top + 128);
        RowLabel(s3.top + 128, L"Channel", L"Beta gets new features first (versions with a \"-\")");
        Segmented(s3.right - 18, s3.top + 128 + 16, {L"Stable", L"Beta"}, g_cfg.beta ? 1 : 0, "channel", [](int i) { g_cfg.beta = i == 1; ConfigSaveSoon(); app::CheckUpdates(true); Invalidate(); });
    }

    // data
    D2D1_RECT_F s4 = Section(L"DATA", 64 * 2);
    RowLabel(s4.top, IsPortable() ? L"Portable mode" : L"Settings and benchmarks", DataDir(), 200);
    ButtonR(s4.right - 18, s4.top + 16, L"Open folder", icon::Folder, Btn::Secondary, K("datadir"), [] {
        EnsureDir(DataDir());
        ShellExecuteW(g_hwnd, L"open", DataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    });
    Divider(s4.top + 64);
    RowLabel(s4.top + 64, L"No accounts, no telemetry", L"Reticle only connects to GitHub to check for updates.", 200);
    ButtonR(s4.right - 18, s4.top + 64 + 16, L"GitHub", 0xE71B, Btn::Subtle, K("github"), [] {
        ShellExecuteW(g_hwnd, L"open", L"https://github.com/larosa023/Reticle", nullptr, nullptr, SW_SHOWNORMAL);
    });
    g_c.Text(L"Portable mode: put an empty file named \"portable\" next to Reticle.exe and everything is kept in a data folder beside it.",
             Format(12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true), D2D1::RectF(g_x.left, g_x.y - 10, g_x.right, g_x.y + 24), g_t.faint);
    g_x.y += 28;
}

// ---------------------------------------------------------------- paint
static void DrawToast() {
    if (g_toast.empty() || GetTickCount64() > g_toastUntil) return;
    auto* f = Format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    float w = std::min(g_W - kSidebar - 40, MeasureWidth(g_toast, f) + 40);
    float cx = kSidebar + (g_W - kSidebar) / 2;
    D2D1_RECT_F r = D2D1::RectF(cx - w / 2, g_H - 64, cx + w / 2, g_H - 24);
    g_c.FillRound(r, 8, g_t.dark ? Rgb(0x2E2E36) : Rgb(0x26262C));
    g_c.Text(g_toast, f, r, Rgb(0xFFFFFF));
}

static void Paint() {
    if (!EnsureDevice()) return;
    D2D1_SIZE_F sz = g_rt->GetSize();
    g_W = sz.width;
    g_H = sz.height;
    g_c.rt = g_rt;
    g_c.br = g_br;
    g_hot.clear();
    g_rt->BeginDraw();
    g_rt->SetTransform(D2D1::Matrix3x2F::Identity());
    g_rt->Clear(g_t.bg);
    float pad = 32;
    g_x.left = kSidebar + pad;
    g_x.right = std::min(g_W - pad, kSidebar + pad + 900);
    g_x.y = 22 - g_scroll;
    g_rt->PushAxisAlignedClip(D2D1::RectF(kSidebar, 0, g_W, g_H), D2D1_ANTIALIAS_MODE_ALIASED);
    switch (g_page) {
        case Page::Home: DrawHome(); break;
        case Page::Overlay: DrawOverlay(); break;
        case Page::Crosshair: DrawCrosshair(); break;
        case Page::Games: DrawGames(); break;
        case Page::Benchmarks: DrawBenchmarks(); break;
        case Page::Shortcuts: DrawShortcuts(); break;
        case Page::Themes: DrawThemes(); break;
        case Page::Settings: DrawSettings(); break;
    }
    g_contentH = g_x.y + g_scroll;
    g_rt->PopAxisAlignedClip();
    // scrollbar
    if (g_contentH > g_H + 1) {
        float th = std::max(40.0f, (g_H - 8) * g_H / g_contentH);
        float t = g_scroll / (g_contentH - g_H);
        float ty = 4 + (g_H - 8 - th) * t;
        g_c.FillRound(D2D1::RectF(g_W - 7, ty, g_W - 3, ty + th), 2, g_t.dark ? Rgb(0xFFFFFF, 0.18f) : Rgb(0x000000, 0.2f));
    }
    DrawSidebar();
    DrawToast();
    HRESULT hr = g_rt->EndDraw();
    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) DiscardDevice();
    FrameDone();
    float maxS = std::max(0.0f, g_contentH - g_H);
    if (g_scroll > maxS) { g_scroll = maxS; Invalidate(); }
}

static void Navigate(Page p) {
    g_scrollFor[g_page] = g_scroll;
    if (g_capture) { g_capture = 0; app::ApplyHotkeys(); }
    g_page = p;
    g_scroll = g_scrollFor.count(p) ? g_scrollFor[p] : 0;
    app::PreviewOverlay(p == Page::Overlay && g_previewWanted);
    if (p == Page::Benchmarks) g_runsLoaded = false;
    ReadSystemState(p == Page::Settings);
    Invalidate();
}

static const Hot* HitAt(float x, float y) {
    for (auto it = g_hot.rbegin(); it != g_hot.rend(); ++it)
        if (x >= it->r.left && x < it->r.right && y >= it->r.top && y < it->r.bottom) return &*it;
    return nullptr;
}

static void SaveWindowPos() {
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (!GetWindowPlacement(g_hwnd, &wp)) return;
    RECT r = wp.rcNormalPosition;
    g_cfg.windowX = r.left; g_cfg.windowY = r.top; g_cfg.windowW = r.right - r.left; g_cfg.windowH = r.bottom - r.top;
    ConfigSaveSoon();
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            g_hwnd = hwnd;
            g_scale = GetDpiForWindow(hwnd) / 96.0f;
            ApplyTheme();
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            Paint();
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_SIZE:
            if (g_rt) g_rt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
            Invalidate();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = (MINMAXINFO*)lp;
            float s = GetDpiForWindow(hwnd) / 96.0f;
            mm->ptMinTrackSize.x = (LONG)(900 * s);
            mm->ptMinTrackSize.y = (LONG)(560 * s);
            return 0;
        }
        case WM_DPICHANGED: {
            g_scale = HIWORD(wp) / 96.0f;
            RECT* r = (RECT*)lp;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            if (g_rt) g_rt->SetDpi((float)HIWORD(wp), (float)HIWORD(wp));
            Invalidate();
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (lp && !wcscmp((const wchar_t*)lp, L"ImmersiveColorSet")) { InvalidateSystemColors(); ApplyTheme(); }
            return 0;
        case WM_DWMCOLORIZATIONCOLORCHANGED:
            InvalidateSystemColors();
            ApplyTheme();
            return 0;
        case WM_MOUSEMOVE: {
            float x = GET_X_LPARAM(lp) / g_scale, y = GET_Y_LPARAM(lp) / g_scale;
            if (g_dragFn) {
                g_dragFn(std::clamp((x - g_dragL) / std::max(1.0f, g_dragR - g_dragL), 0.0f, 1.0f));
                return 0;
            }
            if (!g_tracking) {
                TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&t);
                g_tracking = true;
            }
            const Hot* h = HitAt(x, y);
            uint64_t k = h && h->enabled ? h->key : 0;
            if (k != g_hover) { g_hover = k; Invalidate(); }
            SetCursor(LoadCursorW(nullptr, k ? IDC_HAND : IDC_ARROW));
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) return TRUE;
            break;
        case WM_MOUSELEAVE:
            g_tracking = false;
            if (g_hover) { g_hover = 0; Invalidate(); }
            return 0;
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            float x = GET_X_LPARAM(lp) / g_scale, y = GET_Y_LPARAM(lp) / g_scale;
            const Hot* h = HitAt(x, y);
            g_pressed = h && h->enabled ? h->key : 0;
            if (h && h->drag && h->enabled) {
                g_dragKey = h->key;
                g_dragFn = h->drag;
                g_dragL = h->r.left + 10;
                g_dragR = h->r.right - 10;
                g_dragFn(std::clamp((x - g_dragL) / std::max(1.0f, g_dragR - g_dragL), 0.0f, 1.0f));
            }
            SetCapture(hwnd);
            Invalidate();
            return 0;
        }
        case WM_LBUTTONUP: {
            ReleaseCapture();
            if (g_dragFn) { g_dragFn = nullptr; g_dragKey = 0; g_pressed = 0; ConfigFlush(); Invalidate(); return 0; }
            float x = GET_X_LPARAM(lp) / g_scale, y = GET_Y_LPARAM(lp) / g_scale;
            const Hot* h = HitAt(x, y);
            uint64_t down = g_pressed;
            g_pressed = 0;
            if (h && h->enabled && h->key == down && h->click) {
                auto fn = h->click;   // the list is rebuilt by the next paint
                fn();
            }
            Invalidate();
            return 0;
        }
        case WM_MOUSEWHEEL:
            g_scroll = std::max(0.0f, g_scroll - GET_WHEEL_DELTA_WPARAM(wp) / 120.0f * 72.0f);
            Invalidate();
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (g_capture) { OnCaptureKey((UINT)wp); return 0; }
            if (wp == VK_ESCAPE && overlay::Moving()) { overlay::EndMove(); Invalidate(); return 0; }
            if (wp == VK_PRIOR) { g_scroll = std::max(0.0f, g_scroll - g_H * 0.8f); Invalidate(); return 0; }
            if (wp == VK_NEXT) { g_scroll += g_H * 0.8f; Invalidate(); return 0; }
            if (wp == VK_HOME && !(GetKeyState(VK_CONTROL) & 0x8000)) { g_scroll = 0; Invalidate(); return 0; }
            if (msg == WM_SYSKEYDOWN) break;
            return 0;
        case WM_SYSCHAR:
            if (g_capture) return 0;   // no menu beep while recording Alt shortcuts
            break;
        case WM_TIMER:
            if (wp == 2) { KillTimer(hwnd, 2); Invalidate(); }
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE && g_capture) { g_capture = 0; app::ApplyHotkeys(); Invalidate(); }
            return 0;
        case WM_CLOSE:
            Close();
            return 0;
        case WM_DESTROY:
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void Open(const wchar_t* page) {
    Page p = g_page;
    if (page) {
        std::wstring s = page;
        p = s == L"overlay" ? Page::Overlay : s == L"crosshair" ? Page::Crosshair : s == L"games" ? Page::Games : s == L"benchmarks" ? Page::Benchmarks
          : s == L"shortcuts" ? Page::Shortcuts : s == L"themes" ? Page::Themes : s == L"settings" ? Page::Settings : Page::Home;
    }
    if (g_hwnd) {
        if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
        SetForegroundWindow(g_hwnd);
        if (page) Navigate(p);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = g_app.inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(g_app.inst, MAKEINTRESOURCEW(1));
        wc.hIconSm = (HICON)LoadImageW(g_app.inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
        wc.lpszClassName = L"Reticle.larosa023.Settings";
        RegisterClassExW(&wc);
        registered = true;
    }
    float s = GetDpiForSystem() / 96.0f;
    int w = g_cfg.windowW > 0 ? g_cfg.windowW : (int)(1120 * s);
    int h = g_cfg.windowH > 0 ? g_cfg.windowH : (int)(780 * s);
    int x = g_cfg.windowX, y = g_cfg.windowY;
    if (x != CW_USEDEFAULT) {
        RECT r{x, y, x + w, y + h};
        if (!MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) x = y = CW_USEDEFAULT;
    }
    g_page = p;
    g_scroll = 0;
    HWND hwnd = CreateWindowExW(0, L"Reticle.larosa023.Settings", L"Reticle", WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr, g_app.inst, nullptr);
    if (!hwnd) return;
    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd);
    if (p == Page::Overlay) app::PreviewOverlay(g_previewWanted);
    ReadSystemState(p == Page::Settings);
}

void Close() {
    if (!g_hwnd) return;
    if (g_capture) { g_capture = 0; app::ApplyHotkeys(); }
    app::PreviewOverlay(false);
    SaveWindowPos();
    ConfigFlush();
    HWND h = g_hwnd;
    DiscardDevice();
    g_hwnd = nullptr;
    g_hot.clear();
    g_runs.clear();
    g_selFrames.clear(); g_cmpFrames.clear();
    g_runsLoaded = false;
    DestroyWindow(h);
    ClearTextCaches();
    // give the memory back: Reticle sits in the tray most of the time
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

} // namespace ui
