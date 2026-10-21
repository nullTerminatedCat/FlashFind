#pragma once
// NTFS：MFT 枚举（FSCTL_ENUM_USN_DATA）+ 文件记录解析（FSCTL_GET_NTFS_FILE_RECORD，取大小/修改时间）
// + USN Journal 增量（FSCTL_QUERY_USN_JOURNAL / FSCTL_READ_USN_JOURNAL）。
// 需要打开卷句柄 \\.\C:，即管理员权限（M3 改由服务进程执行）。
#include "Index.h"

namespace ff {
class MftIndexer {
public:
    static bool IsElevated();
    static bool IsNtfs(const std::wstring& root, uint32_t* serial);
    // 全量：成功返回 true，并填好 v.entries/names/frn、journalId、nextUsn
    static bool Build(VolumeData& v, std::atomic<bool>& stop, std::atomic<size_t>& progress, std::wstring& err);
    // 增量：从 v.info.nextUsn 读到当前；journal 不符或已被截断返回 false（调用方标记过期）
    static bool ApplyUsn(VolumeData& v, std::atomic<bool>& stop, size_t* changes, std::wstring& err);
};
}
