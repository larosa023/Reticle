// Reticle - the overlay and crosshair windows (see overlay.h)
#include "overlay.h"
#include "app.h"
#include "gfx.h"
#include "util.h"
#include <wincodec.h>
#include <algorithm>
#include <cmath>

using namespace gfx;

namespace overlay {

// ---------------------------------------------------------------- layered surface
struct Surface {
    HWND hwnd = nullptr;
    HDC mem = nullptr;
    HBITMAP bmp = nullptr, oldBmp = nullptr;
    int w = 0, h = 0;
    ID2D1DCRenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* br = nullptr;

    bool Ensure(int nw, int nh) {
        nw = std::max(1, nw); nh = std::max(1, nh);
        if (!mem) {
            HDC screen = GetDC(nullptr);
            mem = CreateCompatibleDC(screen);
            ReleaseDC(nullptr, screen);
        }
        if (nw != w || nh != h || !bmp) {
            if (bmp) { SelectObject(mem, oldBmp); DeleteObject(bmp); bmp = nullptr; }
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = nw;
            bi.bmiHeader.biHeight = -nh;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            void* bits = nullptr;
            bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (!bmp) return false;
            oldBmp = (HBITMAP)SelectObject(mem, bmp);
            w = nw; h = nh;
        }
        if (!rt) {
            D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
            if (FAILED(d2d->CreateDCRenderTarget(&props, &rt))) return false;
            rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &br);
        }
        RECT r{0, 0, w, h};
        return SUCCEEDED(rt->BindDC(mem, &r));
    }
    void Present(int x, int y, BYTE alpha = 255) {
        POINT pos{x, y}, src{0, 0};
        SIZE size{w, h};
        BLENDFUNCTION bf{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
        UpdateLayeredWindow(hwnd, nullptr, &pos, &size, mem, &src, 0, &bf, ULW_ALPHA);
    }
    void Discard() { SafeRelease(br); SafeRelease(rt); }
    void Free() {
        Discard();
        if (bmp) { SelectObject(mem, oldBmp); DeleteObject(bmp); bmp = nullptr; }
        if (mem) { DeleteDC(mem); mem = nullptr; }
        w = h = 0;
    }
};

static Surface s_panel, s_cross;
static OverlayStyle s_style;
static RECT s_ref{};
static UINT s_dpi = 96;
static bool s_visible = false, s_crossVisible = false;
static std::wstring s_toast;
static ULONGLONG s_toastUntil = 0;
static bool s_result = false;
static ULONGLONG s_resultUntil = 0;
static bench::Run s_resultRun;
static std::vector<float> s_resultGraph;     // downsampled frame times of the finished run
static float s_stickyW = 0;
static uint32_t s_stickyKey = 0;
static ULONGLONG s_lastTopmost = 0;
static int s_px = 0, s_py = 0;               // current panel position
// move mode
static bool s_moving = false, s_dragging = false;
static POINT s_grab{};
static std::function<void(Anchor, float, float)> s_moveDone;
static Anchor s_moveAnchor = Anchor::TopLeft;
static float s_moveX = 0, s_moveY = 0;
static ID2D1Bitmap* s_crossBmp = nullptr;

static const wchar_t* kPanelClass = L"Reticle.larosa023.Overlay";
static const wchar_t* kCrossClass = L"Reticle.larosa023.Crosshair";

static float Scale() { return s_dpi / 96.0f; }

// ---------------------------------------------------------------- formatting
static std::wstring Num(double v, int decimals) {
    wchar_t b[32];
    swprintf(b, 32, L"%.*f", decimals, v);   // always pass the precision: "%.0f" with it read the int as the number
    return b;
}
static std::wstring Temp(float c, bool shortUnit) {
    if (c < 0) return L"—";
    double v = g_cfg.tempF ? c * 9.0 / 5.0 + 32 : c;
    return Num(v, 0) + (shortUnit ? L"°" : (g_cfg.tempF ? L"°F" : L"°C"));
}
static std::wstring Gb(float mb) { return mb < 0 ? L"—" : Num(mb / 1024.0, mb >= 10240 ? 0 : 1); }
static std::wstring Clock() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t b[32];
    if (g_cfg.clock24) swprintf(b, 32, L"%02d:%02d", st.wHour, st.wMinute);
    else swprintf(b, 32, L"%d:%02d %ls", st.wHour % 12 ? st.wHour % 12 : 12, st.wMinute, st.wHour < 12 ? L"AM" : L"PM");
    return b;
}
static std::wstring Duration(double secs) {
    long long s = (long long)secs;
    wchar_t b[32];
    if (s >= 3600) swprintf(b, 32, L"%lld:%02lld:%02lld", s / 3600, (s / 60) % 60, s % 60);
    else swprintf(b, 32, L"%lld:%02lld", s / 60, s % 60);
    return b;
}

