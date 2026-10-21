#include "Log.h"
#include "Settings.h"
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace ff {
bool g_debugLog = false;
static std::mutex g_logMu;
static std::wstring g_logPath;
static thread_local int t_batchDepth = 0;
static thread_local std::string* t_batch = nullptr;

void SetLogPath(const wchar_t* p) { std::lock_guard lk(g_logMu); g_logPath = p; }

static void WriteRaw(const char* p, size_t n) {
    std::lock_guard lk(g_logMu);
    if (g_logPath.empty()) {
        g_logPath = GetSettings().IniPath();
        g_logPath.replace(g_logPath.size() - 4, 4, L".log");
    }
    HANDLE f = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wr; WriteFile(f, p, static_cast<DWORD>(n), &wr, nullptr);
    CloseHandle(f);
}

LogBatch::LogBatch() { if (t_batchDepth++ == 0) t_batch = new std::string(); }
LogBatch::~LogBatch() {
    if (--t_batchDepth == 0) {
        if (!t_batch->empty()) WriteRaw(t_batch->data(), t_batch->size());
        delete t_batch; t_batch = nullptr;
    }
}

void Log(const wchar_t* fmt, ...) {
    wchar_t msg[1024];
    va_list ap; va_start(ap, fmt);
    vswprintf_s(msg, fmt, ap);
    va_end(ap);
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t line[1200];
    int n = swprintf_s(line, L"%04u-%02u-%02u %02u:%02u:%02u.%03u %s\r\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
    if (g_debugLog) OutputDebugStringW(line);
    char u8[4096];
    int b = WideCharToMultiByte(CP_UTF8, 0, line, n, u8, sizeof(u8), nullptr, nullptr);
    if (t_batch) { t_batch->append(u8, b); return; }
    WriteRaw(u8, b);
}
}
