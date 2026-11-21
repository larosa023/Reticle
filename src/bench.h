// Reticle - saved benchmark runs.
//   DataDir()\benchmarks\<game exe>\<id>.json     summary
//   DataDir()\benchmarks\<game exe>\<id>.frames   frame times, little-endian float32 milliseconds
#pragma once
#include "common.h"
#include "frames.h"

namespace bench {

struct Run {
    std::wstring id;            // yyyymmdd-hhmmss
    std::wstring exe, game;
    long long when = 0;         // unix seconds
    frames::Summary s;
    float avgCpu = -1, avgGpu = -1, maxCpuTemp = -1, maxGpuTemp = -1;
    std::wstring cpu, gpu;
    std::wstring dir;           // folder holding the files
};

bool Save(Run& run, const std::vector<float>& frameMs);
std::vector<Run> List();                       // newest first
bool LoadFrames(const Run& run, std::vector<float>& out);
bool Delete(const Run& run);
bool ExportCsv(const Run& run, const std::wstring& path);
std::wstring Dir();

} // namespace bench
