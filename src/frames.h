// Reticle - frame timing from ETW, the same events PresentMon reads.
//
// Nothing is injected into games and no graphics API is hooked. A real-time ETW session listens to
// the present events that Windows' graphics stack already emits:
//   Microsoft-Windows-DXGI     Present_Start / PresentMultiplaneOverlay_Start   (D3D10/11/12, and Vulkan/GL on DXGI swapchains)
//   Microsoft-Windows-D3D9     Present_Start                                    (D3D9)
//   Microsoft-Windows-DxgKrnl  PresentHistory_Start / PresentHistoryDetailed_Start (everything else: Vulkan, OpenGL)
// Per process, the highest-level source that's active wins, so a frame is never counted twice.
//
// Windows only lets administrators (and members of "Performance Log Users") start a trace session.
#pragma once
#include "common.h"

namespace frames {

enum class Status { Off, Running, NoPermission, Failed };

bool Start();                       // starts the session (idempotent); false if it couldn't
void Stop();
Status GetStatus();
DWORD LastError();

void SetTarget(DWORD pid);          // the process whose frames feed the overlay and benchmarks
DWORD Target();

struct Live {
    bool valid = false;             // frames arrived in the last second
    double fps = 0, frameMs = 0;
    double low1 = 0, low01 = 0;     // average FPS of the slowest 1% / 0.1% of frames (last 30 s)
    const wchar_t* api = L"";       // "DXGI", "D3D9", "Vulkan/OpenGL"
    std::vector<float> recent;      // the last frame times (ms), oldest first
};
void Snapshot(Live& out, size_t graphFrames);

double PresentRate(DWORD pid);
std::vector<std::pair<DWORD, double>> ActiveProcesses();   // every process presenting right now, with its rate      // presents per second over the last ~second, any process (game detection)

// Benchmarks: every frame time of the target from Start to Stop.
void BenchStart();
std::vector<float> BenchStop();     // frame times in ms
bool Benching();
double BenchElapsed();              // seconds

// Statistics helpers shared with benchmark results
struct Summary { double avgFps = 0, low1 = 0, low01 = 0, minFps = 0, maxFps = 0, seconds = 0; size_t frames = 0; };
Summary Summarize(const std::vector<float>& frameMs);

} // namespace frames
