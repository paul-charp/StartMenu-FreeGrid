#pragma once

#include <windows.h>
#include <shlobj.h>

#include <string>

// Named events shared by the dll and the exe.
constexpr wchar_t kDisableEvent[] = L"Local\\StartGrid.Disable";
constexpr wchar_t kStopEvent[] = L"Local\\StartGrid.Stop";
constexpr wchar_t kResetEvent[] = L"Local\\StartGrid.Reset";  // auto-reset
constexpr wchar_t kUiMutex[] = L"Local\\StartGrid.UI";
constexpr wchar_t kWatcherMutex[] = L"Local\\StartGrid.Watcher";
constexpr wchar_t kActiveMutex[] = L"Local\\StartGrid.Active";

// {3B0F6E2A-8C41-4D7E-9A55-1F2C7B9D4E62}
constexpr CLSID CLSID_Tap = {
    0x3b0f6e2a, 0x8c41, 0x4d7e, {0x9a, 0x55, 0x1f, 0x2c, 0x7b, 0x9d, 0x4e, 0x62}};

// %LOCALAPPDATA%\StartGrid, created if missing. Ends with a backslash.
inline std::wstring DataDir() {
    PWSTR base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
        dir = std::wstring(base) + L"\\StartGrid\\";
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    CoTaskMemFree(base);
    return dir;
}

// Log line prefix: HH:MM:SS.mmm [who]
inline std::wstring LogPrefix(const wchar_t* who) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t buf[64];
    swprintf_s(buf, L"%02d:%02d:%02d.%03d [%s] ", t.wHour, t.wMinute, t.wSecond,
               t.wMilliseconds, who);
    return buf;
}

// Appends a line to log.txt. Truncates it first when over 1 MB.
inline void AppendLog(const wchar_t* who, const std::wstring& msg) {
    std::wstring path = DataDir() + L"log.txt";
    if (path.size() < 8) {
        return;
    }
    std::wstring line = LogPrefix(who) + msg + L"\r\n";
    int n = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(n > 0 ? n - 1 : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, utf8.data(), n, nullptr, nullptr);
    HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA | GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        return;
    }
    LARGE_INTEGER size;
    if (GetFileSizeEx(f, &size) && size.QuadPart > (1 << 20)) {
        CloseHandle(f);
        f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) {
            return;
        }
    }
    DWORD written;
    WriteFile(f, utf8.data(), DWORD(utf8.size()), &written, nullptr);
    CloseHandle(f);
}