// ---------------------------------------------------------------- panel model
enum Tone { Value, Dim, Warn, Bad, Accent, Rec };
struct Seg { std::wstring text; Tone tone = Value; };
enum RowKind { RowText, RowBig, RowGraph, RowBanner, RowResultGraph };
struct Row { RowKind kind = RowText; std::wstring label; std::vector<Seg> segs; };

static Tone TempTone(float c, float warn, float bad) { return c >= bad ? Bad : c >= warn ? Warn : Value; }

static std::vector<Row> BuildRows(bool compact) {
    std::vector<Row> rows;
    const OverlayStyle& st = s_style;
    const frames::Live& L = g_app.live;
    const sensors::Reading& S = g_app.sens;
    uint32_t on = st.stats;
    bool capture = frames::GetStatus() == frames::Status::Running;
    auto add = [&](RowKind k, const std::wstring& label, std::vector<Seg> segs) { rows.push_back({k, label, std::move(segs)}); };

    if (g_app.benching) {
        double el = frames::BenchElapsed();
        std::wstring t = L"REC " + Duration(el);
        if (g_cfg.benchSeconds > 0) t += L" / " + Duration(g_cfg.benchSeconds);
        add(RowBanner, L"", {{L"●  " + t, Rec}});
    }
    if (!s_toast.empty() && GetTickCount64() < s_toastUntil) add(RowBanner, L"", {{s_toast, Accent}});

    std::wstring fps = !capture ? L"—" : L.valid ? Num(L.fps, 0) : L"0";
    if (st.layout == Layout::Minimal) {
        add(RowBig, L"", {{fps, Value}});
        return rows;
    }
    if (on & StatFps) {
        if (compact) add(RowText, L"FPS", {{fps, Value}});
        else add(RowBig, L"FPS", {{fps, Value}, {capture ? L"" : L"  needs permission", Dim}});
    }
    if (on & StatFrameTime) add(RowText, compact ? L"" : L"FRAME", {{L.valid ? Num(L.frameMs, 1) : L"—", Value}, {L" ms", Dim}});
    if ((on & StatGraph) && !compact) add(RowGraph, L"", {});
    if (on & StatLows) {
        bool have = L.low1 > 0;
        if (compact) add(RowText, L"1%", {{have ? Num(L.low1, 0) : L"—", Value}, {L"  0.1% ", Dim}, {have ? Num(L.low01, 0) : L"—", Value}});
        else add(RowText, L"LOWS", {{have ? Num(L.low1, 0) : L"—", Value}, {L" 1%   ", Dim}, {have ? Num(L.low01, 0) : L"—", Value}, {L" 0.1%", Dim}});
    }
    if (on & (StatCpuUse | StatCpuTemp)) {
        std::vector<Seg> s;
        if (on & StatCpuUse) { s.push_back({S.cpuUse < 0 ? L"—" : Num(S.cpuUse, 0), Value}); s.push_back({L"%", Dim}); }
        if (on & StatCpuTemp) {
            if (!s.empty()) s.push_back({L"  ", Dim});
            s.push_back({Temp(S.cpuTemp, compact), TempTone(S.cpuTemp, 80, 90)});
        }
        add(RowText, L"CPU", s);
    }
    if (on & (StatGpuUse | StatGpuTemp | StatGpuClock | StatGpuPower)) {
        std::vector<Seg> s;
        auto gap = [&] { if (!s.empty()) s.push_back({L"  ", Dim}); };
        if (on & StatGpuUse) { s.push_back({S.gpuUse < 0 ? L"—" : Num(S.gpuUse, 0), Value}); s.push_back({L"%", Dim}); }
        if (on & StatGpuTemp) { gap(); s.push_back({Temp(S.gpuTemp, compact), TempTone(S.gpuTemp, 78, 85)}); }
        if (on & StatGpuClock) { gap(); s.push_back({S.gpuClock < 0 ? L"—" : Num(S.gpuClock, 0), Value}); s.push_back({L" MHz", Dim}); }
        if (on & StatGpuPower) { gap(); s.push_back({S.gpuPower < 0 ? L"—" : Num(S.gpuPower, 0), Value}); s.push_back({L" W", Dim}); }
        add(RowText, L"GPU", s);
    }
    if (on & StatVram) {
        if (compact) add(RowText, L"VRAM", {{Gb(S.vramUsed), Value}, {L"G", Dim}});
        else add(RowText, L"VRAM", {{Gb(S.vramUsed), Value}, {L" / " + Gb(S.vramTotal) + L" GB", Dim}});
    }
    if (on & StatRam) {
        if (compact) add(RowText, L"RAM", {{Gb(S.ramUsed), Value}, {L"G", Dim}});
        else add(RowText, L"RAM", {{Gb(S.ramUsed), Value}, {L" / " + Gb(S.ramTotal) + L" GB", Dim}});
    }
    if (on & StatClock) add(RowText, compact ? L"" : L"TIME", {{Clock(), Value}});
    if (on & StatSession) {
        double secs = g_app.sessionStart ? (GetTickCount64() - g_app.sessionStart) / 1000.0 : 0;
        add(RowText, compact ? L"" : L"PLAYED", {{Duration(secs), Value}});
    }
    return rows;
}

