// Reticle - hardware readings, read the way Ember reads them (without LibreHardwareMonitor's weight):
//   CPU usage        GetSystemTimes
//   CPU temperature  the PawnIO driver (the same signed driver and modules Ember/LibreHardwareMonitor use):
//                    AMD Zen: SMN THM_TCON_CUR_TMP · Intel: IA32_PACKAGE_THERM_STATUS. Needs admin.
//   GPU              NVIDIA's NVML (nvml.dll, installed with every GeForce driver)
//   RAM              GlobalMemoryStatusEx
#pragma once
#include "common.h"

namespace sensors {

struct Reading {
    float cpuUse = -1, cpuTemp = -1;
    float gpuUse = -1, gpuTemp = -1, gpuClock = -1, gpuPower = -1;   // %, °C, MHz, W
    float vramUsed = -1, vramTotal = -1;                             // MB
    float ramUsed = -1, ramTotal = -1;                               // MB
};

enum class CpuTempState { Ok, NeedsAdmin, NeedsDriver, Unsupported, Unknown };

void Init();
void Shutdown();
void Sample(Reading& r);                 // ~once a second
CpuTempState CpuTemp();
bool HasNvidia();
std::wstring GpuName();
std::wstring CpuName();
bool PawnIoInstalled();

} // namespace sensors
