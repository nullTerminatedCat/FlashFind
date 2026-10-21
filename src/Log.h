#pragma once
// 日志：%APPDATA%\FlashFind\FlashFind.log（服务：%ProgramData%\FlashFind\FlashFind-service.log），UTF-8 追加。
// LogBatch：作用域内本线程的日志先缓存，析构时一次写入（每个 USN 批次一次文件写）。
#include <string>
namespace ff {
void Log(const wchar_t* fmt, ...);
void SetLogPath(const wchar_t* path);
extern bool g_debugLog;   // -debug-log：逐条 USN 记录日志
class LogBatch {
public:
    LogBatch();
    ~LogBatch();
    LogBatch(const LogBatch&) = delete;
    LogBatch& operator=(const LogBatch&) = delete;
};
}
