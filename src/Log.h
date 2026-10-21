#pragma once
// 简单日志：%APPDATA%\FlashFind\FlashFind.log（追加）+ OutputDebugString
namespace ff {
void Log(const wchar_t* fmt, ...);
void SetLogPath(const wchar_t* path);
}
