// Reticle - the overlay and crosshair windows.
//
// Both are separate top-level windows: transparent (per-pixel alpha via UpdateLayeredWindow),
// click-through (WS_EX_TRANSPARENT), always on top, never activated and never in Alt-Tab. They're
// drawn with Direct2D into a small bitmap only when something changed, so a frame costs well under a
// millisecond of CPU and nothing at all on the GPU. Windows composes them over borderless and windowed
// games; an exclusive-fullscreen game owns the screen and hides them.
#pragma once
#include "common.h"
#include "config.h"
#include "bench.h"
#include <functional>

namespace overlay {

bool Init(HINSTANCE inst);
void Shutdown();

// stats panel: `ref` is the game's client rect (screen pixels), `dpi` its monitor DPI
void Show(const RECT& ref, UINT dpi);
void Hide();
bool Visible();
bool ToastActive();
void Redraw();                        // re-render from g_app with the current style
void SetStyle(const OverlayStyle& s); // the style to draw (the game's preset or the default)
void Toast(const std::wstring& text, int ms = 1600); // a short line at the top of the panel
void SetHintOnly(bool on);            // draw only the toast/notes, no stats (the overlay is off for this app)
void ShowResult(const bench::Run& run, const std::vector<float>& frameMs);   // 10-second summary card

// move mode: the panel takes the mouse; dragging moves it and snaps to corners
void BeginMove(std::function<void(Anchor, float, float)> done);
bool Moving();
void EndMove();

// crosshair
void ShowCrosshair(const RECT& ref, UINT dpi);
void HideCrosshair();
void CrosshairChanged();
bool CrosshairVisible();

} // namespace overlay
