#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../common/shared.h"
#include "../common/version.h"
#include "app.h"

constexpr wchar_t kHostExe[] = L"StartMenuExperienceHost.exe";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"StartGrid";
constexpr wchar_t kDllName[] = L"StartGrid.dll";
constexpr wchar_t kExeName[] = L"StartGrid.exe";

void Log(const std::wstring& msg) { AppendLog(L"exe", msg); }

// ------------------------------------------------------------------ output

HANDLE g_out = INVALID_HANDLE_VALUE;

void Print(const std::wstring& text) {
    if (g_out == INVALID_HANDLE_VALUE) {
        return;
    }
    std::wstring line = text + L"\r\n";
    DWORD written;
    WriteConsoleW(g_out, line.c_str(), DWORD(line.size()), &written, nullptr);
}

void AttachParentConsole() {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        g_out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, 0, nullptr);
    }
}

// ----------------------------------------------------------------- helpers

std::wstring SelfPath() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}

std::wstring DirOf(const std::wstring& path) {
    return path.substr(0, path.rfind(L'\\') + 1);
}

HANDLE OpenNamedEvent(const wchar_t* name) {
    return CreateEventW(nullptr, TRUE, FALSE, name);
}

void SignalEvent(const wchar_t* name) {
    if (HANDLE e = OpenNamedEvent(name)) {
        SetEvent(e);
        CloseHandle(e);
    }
}

void ClearEvent(const wchar_t* name) {
    if (HANDLE e = OpenNamedEvent(name)) {
        ResetEvent(e);
        CloseHandle(e);
    }
}

static bool MutexExists(const wchar_t* name) {
    if (HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, name)) {
        CloseHandle(m);
        return true;
    }
    return false;
}

bool WatcherRunning() { return MutexExists(kWatcherMutex); }

bool ActiveRunning() { return MutexExists(kActiveMutex); }

// Waits for the watcher to exit (it holds its mutex while it runs).
static void WaitWatcherExit(DWORD ms) {
    if (HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, kWatcherMutex)) {
        DWORD r = WaitForSingleObject(m, ms);
        if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) {
            ReleaseMutex(m);
        }
        CloseHandle(m);
    }
}

// Waits for the dll to disable itself in Start.
static void WaitInactive(int ms) {
    for (int i = 0; i < ms / 100 && ActiveRunning(); ++i) {
        Sleep(100);
    }
}

std::vector<DWORD> HostPids() {
    std::vector<DWORD> pids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return pids;
    }
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, kHostExe) == 0) {
            pids.push_back(pe.th32ProcessID);
        }
    }
    CloseHandle(snap);
    return pids;
}

bool RunValueSet() {
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr,
                        nullptr, nullptr) == ERROR_SUCCESS;
}

// ------------------------------------------------------------------ attach

bool HasModule(DWORD pid, const wchar_t* name, bool& known) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) {
        known = false;
        return false;
    }
    known = true;
    bool found = false;
    MODULEENTRY32W me{sizeof(me)};
    for (BOOL ok = Module32FirstW(snap, &me); ok && !found; ok = Module32NextW(snap, &me)) {
        found = _wcsicmp(me.szModule, name) == 0;
    }
    CloseHandle(snap);
    return found;
}

HRESULT AttachTo(DWORD pid, const std::wstring& dllPath, HANDLE stop) {
    using InitFn = HRESULT(WINAPI*)(PCWSTR, DWORD, PCWSTR, PCWSTR, CLSID, PCWSTR);
    HMODULE xaml = LoadLibraryExW(L"Windows.UI.Xaml.dll", nullptr,
                                  LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto init = xaml ? reinterpret_cast<InitFn>(
                           GetProcAddress(xaml, "InitializeXamlDiagnosticsEx"))
                     : nullptr;
    if (!init) {
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    }
    // ERROR_NOT_FOUND means "try the next connection name".
    HRESULT hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    int n = 1;
    for (; n <= 10000 && hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND); ++n) {
        if (n % 100 == 0 && WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) {
            return E_ABORT;
        }
        wchar_t name[64];
        swprintf_s(name, L"VisualDiagConnection%d", n);
        hr = init(name, pid, L"", dllPath.c_str(), CLSID_Tap, nullptr);
    }
    wchar_t msg[128];
    swprintf_s(msg, L"Attach pid %lu: 0x%08X (connection %d)", pid, hr, n - 1);
    Log(msg);
    if (FAILED(hr)) {
        return hr;
    }
    // S_OK only queues the load: wait for the dll to show up in the process.
    for (int i = 0; i < 20; ++i) {
        bool known;
        if (HasModule(pid, kDllName, known) || !known) {
            return S_OK;
        }
        if (WaitForSingleObject(stop, 250) == WAIT_OBJECT_0) {
            return E_ABORT;
        }
    }
    Log(L"Dll not loaded in pid " + std::to_wstring(pid));
    return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
}

