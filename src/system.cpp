// Reticle - permissions and startup (see system.h)
#include "system.h"
#include "util.h"
#include <shellapi.h>
#include <sddl.h>
#include <lm.h>
#include <shlobj.h>

namespace sys {

static const wchar_t* kGrantKey = L"Software\\larosa023\\Reticle";
static const wchar_t* kTask = L"Reticle (larosa023)";
static std::wstring Schtasks() { return KnownFolder(FOLDERID_System) + L"\\schtasks.exe"; }
static std::wstring UserSid();
int HandleGrantPerfLog(const wchar_t* sidText);

bool IsElevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION e{};
    DWORD n = 0;
    bool r = GetTokenInformation(tok, TokenElevation, &e, sizeof(e), &n) && e.TokenIsElevated;
    CloseHandle(tok);
    return r;
}

static PSID PerfLogSid() {
    static BYTE buf[SECURITY_MAX_SID_SIZE];
    DWORD size = sizeof(buf);
    if (!CreateWellKnownSid(WinBuiltinPerfLoggingUsersSid, nullptr, buf, &size)) return nullptr;
    return buf;
}

bool InPerfLogUsers() {
    BOOL member = FALSE;
    PSID sid = PerfLogSid();
    return sid && CheckTokenMembership(nullptr, sid, &member) && member;
}

static std::wstring UserSid() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return L"";
    BYTE buf[256];
    DWORD n = 0;
    std::wstring out;
    if (GetTokenInformation(tok, TokenUser, buf, sizeof(buf), &n)) {
        LPWSTR s = nullptr;
        if (ConvertSidToStringSidW(((TOKEN_USER*)buf)->User.Sid, &s)) { out = s; LocalFree(s); }
    }
    CloseHandle(tok);
    return out;
}

static bool RunElevated(const std::wstring& args, bool wait, DWORD* exitCode) {
    std::wstring exe = ExePath();
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = args.c_str();
    sei.lpDirectory = nullptr;
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) return false;
    if (sei.hProcess) {
        if (wait) {
            WaitForSingleObject(sei.hProcess, 60000);
            if (exitCode) GetExitCodeProcess(sei.hProcess, exitCode);
        }
        CloseHandle(sei.hProcess);
    }
    return true;
}

bool RestartElevated(const std::wstring& args) { return RunElevated(L"--elevated " + args, false, nullptr); }


bool GrantPerfLogUsers() {
    std::wstring sid = UserSid();
    if (sid.empty()) return false;
    DWORD code = 1;
    if (!RunElevated(L"--grant-perflog " + sid, true, &code)) return false;
    if (code == 0) {
        DWORD one = 1;
        RegSetKeyValueW(HKEY_CURRENT_USER, kGrantKey, L"PerfLogGranted", REG_DWORD, &one, sizeof(one));
    }
    return code == 0;
}

