#pragma once
// NTFS 索引：
//  * 建立（需要管理员）：打开 \\.\X:，读 $MFT 记录 0 的 $DATA 运行列表，按运行大块顺序读取整个 $MFT，
//    做更新序列修正，解析 $STANDARD_INFORMATION / $FILE_NAME / $DATA；扩展记录的属性合并到基记录。
//  * 增量（中完整性即可）：根目录句柄上 FSCTL_QUERY_USN_JOURNAL + FSCTL_READ_UNPRIVILEGED_USN_JOURNAL（V1 输入）；
//    非特权记录不含文件名，创建/改名用 OpenFileById 取名称、大小、时间。管理员时用 FSCTL_READ_USN_JOURNAL。
// FRN 一律按完整 64 位（序列号 << 48 | 记录号）比较。
#include "Index.h"
#include <unordered_map>

namespace ff {

struct UsnChange {
    uint64_t parent = 0;
    uint32_t attrs = 0;
    uint32_t reasons = 0;
    bool deleted = false;
    std::wstring name;     // 特权读取时有；非特权为空
};

class MftIndexer {
public:
    static bool IsElevated();
    static bool IsNtfs(const std::wstring& root, uint32_t* serial);
    static HANDLE OpenRoot(const std::wstring& root);   // 根目录句柄（中完整性可用）
    // 查询 Journal；不存在且 create=true 时尝试创建（需管理员）
    static bool QueryJournal(const std::wstring& root, bool create, uint64_t& journalId, int64_t& firstUsn, int64_t& nextUsn, std::wstring& err);
    // 从 from 读到当前 NextUsn；返回新游标。每个 FRN 只保留最终状态
    static bool ReadChanges(const std::wstring& root, uint64_t journalId, int64_t from, int64_t& next,
                            std::unordered_map<uint64_t, UsnChange>& out, std::wstring& err);
    // 应用到卷数据（需要 v.frn）；unresolved = 新建但无法取得名称的条目数
    static size_t Apply(VolumeData& v, const std::unordered_map<uint64_t, UsnChange>& changes, size_t* unresolved);
    // 管理员：顺序读 $MFT 建立整卷
    static bool Build(VolumeData& v, bool enableUsn, std::atomic<bool>& stop, std::atomic<size_t>& progress, std::wstring& err);
    // 带 FileId 的目录枚举（GetFileInformationByHandleEx FileIdBothDirectoryInfo），仅用于非 NTFS 文件夹索引与自检
    static void EnumerateWithIds(VolumeData& v, const std::vector<std::wstring>& excludes, std::atomic<bool>& stop, std::atomic<size_t>& progress);
};

} // namespace ff
