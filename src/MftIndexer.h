#pragma once
// NTFS 索引（需要管理员或将来的 FlashFind 服务）：
//  * 建立：打开 \\.\X:，取 $MFT 记录 0 的 $DATA 运行列表（若有 $ATTRIBUTE_LIST，则跟随它读取扩展记录，
//    把各段 mapping pairs 合并），按运行大块顺序读取整个 $MFT，做更新序列修正；扩展记录的属性并入基记录。
//  * 增量：FSCTL_READ_USN_JOURNAL（卷句柄），名称直接取自 USN_RECORD_V2；大小/时间用
//    FSCTL_GET_NTFS_FILE_RECORD 单条查询。不使用 OpenFileById（留给将来的 ReFS 支持）。
//  * 中完整性只能读到无文件名的非特权记录：不用于修补，卷标记为需要服务/管理员。
// FRN 一律按完整 64 位（序列号 << 48 | 记录号）比较。
#include "Index.h"
#include "Usn.h"

namespace ff {

class MftIndexer {
public:
    static bool IsElevated();
    static bool IsNtfs(const std::wstring& root, uint32_t* serial);
    static HANDLE OpenRoot(const std::wstring& root);         // 根目录句柄（中完整性可查询 Journal）
    static HANDLE OpenVolumeRead(const std::wstring& root);   // \\.\X:（需要管理员）
    static uint32_t RecordSize(HANDLE vol);
    // 查询 Journal；不存在且 create=true 时尝试创建（需管理员），失败记录 "Failed to create USN Journal"
    static bool QueryJournal(const std::wstring& root, bool create, uint64_t& journalId, int64_t& firstUsn, int64_t& nextUsn, std::wstring& err);
    // 从 from 读到当前末尾（特权）；from 更新为新游标。返回 0 或 Win32 错误（Journal 丢失/过期返回 ERROR_JOURNAL_ENTRY_DELETED 等）
    // 原始 USN_RECORD_V2 字节（去掉每次返回开头的 8 字节 next USN），由调用方 ParseUsnRecords
    static DWORD ReadUsnRaw(HANDLE vol, uint64_t journalId, int64_t& from, std::vector<uint8_t>& raw);
    struct Geometry { uint32_t recSize = 1024, cluster = 4096, sector = 512; };
    static Geometry GetGeometry(HANDLE vol);
    // 单条记录查询：$DATA 大小与 SI 修改时间；基记录只有属性列表时跟随扩展记录；序列号不符则全部失败
    static LookupResult LookupRecord(HANDLE vol, const Geometry& g, uint64_t frn);
    static bool Build(VolumeData& v, bool enableUsn, std::atomic<bool>& stop, std::atomic<size_t>& progress, std::wstring& err);
    // 带 FileId 的目录枚举：非 NTFS 文件夹索引；以及移入文件夹的扫描（start = 该目录，entries[0] 为它本身）
    static void EnumerateWithIds(VolumeData& v, const std::vector<std::wstring>& excludes, std::atomic<bool>& stop, std::atomic<size_t>& progress, const std::wstring& start = L"");
};

} // namespace ff