static std::vector<Row> ResultRows() {
    std::vector<Row> rows;
    const frames::Summary& s = s_resultRun.s;
    rows.push_back({RowBanner, L"", {{L"Benchmark saved · " + s_resultRun.game, Accent}}});
    rows.push_back({RowBig, L"AVG", {{Num(s.avgFps, 0), Value}, {L" fps", Dim}}});
    rows.push_back({RowText, L"1% LOW", {{Num(s.low1, 0), Value}, {L" fps", Dim}}});
    rows.push_back({RowText, L"0.1% LOW", {{Num(s.low01, 0), Value}, {L" fps", Dim}}});
    rows.push_back({RowText, L"LENGTH", {{Duration(s.seconds), Value}, {L"  " + std::to_wstring(s.frames) + L" frames", Dim}}});
    rows.push_back({RowResultGraph, L"", {}});
    return rows;
}

// ---------------------------------------------------------------- drawing
struct Fonts { IDWriteTextFormat *label, *value, *big, *banner; float ts; };

static Fonts MakeFonts() {
    float ts = s_style.textSize * Scale();
    Fonts f;
    f.ts = ts;
    f.label = Format(std::round(ts * 0.78f * 2) / 2, DWRITE_FONT_WEIGHT_BOLD);
    f.value = Format(std::round(ts * 2) / 2, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    f.big = Format(std::round(ts * 1.75f * 2) / 2, DWRITE_FONT_WEIGHT_BOLD);
    f.banner = Format(std::round(ts * 0.85f * 2) / 2, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    return f;
}

static D2D1_COLOR_F ToneColor(Tone t) {
    D2D1_COLOR_F text = Rgb(s_style.textColor);
    switch (t) {
        case Dim: return Alpha(text, 0.62f);
        case Warn: return Rgb(0xF2B84B);
        case Bad: return Rgb(0xFF6B6B);
        case Accent: return s_style.followAccent ? g_app.accent : Rgb(s_style.accentColor);
        case Rec: return Rgb(0xFF5D5D);
        default: return text;
    }
}

static float SegsWidth(const std::vector<Seg>& segs, IDWriteTextFormat* f, IDWriteTextFormat* dimF) {
    float w = 0;
    for (auto& s : segs) w += MeasureWidth(s.text, s.tone == Dim ? dimF : f);
    return w;
}

static void DrawSegs(Canvas& c, const std::vector<Seg>& segs, IDWriteTextFormat* f, IDWriteTextFormat* dimF, float x, float y, float h, bool shadow) {
    for (auto& s : segs) {
        if (s.text.empty()) continue;
        IDWriteTextFormat* ff = s.tone == Dim ? dimF : f;
        float w = MeasureWidth(s.text, ff) + 2;
        if (shadow) c.Text(s.text, ff, D2D1::RectF(x + 1, y + 1, x + w + 1, y + h + 1), D2D1::ColorF(0, 0, 0, 0.75f));
        c.Text(s.text, ff, D2D1::RectF(x, y, x + w, y + h), ToneColor(s.tone));
        x += w - 2;
    }
}

static void DrawGraph(Canvas& c, const D2D1_RECT_F& r, const std::vector<float>& data, bool showAvg) {
    if (data.size() < 2) return;
    float w = r.right - r.left, h = r.bottom - r.top;
    std::vector<float> sorted(data);
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    float median = sorted[sorted.size() / 2];
    float mx = *std::max_element(data.begin(), data.end());
    float top = std::max(median * 2.0f, std::min(mx * 1.1f, median * 6.0f));
    top = std::max(top, 4.0f);
    D2D1_COLOR_F accent = ToneColor(Accent);
    c.FillRound(r, 3 * Scale(), D2D1::ColorF(0, 0, 0, 0.18f));
    // median line
    float my = r.bottom - std::min(1.0f, median / top) * h;
    if (showAvg) c.Line(r.left, my, r.right, my, Alpha(Rgb(s_style.textColor), 0.18f), 1);
    ID2D1PathGeometry* geo = nullptr;
    if (FAILED(d2d->CreatePathGeometry(&geo))) return;
    ID2D1GeometrySink* sink = nullptr;
    if (SUCCEEDED(geo->Open(&sink))) {
        size_t n = data.size();
        float step = w / (float)(n - 1);
        sink->BeginFigure(D2D1::Point2F(r.left, r.bottom - std::min(1.0f, data[0] / top) * h), D2D1_FIGURE_BEGIN_HOLLOW);
        for (size_t i = 1; i < n; ++i) sink->AddLine(D2D1::Point2F(r.left + i * step, r.bottom - std::min(1.0f, data[i] / top) * h));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        sink->Release();
        c.br->SetColor(accent);
        c.rt->DrawGeometry(geo, c.br, std::max(1.0f, 1.25f * Scale()));
    }
    geo->Release();
    // spikes: frames over 2.5x the median
    float step = w / (float)(data.size() - 1);
    for (size_t i = 0; i < data.size(); ++i) {
        if (data[i] > median * 2.5f && data[i] > median + 4) {
            float x = r.left + i * step;
            c.Line(x, r.bottom, x, r.bottom - std::min(1.0f, data[i] / top) * h, Rgb(0xFF6B6B, 0.85f), std::max(1.0f, Scale()));
        }
    }
}

static D2D1_RECT_F RefRect() { return D2D1::RectF((float)s_ref.left, (float)s_ref.top, (float)s_ref.right, (float)s_ref.bottom); }

static void PositionFor(float w, float h, int& x, int& y) {
    float m = 14 * Scale();
    D2D1_RECT_F r = RefRect();
    switch (s_style.anchor) {
        case Anchor::TopLeft: x = (int)(r.left + m); y = (int)(r.top + m); break;
        case Anchor::TopRight: x = (int)(r.right - m - w); y = (int)(r.top + m); break;
        case Anchor::BottomLeft: x = (int)(r.left + m); y = (int)(r.bottom - m - h); break;
        case Anchor::BottomRight: x = (int)(r.right - m - w); y = (int)(r.bottom - m - h); break;
        default:
            x = (int)(r.left + s_style.customX * (r.right - r.left));
            y = (int)(r.top + s_style.customY * (r.bottom - r.top));
            x = std::max((int)r.left, std::min(x, (int)(r.right - w)));
            y = std::max((int)r.top, std::min(y, (int)(r.bottom - h)));
            break;
    }
}

static void RenderPanel() {
    if (!s_visible || !s_panel.hwnd) return;
    if (s_result && GetTickCount64() > s_resultUntil) s_result = false;
    bool compact = s_style.layout == Layout::Compact && !s_result;
    std::vector<Row> rows = s_result ? ResultRows() : BuildRows(compact);
    if (s_moving) rows.push_back({RowBanner, L"", {{L"Drag to move · double-click when done", Accent}}});
    Fonts f = MakeFonts();
    float sc = Scale();
    float pad = std::round(f.ts * 0.7f), rowH = std::round(f.ts * 1.5f), bigH = std::round(f.ts * 2.15f), graphH = std::round(f.ts * 2.6f);
    float gapX = std::round(f.ts * 0.75f);

    float W = 0, H = 0, labelW = 0;
    if (compact) {
        // one line: label value · label value …
        std::vector<Row> line, banners;
        for (auto& r : rows) (r.kind == RowBanner ? banners : line).push_back(r);
        float x = 0;
        for (auto& r : line) {
            if (!r.label.empty()) x += MeasureWidth(r.label, f.label) + f.ts * 0.35f;
            x += SegsWidth(r.segs, f.value, f.label) + gapX;
        }
        W = std::max(x - gapX, 0.0f) + pad * 2;
        for (auto& b : banners) W = std::max(W, SegsWidth(b.segs, f.banner, f.banner) + pad * 2);
        H = pad * 2 * 0.7f + rowH * (1 + banners.size());
        rows.clear();
        rows.insert(rows.end(), banners.begin(), banners.end());
        Row merged;
        merged.kind = RowText;
        for (auto& r : line) {
            if (!merged.segs.empty()) merged.segs.push_back({std::wstring(L"   "), Dim});
            if (!r.label.empty()) merged.segs.push_back({r.label + L" ", Accent});
            merged.segs.insert(merged.segs.end(), r.segs.begin(), r.segs.end());
        }
        rows.push_back(merged);
    } else {
        for (auto& r : rows) if (r.kind == RowText || r.kind == RowBig) labelW = std::max(labelW, MeasureWidth(r.label, f.label));
        float valueW = 0;
        for (auto& r : rows) {
            if (r.kind == RowText) valueW = std::max(valueW, SegsWidth(r.segs, f.value, f.label));
            else if (r.kind == RowBig) valueW = std::max(valueW, SegsWidth(r.segs, f.big, f.label));
        }
        float inner = (labelW > 0 ? labelW + gapX : 0) + valueW;
        for (auto& r : rows) if (r.kind == RowBanner) inner = std::max(inner, SegsWidth(r.segs, f.banner, f.banner));
        inner = std::max(inner, (s_style.layout == Layout::Minimal ? 30.0f : 150.0f) * sc);
        W = inner + pad * 2;
        H = pad * 2;
        for (auto& r : rows) H += r.kind == RowBig ? bigH : (r.kind == RowGraph || r.kind == RowResultGraph) ? graphH + f.ts * 0.4f : rowH;
    }
    // keep the width steady while numbers change; shrink only when the layout changes
    uint32_t key = s_style.stats ^ ((uint32_t)s_style.layout << 20) ^ ((uint32_t)(s_style.textSize * 4) << 22) ^ (s_result ? 1u << 31 : 0) ^ (s_moving ? 1u << 30 : 0);
    if (key != s_stickyKey) { s_stickyKey = key; s_stickyW = 0; }
    s_stickyW = std::max(s_stickyW, std::ceil(W));
    W = s_stickyW;
    H = std::ceil(H);
    if (!s_panel.Ensure((int)W, (int)H)) return;

    Canvas c{s_panel.rt, s_panel.br};
    s_panel.rt->BeginDraw();
    s_panel.rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    D2D1_RECT_F box = D2D1::RectF(0, 0, W, H);
    // while moving, a minimum of background so the whole panel can be grabbed
    c.FillRound(box, 8 * sc, Rgb(s_style.bgColor, s_moving ? std::max(0.45f, s_style.opacity) : s_style.opacity));
    if (s_moving) {
        ID2D1StrokeStyle* dash = nullptr;
        D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties();
        sp.dashStyle = D2D1_DASH_STYLE_DASH;
        d2d->CreateStrokeStyle(sp, nullptr, 0, &dash);
        c.br->SetColor(ToneColor(Accent));
        c.rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(1, 1, W - 1, H - 1), 8 * sc, 8 * sc), c.br, 2, dash);
        SafeRelease(dash);
    }
    bool shadow = s_style.opacity < 0.35f;
    float y = compact ? pad * 0.7f : pad;
    float vx = pad + (labelW > 0 ? labelW + gapX : 0);
    for (auto& r : rows) {
        switch (r.kind) {
            case RowBanner:
                DrawSegs(c, r.segs, f.banner, f.banner, pad, y, rowH, shadow);
                y += rowH;
                break;
            case RowBig:
                if (!r.label.empty()) DrawSegs(c, {{r.label, Accent}}, f.label, f.label, pad, y + bigH * 0.12f, bigH, shadow);
                DrawSegs(c, r.segs, f.big, f.label, s_style.layout == Layout::Minimal ? pad : vx, y, bigH, shadow);
                y += bigH;
                break;
            case RowGraph:
            case RowResultGraph: {
                D2D1_RECT_F g = D2D1::RectF(pad, y + f.ts * 0.15f, W - pad, y + f.ts * 0.15f + graphH);
                if (r.kind == RowGraph) DrawGraph(c, g, g_app.live.recent, true);
                else DrawGraph(c, g, s_resultGraph, true);
                y += graphH + f.ts * 0.4f;
                break;
            }
            default:
                if (compact) DrawSegs(c, r.segs, f.value, f.label, pad, y, rowH, shadow);
                else {
                    if (!r.label.empty()) DrawSegs(c, {{r.label, Accent}}, f.label, f.label, pad, y, rowH, shadow);
                    DrawSegs(c, r.segs, f.value, f.label, vx, y, rowH, shadow);
                }
                y += rowH;
                break;
        }
    }
    HRESULT hr = s_panel.rt->EndDraw();
    FrameDone();
    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) { s_panel.Discard(); return; }
    if (!s_dragging) PositionFor(W, H, s_px, s_py);
    s_panel.Present(s_px, s_py);
    // stay above games that make themselves topmost
    if (GetTickCount64() - s_lastTopmost > 2000) {
        s_lastTopmost = GetTickCount64();
        SetWindowPos(s_panel.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        if (s_crossVisible) SetWindowPos(s_cross.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
}

// ---------------------------------------------------------------- crosshair
static bool LoadCrossImage(ID2D1RenderTarget* rt, const std::wstring& path) {
    SafeRelease(s_crossBmp);
    if (path.empty()) return false;
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    bool ok = SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec)) &&
              SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
              SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
              SUCCEEDED(rt->CreateBitmapFromWicBitmap(conv, nullptr, &s_crossBmp));
    SafeRelease(conv); SafeRelease(frame); SafeRelease(dec); SafeRelease(wic);
    return ok;
}

