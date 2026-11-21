// Reticle - hardware readings (see sensors.h)
#include "sensors.h"
#include "util.h"
#include <intrin.h>
#include <shlobj.h>

namespace sensors {

// ---------------------------------------------------------------- CPU usage
static ULONGLONG s_idle = 0, s_kernel = 0, s_user = 0;
static ULONGLONG Ft(const FILETIME& f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }

static float CpuUsage() {
    FILETIME i, k, u;
    if (!GetSystemTimes(&i, &k, &u)) return -1;
    ULONGLONG idle = Ft(i), kernel = Ft(k), user = Ft(u);
    float pct = -1;
    if (s_kernel) {
        ULONGLONG di = idle - s_idle, dk = kernel - s_kernel, du = user - s_user;
        ULONGLONG total = dk + du;   // kernel time includes idle
        if (total > 0) pct = (float)(100.0 * (double)(total - di) / (double)total);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
    }
    s_idle = idle; s_kernel = kernel; s_user = user;
    return pct;
}

// ---------------------------------------------------------------- PawnIO (CPU temperature)
static HANDLE s_pawn = INVALID_HANDLE_VALUE;
static enum { VendorUnknown, VendorAmd, VendorIntel } s_vendor = VendorUnknown;
static int s_family = 0;
static CpuTempState s_tempState = CpuTempState::Unknown;
static float s_tjMax = 100;
static std::wstring s_cpuName;

constexpr DWORD kPawnDeviceType = 41394u << 16;
constexpr DWORD kIoctlLoad = kPawnDeviceType | (0x821 << 2);
constexpr DWORD kIoctlExecute = kPawnDeviceType | (0x841 << 2);

static bool IsElevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION e{};
    DWORD n = 0;
    bool r = GetTokenInformation(tok, TokenElevation, &e, sizeof(e), &n) && e.TokenIsElevated;
    CloseHandle(tok);
    return r;
}

bool PawnIoInstalled() {
    HKEY k;
    const wchar_t* path = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PawnIO";
    for (REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ | view, &k) == ERROR_SUCCESS) { RegCloseKey(k); return true; }
    }
    return false;
}

static bool PawnExecute(const char* fn, const int64_t* in, int nIn, int64_t* out, int nOut) {
    if (s_pawn == INVALID_HANDLE_VALUE) return false;
    unsigned char buf[32 + 8 * 4] = {};
    strncpy((char*)buf, fn, 31);
    memcpy(buf + 32, in, sizeof(int64_t) * nIn);
    DWORD read = 0;
    if (!DeviceIoControl(s_pawn, kIoctlExecute, buf, 32 + 8 * nIn, out, 8 * nOut, &read, nullptr)) return false;
    return read >= (DWORD)(8 * nOut);
}

static void DetectCpu() {
    int r[4] = {};
    __cpuid(r, 0);
    char vendor[13] = {};
    memcpy(vendor, &r[1], 4); memcpy(vendor + 4, &r[3], 4); memcpy(vendor + 8, &r[2], 4);
    if (!strcmp(vendor, "AuthenticAMD")) s_vendor = VendorAmd;
    else if (!strcmp(vendor, "GenuineIntel")) s_vendor = VendorIntel;
    __cpuid(r, 1);
    int base = (r[0] >> 8) & 0xF;
    s_family = base == 0xF ? base + ((r[0] >> 20) & 0xFF) : base;
    wchar_t name[256] = {};
    DWORD size = sizeof(name);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString",
                     RRF_RT_REG_SZ, nullptr, name, &size) == ERROR_SUCCESS) s_cpuName = Trim(name);
}