struct Retry {
    int tries = 0;
    ULONGLONG next = 0;
};

int Watch() {
    // An old watcher may still be exiting (enable asked it to stop).
    HANDLE mutex = nullptr;
    for (int i = 0; i < 50; ++i) {
        mutex = CreateMutexW(nullptr, TRUE, kWatcherMutex);
        if (mutex && GetLastError() != ERROR_ALREADY_EXISTS) {
            break;
        }
        if (mutex) {
            CloseHandle(mutex);
            mutex = nullptr;
        }
        Sleep(200);
    }
    if (!mutex) {
        return 0;
    }
    HANDLE stop = OpenNamedEvent(kStopEvent);
    HANDLE disable = OpenNamedEvent(kDisableEvent);
    ResetEvent(stop);
    std::wstring dllPath = DirOf(SelfPath()) + kDllName;
    Log(L"Watcher started");

    std::set<DWORD> done;  // attached, or given up
    std::map<DWORD, Retry> retries;
    do {
        auto pids = HostPids();
        auto gone = [&](DWORD pid) {
            return std::find(pids.begin(), pids.end(), pid) == pids.end();
        };
        std::erase_if(done, gone);
        std::erase_if(retries, [&](auto& r) { return gone(r.first); });
        if (WaitForSingleObject(disable, 0) == WAIT_OBJECT_0) {
            continue;
        }
        for (DWORD pid : pids) {
            if (done.count(pid)) {
                continue;
            }
            Retry& r = retries[pid];
            if (GetTickCount64() < r.next) {
                continue;
            }
            if (SUCCEEDED(AttachTo(pid, dllPath, stop))) {
                done.insert(pid);
                retries.erase(pid);
            } else if (++r.tries >= 30) {
                Log(L"Giving up on pid " + std::to_wstring(pid));
                done.insert(pid);
                retries.erase(pid);
            } else {
                r.next = GetTickCount64() + 2000;
            }
        }
    } while (WaitForSingleObject(stop, 2000) == WAIT_TIMEOUT);

    Log(L"Watcher stopped");
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
}

// ------------------------------------------------------------------ enable

std::wstring Timestamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t buf[40];
    swprintf_s(buf, L"%04d%02d%02d-%02d%02d%02d-%03d", t.wYear, t.wMonth, t.wDay,
               t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    return buf;
}

// Removes older installed copies. A copy whose exe is running (a watcher) or
// whose dll is loaded (Start) stays.
void CleanBin(const std::wstring& bin, const std::wstring& keep) {
    std::wstring own = DirOf(SelfPath());
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((bin + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        std::wstring name = fd.cFileName;
        std::wstring dir = bin + name + L"\\";
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || name == L"." ||
            name == L".." || name == keep || _wcsicmp(dir.c_str(), own.c_str()) == 0) {
            continue;
        }
        HANDLE e = CreateFileW((dir + kExeName).c_str(), DELETE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (e == INVALID_HANDLE_VALUE) {
            if (GetLastError() != ERROR_FILE_NOT_FOUND) {
                continue;  // locked
            }
        } else {
            CloseHandle(e);
        }
        DeleteFileW((dir + kDllName).c_str());
        RemoveDirectoryW(dir.c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

bool DoEnable(std::wstring& err) {
    std::wstring data = DataDir();
    std::wstring bin = data + L"bin\\";
    std::wstring from = DirOf(SelfPath());
    CreateDirectoryW(bin.c_str(), nullptr);
    std::wstring stamp = Timestamp();
    std::wstring dir;
    for (int i = 1; i < 100; ++i) {
        std::wstring name = i == 1 ? stamp : stamp + L"-" + std::to_wstring(i);
        dir = bin + name + L"\\";
        if (CreateDirectoryW(dir.c_str(), nullptr)) {
            stamp = name;
            break;
        }
    }
    for (const wchar_t* f : {kExeName, kDllName}) {
        if (!CopyFileW((from + f).c_str(), (dir + f).c_str(), FALSE)) {
            err = std::wstring(L"Copy failed: ") + from + f;
            return false;
        }
    }

    std::wstring ini = data + L"settings.ini";
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        WritePrivateProfileStringW(L"grid", L"columns", L"0", ini.c_str());
        WritePrivateProfileStringW(L"grid", L"rows", L"0", ini.c_str());
    }

    std::wstring installed = dir + kExeName;
    std::wstring cmd = L"\"" + installed + L"\" run";
    RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, REG_SZ, cmd.c_str(),
                    DWORD((cmd.size() + 1) * sizeof(wchar_t)));

    // Replace a running copy: it disables itself, then the old watcher stops.
    SignalEvent(kDisableEvent);
    WaitInactive(5000);
    SignalEvent(kStopEvent);
    WaitWatcherExit(10000);
    ClearEvent(kDisableEvent);

    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(installed.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                        DETACHED_PROCESS, nullptr, dir.c_str(), &si, &pi)) {
        err = L"Start failed";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CleanBin(bin, stamp);
    Log(L"Enabled, " + installed);
    return true;
}

// Returns once the watcher and the dll in Start are down. The CLI also closes
// the window so the exe isn't locked (uninstall).
void DoDisable(bool closeWindow) {
    SignalEvent(kDisableEvent);
    SignalEvent(kStopEvent);
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue);
    WaitWatcherExit(5000);
    WaitInactive(5000);
    if (closeWindow) {
        if (HWND w = FindWindowW(kWindowClass, nullptr)) {
            PostMessageW(w, WM_CLOSE, 0, 0);
            for (int i = 0; i < 30 && IsWindow(w); ++i) {
                Sleep(100);
            }
        }
    }
    Log(L"Disabled");
}

// Opens the dll's auto-reset event: no dll active, nothing to reset.
void DoReset() {
    if (HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, kResetEvent)) {
        SetEvent(e);
        CloseHandle(e);
    }
    Log(L"Reset requested");
}

