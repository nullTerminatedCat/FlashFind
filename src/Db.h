#pragma once
// 数据库文件 FlashFind.db：头（明文）+ 载荷（可选 XPRESS_HUFF 压缩，Windows Compression API）。
// 保存：写 .tmp -> FlushFileBuffers -> MoveFileExW(REPLACE_EXISTING) 原子替换。
#include "Index.h"

namespace ff {
constexpr uint32_t kDbVersion = 2;
bool SaveDb(const std::wstring& path, const IndexData& d, bool compress, uint64_t* bytesWritten);
// 失败原因写入 err（损坏 / 版本不符 / 不存在）
std::shared_ptr<IndexData> LoadDb(const std::wstring& path, std::wstring& err);
}