static void OpenPawn() {
    bool supported = (s_vendor == VendorAmd && s_family >= 0x17) || s_vendor == VendorIntel;
    if (!supported) { s_tempState = CpuTempState::Unsupported; return; }
    if (!PawnIoInstalled()) { s_tempState = CpuTempState::NeedsDriver; return; }
    if (!IsElevated()) { s_tempState = CpuTempState::NeedsAdmin; return; }
    s_pawn = CreateFileW(L"\\\\?\\GLOBALROOT\\Device\\PawnIO", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (s_pawn == INVALID_HANDLE_VALUE) {
        Log(L"PawnIO: couldn't open the driver (%lu)", GetLastError());
        s_tempState = GetLastError() == ERROR_ACCESS_DENIED ? CpuTempState::NeedsAdmin : CpuTempState::NeedsDriver;
        return;
    }
    // the signed module for this CPU, embedded as a resource (200: AMD family 17h+, 201: Intel MSR)
    int id = s_vendor == VendorAmd ? 200 : 201;
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /*RT_RCDATA*/);
    HGLOBAL h = res ? LoadResource(nullptr, res) : nullptr;
    void* data = h ? LockResource(h) : nullptr;
    DWORD size = res ? SizeofResource(nullptr, res) : 0;
    DWORD ret = 0;
    if (!data || !DeviceIoControl(s_pawn, kIoctlLoad, data, size, nullptr, 0, &ret, nullptr)) {
        Log(L"PawnIO: module load failed (%lu)", GetLastError());
        CloseHandle(s_pawn);
        s_pawn = INVALID_HANDLE_VALUE;
        s_tempState = CpuTempState::Unsupported;
        return;
    }
    if (s_vendor == VendorIntel) {
        int64_t in = 0x1A2, out = 0;   // IA32_TEMPERATURE_TARGET
        if (PawnExecute("ioctl_read_msr", &in, 1, &out, 1)) {
            float tj = (float)((out >> 16) & 0xFF);
            if (tj >= 60 && tj <= 130) s_tjMax = tj;
        }
    }
    s_tempState = CpuTempState::Ok;
    Log(L"PawnIO: CPU temperature available (%ls)", s_vendor == VendorAmd ? L"AMD" : L"Intel");
}

static float ReadCpuTemp() {
    if (s_tempState != CpuTempState::Ok) return -1;
    if (s_vendor == VendorAmd) {
        int64_t in = 0x00059800, out = 0;   // THM_TCON_CUR_TMP
        if (!PawnExecute("ioctl_read_smn", &in, 1, &out, 1)) return -1;
        uint32_t v = (uint32_t)out;
        bool offset49 = (v & 0x80000) != 0 || (v & 0x30000) == 0x30000;
        float t = (float)((v >> 21) * 125) * 0.001f;
        if (offset49) t -= 49.0f;
        // Tctl offsets of early Zen parts (k10temp)
        if (s_cpuName.find(L"1600X") != std::wstring::npos || s_cpuName.find(L"1700X") != std::wstring::npos ||
            s_cpuName.find(L"1800X") != std::wstring::npos) t -= 20;
        else if (s_cpuName.find(L"2700X") != std::wstring::npos) t -= 10;
        else if (s_cpuName.find(L"Threadripper 19") != std::wstring::npos || s_cpuName.find(L"Threadripper 29") != std::wstring::npos) t -= 27;
        return t > 0 && t < 150 ? t : -1;
    }
    int64_t in = 0x1B1, out = 0;   // IA32_PACKAGE_THERM_STATUS
    if (!PawnExecute("ioctl_read_msr", &in, 1, &out, 1)) return -1;
    uint32_t eax = (uint32_t)out;
    if (!(eax & 0x80000000u)) return -1;
    return s_tjMax - (float)((eax >> 16) & 0x7F);
}

// ---------------------------------------------------------------- NVML
typedef int (*NvInit)();
typedef int (*NvShutdown)();
typedef int (*NvCount)(unsigned*);
typedef int (*NvHandle)(unsigned, void**);
struct NvUtil { unsigned gpu, memory; };
struct NvMem { unsigned long long total, free, used; };
typedef int (*NvUtilFn)(void*, NvUtil*);
typedef int (*NvTempFn)(void*, int, unsigned*);
typedef int (*NvClockFn)(void*, int, unsigned*);
typedef int (*NvPowerFn)(void*, unsigned*);
typedef int (*NvMemFn)(void*, NvMem*);
typedef int (*NvNameFn)(void*, char*, unsigned);

static HMODULE s_nvml = nullptr;
static void* s_dev = nullptr;
static NvUtilFn s_util; static NvTempFn s_temp; static NvClockFn s_clock; static NvPowerFn s_power; static NvMemFn s_mem;
static NvShutdown s_nvShutdown;
static std::wstring s_gpuName;

