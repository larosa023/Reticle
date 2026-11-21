// Reticle - frame timing from ETW (see frames.h)
#include "frames.h"
#include "util.h"
#include <evntrace.h>
#include <evntcons.h>
#include <mutex>
#include <thread>
#include <atomic>
#include <deque>
#include <unordered_map>
#include <algorithm>
#include <cstddef>

#ifndef EVENT_TRACE_USE_MS_FLUSH_TIMER
#define EVENT_TRACE_USE_MS_FLUSH_TIMER 0x00000010
#endif
#ifndef EVENT_FILTER_TYPE_EVENT_ID
#define EVENT_FILTER_TYPE_EVENT_ID 0x80000200
#endif
#ifndef EVENT_ENABLE_PROPERTY_IGNORE_KEYWORD_0
#define EVENT_ENABLE_PROPERTY_IGNORE_KEYWORD_0 0x00000010
#endif

namespace frames {

// Provider GUIDs and event ids (from PresentMon's ETW headers)
static const GUID kDxgi = {0xCA11C036, 0x0102, 0x4A2D, {0xA6, 0xAD, 0xF0, 0x3C, 0xFE, 0xD5, 0xD3, 0xC9}};
static const GUID kD3d9 = {0x783ACA0A, 0x790E, 0x4D7F, {0x84, 0x51, 0xAA, 0x85, 0x05, 0x11, 0xC6, 0xB9}};
static const GUID kDxgKrnl = {0x802EC45A, 0x1E99, 0x4B83, {0x99, 0x20, 0x87, 0xC9, 0x82, 0x77, 0xBA, 0x9D}};
constexpr USHORT kDxgiPresentStart = 0x2a, kDxgiPresentMpoStart = 0x37;
constexpr USHORT kD3d9PresentStart = 0x01;
constexpr USHORT kKrnlPresentHistoryStart = 0xab, kKrnlPresentHistoryDetailedStart = 0xd7;

static const wchar_t* kSessionName = L"Reticle Frame Capture";

struct SessionProps {
    EVENT_TRACE_PROPERTIES p;
    wchar_t name[128];
};

// The EVENT_FILTER_EVENT_ID layout, declared here so older SDK headers don't matter.
struct EventIdFilter {
    BOOLEAN FilterIn;
    UCHAR Reserved;
    USHORT Count;
    USHORT Events[4];
};

// Sources, best first. Within a process the best source seen recently wins.
enum Source : int { SrcNone = 0, SrcKrnlDetailed = 1, SrcKrnl = 2, SrcD3d9 = 3, SrcDxgi = 4 };

struct Frame { int64_t qpc; float ms; };

static std::mutex s_lock;
static std::atomic<Status> s_status{Status::Off};
static std::atomic<DWORD> s_error{0};
static TRACEHANDLE s_session = 0;
static TRACEHANDLE s_trace = INVALID_PROCESSTRACE_HANDLE;
static std::thread s_thread;
static GUID s_sessionGuid{};
static int64_t s_qpf = 1;

// target (guarded by s_lock)
static DWORD s_target = 0;
static int64_t s_lastQpc = 0;                  // last accepted present of the target
static int64_t s_srcSeen[5] = {};              // last time each source was seen for the target
static int s_srcUsed = SrcNone;
static std::deque<Frame> s_frames;             // last ~30 s
static bool s_bench = false;
static int64_t s_benchStart = 0;
static std::vector<float> s_benchFrames;

// presents per second for every process (game detection). Runtime (DXGI/D3D9) and kernel presents
// are counted separately so one frame isn't counted twice; the higher rate wins.
struct Counter { int64_t windowStart = 0; int count = 0; double rate = 0; int64_t last = 0; };
struct Rate { Counter c[2]; int64_t last = 0, krnl = 0; };
static std::unordered_map<DWORD, Rate> s_rates;

Status GetStatus() { return s_status; }
DWORD LastError() { return s_error; }

static void OnPresent(DWORD pid, int src, int64_t qpc) {
    std::lock_guard<std::mutex> g(s_lock);
    Rate& r = s_rates[pid];
    if (src == SrcKrnl) r.krnl = qpc;
    bool countIt = !(src == SrcKrnlDetailed && r.krnl && qpc - r.krnl < s_qpf);
    if (countIt) {
    Counter& c = r.c[src >= SrcD3d9 ? 0 : 1];
    if (!c.windowStart) c.windowStart = qpc;
    ++c.count;
    double el = (double)(qpc - c.windowStart) / s_qpf;
    if (el >= 1.0) { c.rate = c.count / el; c.count = 0; c.windowStart = qpc; }
    c.last = r.last = qpc;
    }
    if (s_rates.size() > 512) {
        // forget processes that stopped presenting
        for (auto it = s_rates.begin(); it != s_rates.end();) {
            if (qpc - it->second.last > s_qpf * 5) it = s_rates.erase(it); else ++it;
        }
    }

    if (pid != s_target || !s_target) return;
    s_srcSeen[src] = qpc;
    // the best source seen in the last second decides
    int best = SrcNone;
    for (int k = SrcDxgi; k > SrcNone; --k) if (s_srcSeen[k] && qpc - s_srcSeen[k] < s_qpf) { best = k; break; }
    if (src != best) return;
    if (best != s_srcUsed) { s_srcUsed = best; s_lastQpc = 0; }
    if (s_lastQpc && qpc > s_lastQpc) {
        float ms = (float)((qpc - s_lastQpc) * 1000.0 / s_qpf);
        if (ms < 5000) {
            s_frames.push_back({qpc, ms});
            if (s_bench) s_benchFrames.push_back(ms);
        }
        while (!s_frames.empty() && (qpc - s_frames.front().qpc > s_qpf * 30 || s_frames.size() > 120000)) s_frames.pop_front();
    }
    s_lastQpc = qpc;
}

static void WINAPI OnEvent(PEVENT_RECORD rec) {
    const EVENT_HEADER& h = rec->EventHeader;
    USHORT id = h.EventDescriptor.Id;
    int src = SrcNone;
    if (IsEqualGUID(h.ProviderId, kDxgi)) { if (id == kDxgiPresentStart || id == kDxgiPresentMpoStart) src = SrcDxgi; }
    else if (IsEqualGUID(h.ProviderId, kD3d9)) { if (id == kD3d9PresentStart) src = SrcD3d9; }
    else if (IsEqualGUID(h.ProviderId, kDxgKrnl)) {
        if (id == kKrnlPresentHistoryStart) src = SrcKrnl;
        else if (id == kKrnlPresentHistoryDetailedStart) src = SrcKrnlDetailed;
    }
    if (src == SrcNone || h.ProcessId == 0 || h.ProcessId == GetCurrentProcessId()) return;
    OnPresent(h.ProcessId, src, h.TimeStamp.QuadPart);
}

static bool IsWin11() {
    using Fn = LONG(WINAPI*)(OSVERSIONINFOW*);
    auto fn = (Fn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    OSVERSIONINFOW vi{sizeof(vi)};
    return fn && fn(&vi) == 0 && vi.dwBuildNumber >= 22000;
}

static ULONG Enable(const GUID& provider, UCHAR level, ULONGLONG any, ULONGLONG all, std::initializer_list<USHORT> ids) {
    EventIdFilter f{};
    f.FilterIn = TRUE;
    for (USHORT id : ids) f.Events[f.Count++] = id;
    EVENT_FILTER_DESCRIPTOR desc{};
    desc.Ptr = (ULONGLONG)(ULONG_PTR)&f;
    desc.Size = (ULONG)(offsetof(EventIdFilter, Events) + sizeof(USHORT) * f.Count);
    desc.Type = EVENT_FILTER_TYPE_EVENT_ID;
    ENABLE_TRACE_PARAMETERS params{};
    params.Version = ENABLE_TRACE_PARAMETERS_VERSION_2;
    params.EnableProperty = EVENT_ENABLE_PROPERTY_IGNORE_KEYWORD_0;
    params.SourceId = s_sessionGuid;
    params.EnableFilterDesc = &desc;
    params.FilterDescCount = 1;
    ULONG st = EnableTraceEx2(s_session, &provider, EVENT_CONTROL_CODE_ENABLE_PROVIDER, level, any, all, 0, &params);
    if (st != ERROR_SUCCESS) {
        // very old builds without event-id filtering: enable unfiltered (the callback filters anyway)
        st = EnableTraceEx2(s_session, &provider, EVENT_CONTROL_CODE_ENABLE_PROVIDER, level, any, all, 0, nullptr);
    }
    return st;
}

static void StopSessionByName() {
    SessionProps sp{};
    sp.p.Wnode.BufferSize = sizeof(sp);
    sp.p.LoggerNameOffset = offsetof(SessionProps, name);
    ControlTraceW(0, kSessionName, &sp.p, EVENT_TRACE_CONTROL_STOP);
}

bool Start() {
    if (s_status == Status::Running) return true;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    s_qpf = f.QuadPart ? f.QuadPart : 1;

    SessionProps sp{};
    sp.p.Wnode.BufferSize = sizeof(sp);
    sp.p.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    sp.p.Wnode.ClientContext = 1;               // QPC timestamps
    sp.p.LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_USE_MS_FLUSH_TIMER;
    sp.p.FlushTimer = 100;                      // deliver events within 100 ms
    sp.p.BufferSize = 16;                       // KB
    sp.p.MinimumBuffers = 8;
    sp.p.MaximumBuffers = 64;
    sp.p.LoggerNameOffset = offsetof(SessionProps, name);
    ULONG st = StartTraceW(&s_session, kSessionName, &sp.p);
    if (st == ERROR_ALREADY_EXISTS) {
        // left over from a Reticle that didn't exit cleanly
        StopSessionByName();
        sp = SessionProps{};
        sp.p.Wnode.BufferSize = sizeof(sp);
        sp.p.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        sp.p.Wnode.ClientContext = 1;
        sp.p.LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_USE_MS_FLUSH_TIMER;
        sp.p.FlushTimer = 100;
        sp.p.BufferSize = 16;
        sp.p.MinimumBuffers = 8;
        sp.p.MaximumBuffers = 64;
        sp.p.LoggerNameOffset = offsetof(SessionProps, name);
        st = StartTraceW(&s_session, kSessionName, &sp.p);
    }
    if (st == ERROR_INVALID_PARAMETER) {
        // Windows 7/8.0 don't know the millisecond flush timer
        sp.p.LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
        sp.p.FlushTimer = 1;
        st = StartTraceW(&s_session, kSessionName, &sp.p);
    }
    if (st != ERROR_SUCCESS) {
        s_session = 0;
        s_error = st;
        s_status = st == ERROR_ACCESS_DENIED ? Status::NoPermission : Status::Failed;
        Log(L"frame capture: StartTrace failed (%lu)", st);
        return false;
    }
    s_sessionGuid = sp.p.Wnode.Guid;

    // keywords as PresentMon uses them; the DxgKrnl "Present" keyword only exists on Windows 11
    ULONGLONG krnl = IsWin11() ? 0x8000001ull : 0x1ull;
    ULONG e1 = Enable(kDxgi, TRACE_LEVEL_INFORMATION, 0x8000000000000002ull, 0x8000000000000002ull, {kDxgiPresentStart, kDxgiPresentMpoStart});
    ULONG e2 = Enable(kD3d9, TRACE_LEVEL_INFORMATION, 0x8000000000000002ull, 0x8000000000000002ull, {kD3d9PresentStart});
    ULONG e3 = Enable(kDxgKrnl, TRACE_LEVEL_INFORMATION, krnl, krnl, {kKrnlPresentHistoryStart, kKrnlPresentHistoryDetailedStart});
    if (e1 || e2 || e3) Log(L"frame capture: enable results dxgi=%lu d3d9=%lu dxgkrnl=%lu", e1, e2, e3);

    EVENT_TRACE_LOGFILEW lf{};
    lf.LoggerName = (LPWSTR)kSessionName;
    lf.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    lf.EventRecordCallback = OnEvent;
    s_trace = OpenTraceW(&lf);
    if (s_trace == INVALID_PROCESSTRACE_HANDLE) {
        DWORD e = GetLastError();
        Log(L"frame capture: OpenTrace failed (%lu)", e);
        SessionProps stop{};
        stop.p.Wnode.BufferSize = sizeof(stop);
        stop.p.LoggerNameOffset = offsetof(SessionProps, name);
        ControlTraceW(s_session, nullptr, &stop.p, EVENT_TRACE_CONTROL_STOP);
        s_session = 0;
        s_error = e;
        s_status = Status::Failed;
        return false;
    }
    TRACEHANDLE th = s_trace;
    s_thread = std::thread([th] {
        TRACEHANDLE h = th;
        ProcessTrace(&h, 1, nullptr, nullptr);   // returns once the trace is closed
    });
    s_status = Status::Running;
    s_error = 0;
    Log(L"frame capture: started");
    return true;
}

void Stop() {
    if (s_status != Status::Running) {
        if (s_status != Status::Off) s_status = Status::Off;
        return;
    }
    SessionProps sp{};
    sp.p.Wnode.BufferSize = sizeof(sp);
    sp.p.LoggerNameOffset = offsetof(SessionProps, name);
    ControlTraceW(s_session, nullptr, &sp.p, EVENT_TRACE_CONTROL_STOP);
    if (s_trace != INVALID_PROCESSTRACE_HANDLE) CloseTrace(s_trace);
    if (s_thread.joinable()) s_thread.join();
    s_trace = INVALID_PROCESSTRACE_HANDLE;
    s_session = 0;
    s_status = Status::Off;
    std::lock_guard<std::mutex> g(s_lock);
    s_rates.clear();
    Log(L"frame capture: stopped");
}

void SetTarget(DWORD pid) {
    std::lock_guard<std::mutex> g(s_lock);
    if (pid == s_target) return;
    s_target = pid;
    s_lastQpc = 0;
    s_srcUsed = SrcNone;
    for (auto& t : s_srcSeen) t = 0;
    s_frames.clear();
}

DWORD Target() {
    std::lock_guard<std::mutex> g(s_lock);
    return s_target;
}

static int64_t Now() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

double PresentRate(DWORD pid) {
    std::lock_guard<std::mutex> g(s_lock);
    auto it = s_rates.find(pid);
    if (it == s_rates.end()) return 0;
    int64_t now = Now();
    double best = 0;
    for (const Counter& c : it->second.c) {
        if (!c.last || now - c.last > s_qpf * 3 / 2) continue;    // stopped presenting
        double r = c.rate;
        if (r <= 0) {
            double el = (double)(now - c.windowStart) / s_qpf;
            r = el > 0.25 ? c.count / el : 0;
        }
        best = std::max(best, r);
    }
    return best;
}

static double LowFps(std::vector<float>& sortedDesc, double fraction) {
    if (sortedDesc.empty()) return 0;
    size_t n = std::max<size_t>(1, (size_t)(sortedDesc.size() * fraction));
    double sum = 0;
    for (size_t i = 0; i < n; ++i) sum += sortedDesc[i];
    double avg = sum / n;
    return avg > 0 ? 1000.0 / avg : 0;
}

Summary Summarize(const std::vector<float>& frameMs) {
    Summary s;
    if (frameMs.empty()) return s;
    double total = 0;
    float mn = frameMs[0], mx = frameMs[0];
    for (float f : frameMs) { total += f; mn = std::min(mn, f); mx = std::max(mx, f); }
    s.frames = frameMs.size();
    s.seconds = total / 1000.0;
    s.avgFps = total > 0 ? frameMs.size() * 1000.0 / total : 0;
    std::vector<float> sorted = frameMs;
    std::sort(sorted.begin(), sorted.end(), std::greater<float>());
    s.low1 = LowFps(sorted, 0.01);
    s.low01 = LowFps(sorted, 0.001);
    s.minFps = mx > 0 ? 1000.0 / mx : 0;
    s.maxFps = mn > 0 ? 1000.0 / mn : 0;
    return s;
}

void Snapshot(Live& out, size_t graphFrames) {
    static std::vector<float> window;
    static int64_t lowsAt = 0;
    static double low1 = 0, low01 = 0;
    out.recent.clear();
    int64_t now = Now();
    std::lock_guard<std::mutex> g(s_lock);
    out.api = s_srcUsed == SrcDxgi ? L"DXGI" : s_srcUsed == SrcD3d9 ? L"D3D9" : s_srcUsed ? L"Vulkan/OpenGL" : L"";
    if (s_frames.empty() || now - s_frames.back().qpc > s_qpf) {
        out.valid = false;
        out.fps = out.frameMs = 0;
        out.low1 = low1;
        out.low01 = low01;
        if (s_frames.empty()) low1 = low01 = 0;
        return;
    }
    out.valid = true;
    // FPS: frames over the last second
    double sum = 0;
    int n = 0;
    double half = 0;
    int nh = 0;
    for (auto it = s_frames.rbegin(); it != s_frames.rend(); ++it) {
        if (now - it->qpc > s_qpf) break;
        sum += it->ms;
        ++n;
        if (now - it->qpc <= s_qpf / 2) { half += it->ms; ++nh; }
    }
    out.fps = sum > 0 ? n * 1000.0 / sum : 0;
    out.frameMs = nh ? half / nh : (n ? sum / n : 0);
    // lows over the last 30 s, recomputed at most once a second
    if (now - lowsAt > s_qpf) {
        lowsAt = now;
        window.clear();
        window.reserve(s_frames.size());
        for (auto& f : s_frames) window.push_back(f.ms);
        std::sort(window.begin(), window.end(), std::greater<float>());
        low1 = LowFps(window, 0.01);
        low01 = LowFps(window, 0.001);
    }
    out.low1 = low1;
    out.low01 = low01;
    size_t k = std::min(graphFrames, s_frames.size());
    out.recent.reserve(k);
    for (size_t i = s_frames.size() - k; i < s_frames.size(); ++i) out.recent.push_back(s_frames[i].ms);
}

void BenchStart() {
    std::lock_guard<std::mutex> g(s_lock);
    s_bench = true;
    s_benchStart = Now();
    s_benchFrames.clear();
    s_benchFrames.reserve(60 * 400);
}

std::vector<float> BenchStop() {
    std::lock_guard<std::mutex> g(s_lock);
    s_bench = false;
    std::vector<float> out;
    out.swap(s_benchFrames);
    return out;
}

bool Benching() {
    std::lock_guard<std::mutex> g(s_lock);
    return s_bench;
}

double BenchElapsed() {
    std::lock_guard<std::mutex> g(s_lock);
    return s_bench ? (double)(Now() - s_benchStart) / s_qpf : 0;
}

} // namespace frames