bool PerfLogGrantedPendingSignIn() {
    DWORD v = 0, size = sizeof(v);
    bool granted = RegGetValueW(HKEY_CURRENT_USER, kGrantKey, L"PerfLogGranted", RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS && v;
    return granted && !InPerfLogUsers();
}

int HandleGrantPerfLog(const wchar_t* sidText) {
    PSID user = nullptr;
    if (!ConvertStringSidToSidW(sidText, &user)) return 2;
    // the group's name is localized; look it up from its well-known SID
    wchar_t name[256], domain[256];
    DWORD nn = 256, dn = 256;
    SID_NAME_USE use;
    int rc = 3;
    if (LookupAccountSidW(nullptr, PerfLogSid(), name, &nn, domain, &dn, &use)) {
        LOCALGROUP_MEMBERS_INFO_0 m{user};
        NET_API_STATUS st = NetLocalGroupAddMembers(nullptr, name, 0, (LPBYTE)&m, 1);
        rc = (st == NERR_Success || st == ERROR_MEMBER_IN_ALIAS) ? 0 : 4;
    }
    LocalFree(user);
    return rc;
}

// ---------------------------------------------------------------- startup
static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

static bool RunKeySet() {
    wchar_t buf[1024];
    DWORD size = sizeof(buf);
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"Reticle", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS;
}


static bool TaskExists() {
    DWORD code = 1;
    RunAndWait(Schtasks(), std::wstring(L"/Query /TN \"") + kTask + L"\"", 10000, &code, true);
    return code == 0;
}

static int s_taskCached = -1;
void RefreshStartupState() { s_taskCached = TaskExists() ? 1 : 0; }
bool StartupEnabled() { return RunKeySet() || StartupIsAdmin(); }
bool StartupIsAdmin() {
    if (s_taskCached < 0) RefreshStartupState();
    return s_taskCached == 1;
}

static std::wstring XmlEscape(const std::wstring& s);

// A logon task that starts Reticle with highest privileges for this user: no UAC prompt at sign-in,
// and Reticle can also start it on demand ("schtasks /Run") to hand over to an admin copy.
static bool CreateAdminTask(const std::wstring& userSid) {
    if (userSid.empty()) return false;
    std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"  <RegistrationInfo><Description>Starts Reticle with admin rights at sign-in, for frame capture and CPU temperature.</Description></RegistrationInfo>\r\n"
        L"  <Triggers><LogonTrigger><Enabled>true</Enabled><UserId>" + XmlEscape(userSid) + L"</UserId><Delay>PT5S</Delay></LogonTrigger></Triggers>\r\n"
        L"  <Principals><Principal id=\"Author\"><UserId>" + XmlEscape(userSid) + L"</UserId><LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></Principal></Principals>\r\n"
        L"  <Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy><DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
        L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries><ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Priority>7</Priority>"
        L"<IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd><RestartOnIdle>false</RestartOnIdle></IdleSettings></Settings>\r\n"
        L"  <Actions Context=\"Author\"><Exec><Command>" + XmlEscape(ExePath()) + L"</Command><Arguments>--tray --from-task</Arguments></Exec></Actions>\r\n"
        L"</Task>\r\n";
    std::wstring path = DataDir() + L"\\startup-task.xml";
    EnsureDir(DataDir());
    std::string bytes("\xFF\xFE", 2);   // schtasks wants UTF-16 with a BOM
    bytes.append((const char*)xml.data(), xml.size() * sizeof(wchar_t));
    WriteFileAtomic(path, bytes);
    DWORD code = 1;
    RunAndWait(Schtasks(), std::wstring(L"/Create /F /TN \"") + kTask + L"\" /XML \"" + path + L"\"", 15000, &code, true);
    DeleteFileW(path.c_str());
    Log(L"admin startup task: %ls", code == 0 ? L"created" : L"failed");
    return code == 0;
}

bool RunAdminTask() {
    DWORD code = 1;
    RunAndWait(Schtasks(), std::wstring(L"/Run /TN \"") + kTask + L"\"", 15000, &code, true);
    return code == 0;
}

// The elevated side of SetupAdmin: allow frame capture for good and register the admin startup task.
int HandleSetupAdmin(const wchar_t* sid) {
    int a = HandleGrantPerfLog(sid);
    bool t = CreateAdminTask(sid);
    Log(L"setup-admin: perf log users %d, task %d", a, t ? 1 : 0);
    return t ? 0 : (a == 0 ? 5 : 6);
}

bool SetupAdmin() {
    std::wstring sid = UserSid();
    if (sid.empty()) return false;
    DWORD code = 1;
    if (!RunElevated(L"--setup-admin " + sid, true, &code)) return false;   // declined
    if (code == 0 || code == 5) {
        DWORD one = 1;
        RegSetKeyValueW(HKEY_CURRENT_USER, kGrantKey, L"PerfLogGranted", REG_DWORD, &one, sizeof(one));
    }
    RefreshStartupState();
    return code == 0;
}

static std::wstring XmlEscape(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        if (c == L'&') o += L"&amp;"; else if (c == L'<') o += L"&lt;"; else if (c == L'>') o += L"&gt;"; else if (c == L'"') o += L"&quot;"; else o += c;
    }
    return o;
}

bool SetStartup(bool on, bool admin, std::wstring* error) {
    std::wstring cmd = L"\"" + ExePath() + L"\" --tray";
    // the Run entry
    if (on && !admin) {
        if (RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, L"Reticle", REG_SZ, cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t))) != ERROR_SUCCESS) {
            if (error) *error = L"Couldn't add Reticle to startup.";
            return false;
        }
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, L"Reticle");
    }
    // the elevated logon task (creating or deleting it needs admin)
    bool hasTask = TaskExists();
    if (on && admin && !hasTask) {
        if (!IsElevated()) { if (error) *error = L"Restart Reticle as administrator once to turn this on."; return false; }
        if (!CreateAdminTask(UserSid())) { if (error) *error = L"Windows didn't accept the startup task."; return false; }
    } else if ((!on || !admin) && hasTask) {
        DWORD code = 1;
        if (IsElevated()) RunAndWait(Schtasks(), std::wstring(L"/Delete /F /TN \"") + kTask + L"\"", 15000, &code, true);
        if (code != 0) {
            if (error) *error = L"Restart Reticle as administrator to remove the admin startup task.";
            return false;
        }
    }
    RefreshStartupState();
    return true;
}

} // namespace sys
