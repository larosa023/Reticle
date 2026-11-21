// Reticle - saved benchmark runs
#include "bench.h"
#include "util.h"
#include "json.h"
#include <algorithm>
#include <ctime>

namespace bench {

std::wstring Dir() { return DataDir() + L"\\benchmarks"; }

static std::wstring SafeName(std::wstring s) {
    for (auto& c : s) if (wcschr(L"\\/:*?\"<>|", c)) c = L'_';
    return s.empty() ? L"game" : s;
}

bool Save(Run& run, const std::vector<float>& frameMs) {
    if (frameMs.empty()) return false;
    time_t now = time(nullptr);
    tm lt{};
    localtime_s(&lt, &now);
    wchar_t id[32];
    swprintf(id, 32, L"%04d%02d%02d-%02d%02d%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);
    run.id = id;
    run.when = (long long)now;
    run.s = frames::Summarize(frameMs);
    run.dir = Dir() + L"\\" + SafeName(run.exe);
    EnsureDir(run.dir);
    json::Value v = json::Value::MakeObject();
    v.Set(L"schema", json::Value::Num(1));
    v.Set(L"exe", json::Value::Str(run.exe));
    v.Set(L"game", json::Value::Str(run.game));
    v.Set(L"when", json::Value::Num((double)run.when));
    v.Set(L"seconds", json::Value::Num(run.s.seconds));
    v.Set(L"frames", json::Value::Num((double)run.s.frames));
    v.Set(L"avgFps", json::Value::Num(run.s.avgFps));
    v.Set(L"low1", json::Value::Num(run.s.low1));
    v.Set(L"low01", json::Value::Num(run.s.low01));
    v.Set(L"minFps", json::Value::Num(run.s.minFps));
    v.Set(L"maxFps", json::Value::Num(run.s.maxFps));
    v.Set(L"avgCpu", json::Value::Num(run.avgCpu));
    v.Set(L"avgGpu", json::Value::Num(run.avgGpu));
    v.Set(L"maxCpuTemp", json::Value::Num(run.maxCpuTemp));
    v.Set(L"maxGpuTemp", json::Value::Num(run.maxGpuTemp));
    v.Set(L"cpu", json::Value::Str(run.cpu));
    v.Set(L"gpu", json::Value::Str(run.gpu));
    v.Set(L"app", json::Value::Str(std::wstring(L"Reticle ") + RETICLE_VERSION));
    std::wstring text;
    json::Write(v, text);
    std::string bin((const char*)frameMs.data(), frameMs.size() * sizeof(float));
    bool ok = WriteFileAtomic(run.dir + L"\\" + run.id + L".frames", bin) &&
              WriteTextFileAtomic(run.dir + L"\\" + run.id + L".json", text);
    Log(L"benchmark saved: %ls %ls (%zu frames, %.1f fps avg)", run.exe.c_str(), run.id.c_str(), run.s.frames, run.s.avgFps);
    return ok;
}

std::vector<Run> List() {
    std::vector<Run> out;
    WIN32_FIND_DATAW gd;
    HANDLE g = FindFirstFileW((Dir() + L"\\*").c_str(), &gd);
    if (g == INVALID_HANDLE_VALUE) return out;
    do {
        std::wstring gname = gd.cFileName;
        if (gname == L"." || gname == L".." || !(gd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::wstring dir = Dir() + L"\\" + gname;
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW((dir + L"\\*.json").c_str(), &fd);
        if (f == INVALID_HANDLE_VALUE) continue;
        do {
            std::wstring text;
            if (!ReadTextFile(dir + L"\\" + fd.cFileName, text)) continue;
            json::Value v;
            if (!json::Parser(text).Parse(v) || v.type != json::Value::Object) continue;
            Run r;
            std::wstring file = fd.cFileName;
            r.id = file.substr(0, file.size() - 5);
            r.dir = dir;
            r.exe = v.GetStr(L"exe", gname);
            r.game = v.GetStr(L"game", r.exe);
            r.when = (long long)v.GetNum(L"when", 0);
            r.s.seconds = v.GetNum(L"seconds", 0);
            r.s.frames = (size_t)v.GetNum(L"frames", 0);
            r.s.avgFps = v.GetNum(L"avgFps", 0);
            r.s.low1 = v.GetNum(L"low1", 0);
            r.s.low01 = v.GetNum(L"low01", 0);
            r.s.minFps = v.GetNum(L"minFps", 0);
            r.s.maxFps = v.GetNum(L"maxFps", 0);
            r.avgCpu = (float)v.GetNum(L"avgCpu", -1);
            r.avgGpu = (float)v.GetNum(L"avgGpu", -1);
            r.maxCpuTemp = (float)v.GetNum(L"maxCpuTemp", -1);
            r.maxGpuTemp = (float)v.GetNum(L"maxGpuTemp", -1);
            r.cpu = v.GetStr(L"cpu");
            r.gpu = v.GetStr(L"gpu");
            out.push_back(r);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    } while (FindNextFileW(g, &gd));
    FindClose(g);
    std::sort(out.begin(), out.end(), [](const Run& a, const Run& b) { return a.when > b.when; });
    return out;
}

bool LoadFrames(const Run& run, std::vector<float>& out) {
    std::string bin;
    out.clear();
    if (!ReadFileBytes(run.dir + L"\\" + run.id + L".frames", bin)) return false;
    out.resize(bin.size() / sizeof(float));
    memcpy(out.data(), bin.data(), out.size() * sizeof(float));
    return !out.empty();
}

bool Delete(const Run& run) {
    bool a = DeleteFileW((run.dir + L"\\" + run.id + L".json").c_str()) != 0;
    DeleteFileW((run.dir + L"\\" + run.id + L".frames").c_str());
    RemoveDirectoryW(run.dir.c_str());   // only succeeds once it's empty
    return a;
}

bool ExportCsv(const Run& run, const std::wstring& path) {
    std::vector<float> f;
    if (!LoadFrames(run, f)) return false;
    std::string csv = "frame,time_s,frame_time_ms,fps\r\n";
    double t = 0;
    char line[96];
    for (size_t i = 0; i < f.size(); ++i) {
        t += f[i] / 1000.0;
        snprintf(line, sizeof(line), "%zu,%.4f,%.3f,%.1f\r\n", i + 1, t, f[i], f[i] > 0 ? 1000.0 / f[i] : 0.0);
        csv += line;
    }
    return WriteFileAtomic(path, csv);
}

} // namespace bench