static void InitNvml() {
    s_nvml = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!s_nvml) {
        std::wstring p = ExpandEnv(L"%ProgramFiles%\\NVIDIA Corporation\\NVSMI\\nvml.dll");
        s_nvml = LoadLibraryExW(p.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    }
    if (!s_nvml) return;
    auto init = (NvInit)GetProcAddress(s_nvml, "nvmlInit_v2");
    auto count = (NvCount)GetProcAddress(s_nvml, "nvmlDeviceGetCount_v2");
    auto handle = (NvHandle)GetProcAddress(s_nvml, "nvmlDeviceGetHandleByIndex_v2");
    s_util = (NvUtilFn)GetProcAddress(s_nvml, "nvmlDeviceGetUtilizationRates");
    s_temp = (NvTempFn)GetProcAddress(s_nvml, "nvmlDeviceGetTemperature");
    s_clock = (NvClockFn)GetProcAddress(s_nvml, "nvmlDeviceGetClockInfo");
    s_power = (NvPowerFn)GetProcAddress(s_nvml, "nvmlDeviceGetPowerUsage");
    s_mem = (NvMemFn)GetProcAddress(s_nvml, "nvmlDeviceGetMemoryInfo");
    s_nvShutdown = (NvShutdown)GetProcAddress(s_nvml, "nvmlShutdown");
    auto name = (NvNameFn)GetProcAddress(s_nvml, "nvmlDeviceGetName");
    if (!init || !count || !handle || init() != 0) { FreeLibrary(s_nvml); s_nvml = nullptr; return; }
    unsigned n = 0;
    count(&n);
    // the card with the most memory is the one games use
    unsigned long long bestMem = 0;
    for (unsigned i = 0; i < n; ++i) {
        void* d = nullptr;
        if (handle(i, &d) != 0) continue;
        NvMem m{};
        unsigned long long total = (s_mem && s_mem(d, &m) == 0) ? m.total : 1;
        if (!s_dev || total > bestMem) { s_dev = d; bestMem = total; }
    }
    if (s_dev && name) {
        char buf[96] = {};
        if (name(s_dev, buf, sizeof(buf)) == 0) s_gpuName = Utf8ToWide(buf);
    }
    Log(L"NVML: %u GPU(s)%ls%ls", n, s_gpuName.empty() ? L"" : L", using ", s_gpuName.c_str());
}

// ---------------------------------------------------------------- public
void Init() {
    DetectCpu();
    OpenPawn();
    InitNvml();
    CpuUsage();   // prime the delta
}

void Shutdown() {
    if (s_pawn != INVALID_HANDLE_VALUE) CloseHandle(s_pawn);
    s_pawn = INVALID_HANDLE_VALUE;
    if (s_nvml) {
        if (s_nvShutdown) s_nvShutdown();
        FreeLibrary(s_nvml);
        s_nvml = nullptr;
    }
}

void Sample(Reading& r) {
    r = Reading{};
    r.cpuUse = CpuUsage();
    r.cpuTemp = ReadCpuTemp();
    MEMORYSTATUSEX ms{sizeof(ms)};
    if (GlobalMemoryStatusEx(&ms)) {
        r.ramTotal = (float)(ms.ullTotalPhys / (1024.0 * 1024.0));
        r.ramUsed = (float)((ms.ullTotalPhys - ms.ullAvailPhys) / (1024.0 * 1024.0));
    }
    if (s_dev) {
        NvUtil u{};
        if (s_util && s_util(s_dev, &u) == 0) r.gpuUse = (float)u.gpu;
        unsigned v = 0;
        if (s_temp && s_temp(s_dev, 0 /*NVML_TEMPERATURE_GPU*/, &v) == 0) r.gpuTemp = (float)v;
        if (s_clock && s_clock(s_dev, 0 /*NVML_CLOCK_GRAPHICS*/, &v) == 0) r.gpuClock = (float)v;
        if (s_power && s_power(s_dev, &v) == 0) r.gpuPower = v / 1000.0f;
        NvMem m{};
        if (s_mem && s_mem(s_dev, &m) == 0) {
            r.vramTotal = (float)(m.total / (1024.0 * 1024.0));
            r.vramUsed = (float)(m.used / (1024.0 * 1024.0));
        }
    }
}

CpuTempState CpuTemp() { return s_tempState; }
bool HasNvidia() { return s_dev != nullptr; }
std::wstring GpuName() { return s_gpuName; }
std::wstring CpuName() { return s_cpuName; }

} // namespace sensors
