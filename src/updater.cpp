// Reticle - built-in updater (see updater.h)
#include "updater.h"
#include "net.h"
#include "util.h"
#include "json.h"
#include "config.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <ctime>
#include <shellapi.h>

namespace updater {

struct Asset { std::wstring name, url, sha256; uint64_t size = 0; };
struct Release { std::wstring version, notes; bool prerelease = false; std::vector<Asset> assets; };

static std::atomic<bool> s_busy{false};
static const wchar_t* kAsset = L"Reticle.exe";
static const wchar_t* kSums = L"SHA256SUMS.txt";

static void Post(HWND hwnd, SelfUpdateState* s) {
    if (!hwnd || !PostMessageW(hwnd, WM_RETICLE_SELF, 0, (LPARAM)s)) delete s;
}
static void PostState(HWND hwnd, SelfUpdateState::Kind k, const std::wstring& version = L"", const std::wstring& msg = L"", int pct = -1) {
    auto* s = new SelfUpdateState;
    s->kind = k;
    s->version = version;
    s->message = msg;
    s->percent = pct;
    Post(hwnd, s);
}

static bool ParseReleases(const std::wstring& text, std::vector<Release>& out) {
    json::Value root;
    if (!json::Parser(text).Parse(root) || root.type != json::Value::Array) return false;
    for (const json::Value& r : root.arr) {
        if (r.GetBool(L"draft", false)) continue;
        Release rel;
        rel.version = NormalizeVersion(r.GetStr(L"tag_name"));
        if (rel.version.empty() || !iswdigit(rel.version[0])) continue;
        rel.notes = r.GetStr(L"body");
        rel.prerelease = r.GetBool(L"prerelease", false) || rel.version.find(L'-') != std::wstring::npos;
        if (const json::Value* a = r.Get(L"assets"); a && a->type == json::Value::Array) {
            for (const json::Value& x : a->arr) {
                Asset as;
                as.name = x.GetStr(L"name");
                as.url = x.GetStr(L"browser_download_url");
                as.size = (uint64_t)x.GetNum(L"size", 0);
                std::wstring d = x.GetStr(L"digest");
                if (IStartsWith(d, L"sha256:")) as.sha256 = Lower(d.substr(7));
                if (!as.name.empty() && !as.url.empty()) rel.assets.push_back(as);
            }
        }
        out.push_back(rel);
    }
    std::sort(out.begin(), out.end(), [](const Release& a, const Release& b) { return CompareVersions(a.version, b.version) > 0; });
    return true;
}

static const Asset* FindAsset(const Release& r, const std::wstring& name) {
    for (auto& a : r.assets) if (IEquals(a.name, name)) return &a;
    return nullptr;
}

// Hangar's release index (one file for the whole suite, refreshed every 10 minutes by a workflow)
// keeps every app clear of GitHub's 60-requests-an-hour limit. The API is the fallback.
static bool FromIndex(std::vector<Release>& out, bool force) {
    net::Response r;
    std::wstring cache = CacheDir() + L"\\index.json", etagFile = CacheDir() + L"\\index.etag", etag;
    ReadTextFile(etagFile, etag);
    std::string body;
    if (!net::Get(L"https://raw.githubusercontent.com/larosa023/Hangar/index/releases.json", r, Trim(etag))) return false;
    if (r.status == 304) { if (!ReadFileBytes(cache, body)) return false; }
    else if (r.status == 200) { body = r.body; EnsureDir(CacheDir()); WriteFileAtomic(cache, body); WriteTextFileAtomic(etagFile, r.etag); }
    else return false;
    json::Value v;
    if (!json::Parser(Utf8ToWide(body)).Parse(v) || v.type != json::Value::Object) return false;
    long long generated = (long long)v.GetNum(L"generated", 0);
    // the index refreshes about hourly in practice: a manual check or a stale index asks GitHub directly
    long long age = generated ? (long long)time(nullptr) - generated : 0;
    if (age > (force ? 10 * 60 : 45 * 60)) return false;
    const json::Value* repos = v.Get(L"repos");
    const json::Value* rels = repos ? repos->Get(L"larosa023/reticle") : nullptr;
    if (!rels || rels->type != json::Value::Array) return false;
    std::wstring text;
    json::Write(*rels, text);
    return ParseReleases(text, out) && !out.empty();
}

static std::wstring HashFromSums(const std::string& sums, const std::wstring& assetName) {
    std::wstring text = Utf8ToWide(sums);
    size_t i = 0;
    while (i < text.size()) {
        size_t e = text.find(L'\n', i);
        if (e == std::wstring::npos) e = text.size();
        std::wstring line = Trim(text.substr(i, e - i));
        i = e + 1;
        size_t sp = line.find_first_of(L" \t");
        if (sp == std::wstring::npos) continue;
        std::wstring hash = Lower(line.substr(0, sp)), name = Trim(line.substr(sp));
        if (!name.empty() && name[0] == L'*') name.erase(0, 1);
        if (hash.size() == 64 && IEquals(name, assetName)) return hash;
    }
    return L"";
}

void Check(HWND notify, bool force, bool download) {
    if (s_busy.exchange(true)) return;
    bool beta = g_cfg.beta;
    std::thread([notify, force, download, beta] {
        PostState(notify, SelfUpdateState::Checking);
        std::vector<Release> rels;
        std::wstring err;
        if (!FromIndex(rels, force)) {
            rels.clear();
            net::Response r;
            if (!net::Get(net::ApiBase() + L"/repos/larosa023/Reticle/releases?per_page=10", r, L"", true)) err = r.error;
            else if (r.status == 403 || r.status == 429) err = L"GitHub's hourly limit was reached. Reticle will try again later.";
            else if (r.status != 200) err = L"GitHub answered with HTTP " + std::to_wstring(r.status) + L".";
            else if (!ParseReleases(Utf8ToWide(r.body), rels)) err = L"GitHub sent an unexpected reply.";
        }
        if (!err.empty()) { Log(L"update check: %ls", err.c_str()); PostState(notify, SelfUpdateState::Error, L"", err); s_busy = false; return; }

        const Release* best = nullptr;
        for (auto& rel : rels) {
            if (rel.prerelease && !beta) continue;
            if (!force && IsSkippedVersion(rel.version)) continue;
            if (!FindAsset(rel, kAsset)) continue;
            best = &rel;
            break;
        }
        if (!best || CompareVersions(best->version, RETICLE_VERSION) <= 0) {
            PostState(notify, SelfUpdateState::UpToDate);
            s_busy = false;
            return;
        }
        const Release rel = *best;
        const Asset* asset = FindAsset(rel, kAsset);
        Log(L"update: %ls available", rel.version.c_str());
        if (!download) {
            auto* s = new SelfUpdateState;
            s->kind = SelfUpdateState::Available;
            s->version = rel.version;
            s->notes = rel.notes;
            Post(notify, s);
            s_busy = false;
            return;
        }
        std::wstring expected = asset->sha256;
        if (const Asset* sums = FindAsset(rel, kSums)) {
            net::Response sr;
            if (net::Get(sums->url, sr) && sr.status == 200) {
                std::wstring h = HashFromSums(sr.body, asset->name);
                if (!h.empty() && !expected.empty() && h != expected) {
                    PostState(notify, SelfUpdateState::Error, rel.version, L"The checksums for Reticle " + rel.version + L" don't agree. Not installing it.");
                    s_busy = false;
                    return;
                }
                if (!h.empty()) expected = h;
            }
        }
        if (expected.empty()) {
            PostState(notify, SelfUpdateState::Error, rel.version, L"Reticle " + rel.version + L" has no checksum, so it can't be verified.");
            s_busy = false;
            return;
        }
        std::wstring path = DataDir() + L"\\updates\\" + rel.version + L"\\Reticle.exe";
        bool ok = FileExists(path) && Sha256File(path) == expected;
        if (!ok) {
            int last = -2;
            ok = net::Download(asset->url, path, [&](uint64_t done, uint64_t total) {
                if (!total) total = asset->size;
                int pct = total ? (int)(done * 100 / total) : -1;
                if (pct != last) { last = pct; PostState(notify, SelfUpdateState::Downloading, rel.version, L"", pct); }
                return true;
            }, &err);
            if (ok && Sha256File(path) != expected) { DeleteFileW(path.c_str()); ok = false; err = L"The download didn't match its checksum."; }
        }
        if (!ok) {
            Log(L"update download: %ls", err.c_str());
            PostState(notify, SelfUpdateState::Error, rel.version, err);
            s_busy = false;
            return;
        }
        Log(L"update: %ls downloaded and verified", rel.version.c_str());
        auto* s = new SelfUpdateState;
        s->kind = SelfUpdateState::Ready;
        s->version = rel.version;
        s->notes = rel.notes;
        s->staged = path;
        Post(notify, s);
        s_busy = false;
    }).detach();
}

bool Apply(const std::wstring& newExe, const std::wstring& version, std::wstring* error) {
    std::wstring exe = ExePath(), old = exe + L".old";
    DeleteFileW(old.c_str());
    if (!MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        *error = L"Reticle can't update itself in " + ExeDir() + L" (no permission). Move Reticle.exe to a folder you own.";
        return false;
    }
    if (!CopyFileW(newExe.c_str(), exe.c_str(), FALSE)) {
        MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);
        *error = L"Couldn't put the new Reticle.exe in place.";
        return false;
    }
    wchar_t evName[96];
    swprintf(evName, 96, L"Local\\Reticle.larosa023.Healthy.%lu", GetCurrentProcessId());
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, evName);
    ReleaseInstanceMutex();
    wchar_t args[256];
    swprintf(args, 256, L"--after-update %ls --healthy-event %ls --old-pid %lu", RETICLE_VERSION, evName, GetCurrentProcessId());
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    bool started = CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE, 0, nullptr, ExeDir().c_str(), &si, &pi) != 0;
    bool healthy = false;
    if (started) {
        CloseHandle(pi.hThread);
        HANDLE waits[2] = {ev, pi.hProcess};
        DWORD w = WaitForMultipleObjects(2, waits, FALSE, 20000);
        healthy = w == WAIT_OBJECT_0;
        if (!healthy) {
            Log(L"update: Reticle %ls didn't start (wait %lu); rolling back", version.c_str(), w);
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
        }
        CloseHandle(pi.hProcess);
    }
    if (ev) CloseHandle(ev);
    if (healthy) { Log(L"update: handed over to Reticle %ls", version.c_str()); return true; }
    DeleteFileW(exe.c_str());
    MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);
    ReacquireInstanceMutex();
    *error = L"Reticle " + version + L" didn't start, so you're still on " + RETICLE_VERSION + L". Reticle will skip that version.";
    return false;
}