static int CrossSize() {
    const CrosshairStyle& x = g_cfg.cross;
    float sc = Scale();
    if (x.shape == CrossShape::Image && s_crossBmp) {
        D2D1_SIZE_F s = s_crossBmp->GetSize();
        return (int)std::ceil(std::max(s.width, s.height) * x.imageScale * sc) + 4;
    }
    float ext = (x.size + x.gap + x.thickness + (x.outline ? x.outlineWidth : 0)) * sc;
    return (int)std::ceil(ext * 2) + 6;
}

static void RenderCross() {
    if (!s_crossVisible || !s_cross.hwnd) return;
    const CrosshairStyle& x = g_cfg.cross;
    if (x.shape == CrossShape::Image && !s_crossBmp) {
        s_cross.Ensure(8, 8);
        LoadCrossImage(s_cross.rt, x.image);
    }
    int S = CrossSize();
    if (S % 2 == 0) ++S;   // an odd size has a true center pixel
    if (!s_cross.Ensure(S, S)) return;
    Canvas c{s_cross.rt, s_cross.br};
    float sc = Scale(), cx = S / 2.0f, cy = S / 2.0f;
    float t = std::max(1.0f, std::round(x.thickness * sc)), len = std::round(x.size * sc), gap = std::round(x.gap * sc);
    float ow = x.outline ? std::max(1.0f, std::round(x.outlineWidth * sc)) : 0;
    D2D1_COLOR_F col = Rgb(x.color), oc = Rgb(x.outlineColor);
    s_cross.rt->BeginDraw();
    s_cross.rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    auto bar = [&](float l, float tp, float r, float b) {
        s_cross.rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        if (ow > 0) c.Fill(D2D1::RectF(l - ow, tp - ow, r + ow, b + ow), oc);
        c.Fill(D2D1::RectF(l, tp, r, b), col);
    };
    // center the bars on the middle pixel
    float h0 = std::floor(cx - t / 2), h1 = h0 + t;
    switch (x.shape) {
        case CrossShape::Dot: {
            s_cross.rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            float r = std::max(1.0f, x.size * 0.3f * sc);
            if (ow > 0) c.Circle(cx, cy, r + ow, oc);
            c.Circle(cx, cy, r, col);
            break;
        }
        case CrossShape::Cross:
        case CrossShape::T:
            bar(h0 - gap - len, h0, h0 - gap, h1);               // left
            bar(h1 + gap, h0, h1 + gap + len, h1);               // right
            bar(h0, h1 + gap, h1, h1 + gap + len);               // bottom
            if (x.shape == CrossShape::Cross) bar(h0, h0 - gap - len, h1, h0 - gap);   // top
            break;
        case CrossShape::Circle: {
            s_cross.rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            float r = len + gap;
            D2D1_ELLIPSE e = D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r);
            if (ow > 0) { c.br->SetColor(oc); s_cross.rt->DrawEllipse(e, c.br, t + ow * 2); }
            c.br->SetColor(col);
            s_cross.rt->DrawEllipse(e, c.br, t);
            break;
        }
        case CrossShape::Image:
            if (s_crossBmp) {
                D2D1_SIZE_F bs = s_crossBmp->GetSize();
                float w = bs.width * x.imageScale * sc, h = bs.height * x.imageScale * sc;
                s_cross.rt->DrawBitmap(s_crossBmp, D2D1::RectF(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            }
            break;
    }
    if (x.centerDot && x.shape != CrossShape::Dot && x.shape != CrossShape::Image) {
        float d = std::max(1.0f, std::round(t));
        float l = std::floor(cx - d / 2);
        bar(l, l, l + d, l + d);
    }
    HRESULT hr = s_cross.rt->EndDraw();
    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) { SafeRelease(s_crossBmp); s_cross.Discard(); return; }
    int x0 = (s_ref.left + s_ref.right) / 2 - S / 2, y0 = (s_ref.top + s_ref.bottom) / 2 - S / 2;
    s_cross.Present(x0, y0, (BYTE)std::lround(255 * x.opacity));
}

