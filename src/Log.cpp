#include "Log.h"
#include "Settings.h"
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace ff {
static std::mutex g_logMu;
static std::wstring g_logPath;

void SetLogPath(const wchar_t* p) { std::lock_guard lk(g_logMu); g_logPath = p; }

void Log(const wchar_t* fmt, ...) {
    wchar_t msg[1024];
    va_list ap; va_start(ap, fmt);
    vswprintf_s(msg, fmt, ap);
    va_end(ap);
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t line[1200];
    int n = swprintf_s(line, L"%04u-%02u-%02u %02u:%02u:%02u.%03u %s\r\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
    OutputDebugStringW(line);
    std::lock_guard lk(g_logMu);
    if (g_logPath.empty()) {
        g_logPath = GetSettings().IniPath();
        g_logPath.replace(g_logPath.size() - 4, 4, L".log");
    }
    HANDLE f = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    char u8[4096];
    int b = WideCharToMultiByte(CP_UTF8, 0, line, n, u8, sizeof(u8), nullptr, nullptr);
    DWORD wr; WriteFile(f, u8, static_cast<DWORD>(b), &wr, nullptr);
    CloseHandle(f);
}
}