AfterUpdate ParseArgs(const wchar_t* cmdLine) {
    AfterUpdate a;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdLine && *cmdLine ? cmdLine : L"x", &argc);
    if (!argv) return a;
    for (int i = 0; i + 1 < argc; ++i) {
        std::wstring k = argv[i];
        if (k == L"--after-update") { a.active = true; a.fromVersion = argv[i + 1]; }
        else if (k == L"--healthy-event") a.healthyEvent = argv[i + 1];
        else if (k == L"--old-pid") a.oldPid = (DWORD)_wtoi(argv[i + 1]);
    }
    LocalFree(argv);
    return a;
}

void SignalHealthy(const AfterUpdate& a) {
    if (!a.active || a.healthyEvent.empty()) return;
    if (HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, a.healthyEvent.c_str())) { SetEvent(ev); CloseHandle(ev); }
}

void FinishAfterUpdate(const AfterUpdate& a) {
    if (!a.active) return;
    AfterUpdate copy = a;
    std::thread([copy] {
        if (copy.oldPid) {
            if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, copy.oldPid)) { WaitForSingleObject(h, 15000); CloseHandle(h); }
        }
        std::wstring old = ExePath() + L".old";
        for (int i = 0; i < 10 && FileExists(old); ++i) { if (DeleteFileW(old.c_str())) break; Sleep(1000); }
        if (FileExists(old)) MoveFileExW(old.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        // staged downloads are no longer needed
        std::wstring updates = DataDir() + L"\\updates";
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW((updates + L"\\*").c_str(), &fd);
        if (f != INVALID_HANDLE_VALUE) {
            do {
                std::wstring n = fd.cFileName;
                if (n == L"." || n == L".." || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                DeleteFileW((updates + L"\\" + n + L"\\Reticle.exe").c_str());
                RemoveDirectoryW((updates + L"\\" + n).c_str());
            } while (FindNextFileW(f, &fd));
            FindClose(f);
        }
        Log(L"after update from %ls: cleaned up", copy.fromVersion.c_str());
    }).detach();
}

} // namespace updater