// ---------------------------------------------------------------- windows
static void FinishMoveGeometry() {
    // decide anchor: snap to a corner when the panel sits in one, else store a relative position
    RECT wr;
    GetWindowRect(s_panel.hwnd, &wr);
    float sc = Scale(), m = 14 * sc, snap = 28 * sc;
    D2D1_RECT_F r = RefRect();
    float w = (float)(wr.right - wr.left), h = (float)(wr.bottom - wr.top);
    bool left = std::fabs(wr.left - (r.left + m)) < snap, right = std::fabs(wr.right - (r.right - m)) < snap;
    bool top = std::fabs(wr.top - (r.top + m)) < snap, bottom = std::fabs(wr.bottom - (r.bottom - m)) < snap;
    if (top && left) s_moveAnchor = Anchor::TopLeft;
    else if (top && right) s_moveAnchor = Anchor::TopRight;
    else if (bottom && left) s_moveAnchor = Anchor::BottomLeft;
    else if (bottom && right) s_moveAnchor = Anchor::BottomRight;
    else s_moveAnchor = Anchor::Custom;
    float rw = std::max(1.0f, r.right - r.left), rh = std::max(1.0f, r.bottom - r.top);
    s_moveX = std::clamp((wr.left - r.left) / rw, 0.0f, std::max(0.0f, 1.0f - w / rw));
    s_moveY = std::clamp((wr.top - r.top) / rh, 0.0f, std::max(0.0f, 1.0f - h / rh));
    s_style.anchor = s_moveAnchor;
    s_style.customX = s_moveX;
    s_style.customY = s_moveY;
}