// ------------------------------------------------------------ status, log

int Status() {
    Print(std::wstring(L"watcher: ") + (WatcherRunning() ? L"running" : L"stopped"));
    Print(std::wstring(L"autostart: ") + (RunValueSet() ? L"on" : L"off"));
    auto pids = HostPids();
    if (pids.empty()) {
        Print(std::wstring(kHostExe) + L": not running");
    }
    for (DWORD pid : pids) {
        bool known;
        bool loaded = HasModule(pid, kDllName, known);
        Print(std::wstring(kHostExe) + L" " + std::to_wstring(pid) + L": " +
              (!known ? L"unknown" : loaded ? L"loaded" : L"not loaded"));
    }
    return 0;
}

int ShowLog() {
    HANDLE f = CreateFileW((DataDir() + L"log.txt").c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        return 0;
    }
    std::string utf8(GetFileSize(f, nullptr), '\0');
    DWORD read = 0;
    ReadFile(f, utf8.data(), DWORD(utf8.size()), &read, nullptr);
    CloseHandle(f);
    utf8.resize(read);
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
    std::wstring text(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), text.data(), n);
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r')) {
        text.pop_back();
    }
    size_t pos = text.size();
    for (int i = 0; i < 50 && pos != std::wstring::npos && pos > 0; ++i) {
        pos = text.rfind(L'\n', pos - 1);
    }
    size_t start = pos == std::wstring::npos ? 0 : pos + 1;
    size_t end;
    for (; start < text.size(); start = end + 1) {
        end = text.find(L'\n', start);
        if (end == std::wstring::npos) {
            end = text.size();
        }
        size_t len = end - start;
        if (len && text[start + len - 1] == L'\r') {
            --len;
        }
        Print(text.substr(start, len));
    }
    return 0;
}

int Usage() {
    Print(L"(none)   window");
    Print(L"enable   install, start at logon, run now");
    Print(L"disable  restore stock Start, stop, remove autostart");
    Print(L"run      watcher (used by autostart)");
    Print(L"status   watcher, autostart, Start process");
    Print(L"reset    forget all tile cells");
    Print(L"log      last 50 log lines");
    Print(L"version  print version");
    return 0;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring cmd = argc > 1 ? argv[1] : L"";
    LocalFree(argv);

    if (cmd == L"run") {
        return Watch();
    }
    if (cmd.empty()) {
        return RunWindow();
    }
    AttachParentConsole();
    if (cmd == L"enable") {
        std::wstring err;
        if (!DoEnable(err)) {
            Print(err);
            return 1;
        }
        Print(L"Enabled");
        return 0;
    }
    if (cmd == L"disable") {
        DoDisable(true);
        Print(L"Disabled");
        return 0;
    }
    if (cmd == L"reset") {
        DoReset();
        Print(L"Reset");
        return 0;
    }
    if (cmd == L"version") {
        Print(STARTGRID_VERSION_STR);
        return 0;
    }
    if (cmd == L"status") {
        return Status();
    }
    if (cmd == L"log") {
        return ShowLog();
    }
    return Usage();
}
