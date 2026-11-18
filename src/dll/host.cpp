#include "host.h"

#include <stdarg.h>

#include <atomic>

namespace {

struct Watch {
    std::atomic<bool> stop{false};
    HANDLE thread = nullptr;
};

Watch* g_watch = nullptr;
void (*g_onChange)() = nullptr;

std::wstring SettingsPath() { return DataDir() + L"settings.ini"; }

FILETIME SettingsTime() {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    GetFileAttributesExW(SettingsPath().c_str(), GetFileExInfoStandard, &d);
    return d.ftLastWriteTime;
}

DWORD WINAPI WatchThread(void* arg) {
    auto* w = static_cast<Watch*>(arg);
    HANDLE h = FindFirstChangeNotificationW(DataDir().c_str(), FALSE,
                                            FILE_NOTIFY_CHANGE_LAST_WRITE);
    if (h == INVALID_HANDLE_VALUE) {
        Log(L"Settings watcher failed: %lu", GetLastError());
        return 0;
    }
    FILETIME last = SettingsTime();
    while (!w->stop) {
        if (WaitForSingleObject(h, 500) == WAIT_OBJECT_0) {
            FindNextChangeNotification(h);
            FILETIME now = SettingsTime();
            if (CompareFileTime(&now, &last) != 0) {
                last = now;
                Log(L"Settings changed");
                g_onChange();
            }
        }
    }
    FindCloseChangeNotification(h);
    return 0;
}

}  // namespace

void Log(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);
    va_end(args);
    std::wstring line = LogPrefix(L"dll") + buf;
    OutputDebugStringW((line + L"\n").c_str());
    AppendLog(L"dll", buf);
}

int GetGridInt(const wchar_t* key) {
    return int(GetPrivateProfileIntW(L"grid", key, 0, SettingsPath().c_str()));
}

std::wstring ReadCells() {
    std::wstring path = DataDir() + L"cells.txt";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        return {};
    }
    std::string utf8(GetFileSize(f, nullptr), '\0');
    DWORD read = 0;
    ReadFile(f, utf8.data(), DWORD(utf8.size()), &read, nullptr);
    CloseHandle(f);
    utf8.resize(read);
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
    std::wstring text(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), text.data(), n);
    return text;
}

void WriteCells(const std::wstring& text) {
    std::wstring path = DataDir() + L"cells.txt";
    std::wstring tmp = path + L".tmp";
    int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0,
                                nullptr, nullptr);
    std::string utf8(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), utf8.data(), n,
                        nullptr, nullptr);
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        Log(L"Saving cells failed: %lu", GetLastError());
        return;
    }
    DWORD written;
    WriteFile(f, utf8.data(), DWORD(utf8.size()), &written, nullptr);
    CloseHandle(f);
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        Log(L"Saving cells failed: %lu", GetLastError());
    }
}

void StartSettingsWatcher(void (*onChange)()) {
    StopSettingsWatcher();
    g_onChange = onChange;
    g_watch = new Watch;
    g_watch->thread = CreateThread(nullptr, 0, WatchThread, g_watch, 0, nullptr);
}

void StopSettingsWatcher() {
    Watch* w = g_watch;
    g_watch = nullptr;
    if (!w) {
        return;
    }
    w->stop = true;
    if (w->thread) {
        // The thread may be inside onChange (up to 5 s on the UI thread).
        if (WaitForSingleObject(w->thread, 10000) != WAIT_OBJECT_0) {
            Log(L"Settings watcher didn't stop, leaving it");
            return;  // it still owns w: the leak is deliberate
        }
        CloseHandle(w->thread);
    }
    delete w;
}