static LRESULT CALLBACK PanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_NCHITTEST: return s_moving ? HTCLIENT : HTTRANSPARENT;
        case WM_SETCURSOR:
            if (s_moving) { SetCursor(LoadCursorW(nullptr, IDC_SIZEALL)); return TRUE; }
            break;
        case WM_LBUTTONDOWN:
            if (s_moving) {
                POINT p;
                GetCursorPos(&p);
                s_grab = {p.x - s_px, p.y - s_py};
                s_dragging = true;
                SetCapture(hwnd);
            }
            return 0;
        case WM_MOUSEMOVE:
            if (s_dragging) {
                POINT p;
                GetCursorPos(&p);
                int nx = p.x - s_grab.x, ny = p.y - s_grab.y;
                // snap to the corners
                float sc = Scale(), m = 14 * sc, snap = 28 * sc;
                D2D1_RECT_F r = RefRect();
                int w = s_panel.w, h = s_panel.h;
                if (std::fabs(nx - (r.left + m)) < snap) nx = (int)(r.left + m);
                if (std::fabs(nx + w - (r.right - m)) < snap) nx = (int)(r.right - m - w);
                if (std::fabs(ny - (r.top + m)) < snap) ny = (int)(r.top + m);
                if (std::fabs(ny + h - (r.bottom - m)) < snap) ny = (int)(r.bottom - m - h);
                nx = std::max((int)r.left, std::min(nx, (int)r.right - w));
                ny = std::max((int)r.top, std::min(ny, (int)r.bottom - h));
                s_px = nx; s_py = ny;
                SetWindowPos(hwnd, nullptr, nx, ny, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        case WM_LBUTTONUP:
            if (s_dragging) {
                s_dragging = false;
                ReleaseCapture();
                FinishMoveGeometry();
                RenderPanel();
            }
            return 0;
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONUP:
            if (s_moving) EndMove();
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND MakeLayered(HINSTANCE inst, const wchar_t* cls) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = cls == kPanelClass ? PanelProc : DefWindowProcW;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = cls;
    RegisterClassExW(&wc);
    return CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                           cls, cls == kPanelClass ? L"Reticle overlay" : L"Reticle crosshair", WS_POPUP, 0, 0, 1, 1,
                           nullptr, nullptr, inst, nullptr);
}

bool Init(HINSTANCE inst) {
    s_panel.hwnd = MakeLayered(inst, kPanelClass);
    s_cross.hwnd = MakeLayered(inst, kCrossClass);
    return s_panel.hwnd && s_cross.hwnd;
}

void Shutdown() {
    SafeRelease(s_crossBmp);
    s_panel.Free();
    s_cross.Free();
    if (s_panel.hwnd) DestroyWindow(s_panel.hwnd);
    if (s_cross.hwnd) DestroyWindow(s_cross.hwnd);
    s_panel.hwnd = s_cross.hwnd = nullptr;
}

void Show(const RECT& ref, UINT dpi) {
    bool dpiChanged = dpi != s_dpi;
    s_ref = ref;
    s_dpi = dpi ? dpi : 96;
    if (dpiChanged) s_stickyW = 0;
    bool was = s_visible;
    s_visible = true;
    RenderPanel();
    if (!was) ShowWindow(s_panel.hwnd, SW_SHOWNOACTIVATE);
}

void Hide() {
    if (s_moving) EndMove();
    if (!s_visible) return;
    s_visible = false;
    ShowWindow(s_panel.hwnd, SW_HIDE);
    // the bitmap is cheap to recreate; give the memory back while no game is in front
    s_panel.Free();
    s_stickyW = 0;
}

bool Visible() { return s_visible; }
void Redraw() { RenderPanel(); }

void SetStyle(const OverlayStyle& s) {
    if (s_moving) return;   // the move in progress owns the position
    s_style = s;
}

void Toast(const std::wstring& text) {
    s_toast = text;
    s_toastUntil = GetTickCount64() + 1600;
    RenderPanel();
}

void ShowResult(const bench::Run& run, const std::vector<float>& frameMs) {
    s_resultRun = run;
    // downsample to ~200 points, keeping each bucket's worst frame so spikes survive
    s_resultGraph.clear();
    size_t n = frameMs.size(), buckets = std::min<size_t>(200, n);
    for (size_t b = 0; b < buckets; ++b) {
        size_t a = b * n / buckets, e = std::max(a + 1, (b + 1) * n / buckets);
        float mx = 0;
        for (size_t i = a; i < e && i < n; ++i) mx = std::max(mx, frameMs[i]);
        s_resultGraph.push_back(mx);
    }
    s_result = true;
    s_resultUntil = GetTickCount64() + 10000;
    RenderPanel();
}

void BeginMove(std::function<void(Anchor, float, float)> done) {
    s_moveDone = std::move(done);
    s_moving = true;
    s_moveAnchor = s_style.anchor;
    s_moveX = s_style.customX;
    s_moveY = s_style.customY;
    LONG_PTR ex = GetWindowLongPtrW(s_panel.hwnd, GWL_EXSTYLE);
    SetWindowLongPtrW(s_panel.hwnd, GWL_EXSTYLE, ex & ~WS_EX_TRANSPARENT);
    RenderPanel();
}

bool Moving() { return s_moving; }

void EndMove() {
    if (!s_moving) return;
    s_moving = false;
    s_dragging = false;
    LONG_PTR ex = GetWindowLongPtrW(s_panel.hwnd, GWL_EXSTYLE);
    SetWindowLongPtrW(s_panel.hwnd, GWL_EXSTYLE, ex | WS_EX_TRANSPARENT);
    auto done = std::move(s_moveDone);
    s_moveDone = nullptr;
    if (done) done(s_moveAnchor, s_moveX, s_moveY);
    RenderPanel();
}

void ShowCrosshair(const RECT& ref, UINT dpi) {
    static RECT last{};
    static UINT lastDpi = 0;
    if (s_crossVisible && EqualRect(&last, &ref) && lastDpi == dpi) return;
    last = ref;
    lastDpi = dpi;
    if (dpi) s_dpi = dpi;
    s_ref = ref;
    bool was = s_crossVisible;
    s_crossVisible = true;
    RenderCross();
    if (!was) ShowWindow(s_cross.hwnd, SW_SHOWNOACTIVATE);
}

void HideCrosshair() {
    if (!s_crossVisible) return;
    s_crossVisible = false;
    ShowWindow(s_cross.hwnd, SW_HIDE);
}

void CrosshairChanged() {
    SafeRelease(s_crossBmp);
    RenderCross();
}

bool CrosshairVisible() { return s_crossVisible; }

} // namespace overlay
