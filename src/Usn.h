#pragma once
// USN 增量：解析 USN_RECORD_V2（名称取自记录本身），以及对快照做增量插入/删除。
// 不使用 OpenFileById；只有带名称的特权记录才用于修补索引。
#include "Index.h"
#include <functional>
#include <unordered_map>

namespace ff {

struct LookupResult { uint8_t sizeOk = 0, timeOk = 0; uint64_t size = 0, mtime = 0; };

struct UsnEvent {
    uint64_t frn = 0, parent = 0;   // 完整 64 位
    uint32_t reason = 0, attrs = 0;
    int64_t usn = 0, time = 0;
    std::wstring name;
};

// 解析一段记录（不含开头 8 字节的 next USN）。校验 RecordLength / FileNameOffset；损坏返回 false。
// logBudget：最多记录多少条逐记录日志（parent FRN、FRN、reason、filename）；默认 0，仅 -debug-log 时开启
bool ParseUsnRecords(const uint8_t* p, size_t n, std::vector<UsnEvent>& out, std::wstring& err, int& logBudget);
// 把一条记录编码为 USN_RECORD_V2（自检用于构造合成数据）
void EncodeUsnRecord(std::vector<uint8_t>& buf, const UsnEvent& e);

class LiveUpdater {
public:
    // 批量取大小/时间（服务或管理员：FSCTL_GET_NTFS_FILE_RECORD）；为空则只用记录时间戳
    using Lookup = std::function<void(uint32_t vol, const std::vector<uint64_t>& frns, std::vector<LookupResult>& out)>;
    // 扫描一个目录（移入索引范围的文件夹）：out.entries[0] 为该目录本身，其余为子树（带 FRN）
    using Scanner = std::function<bool(uint32_t vol, const std::wstring& path, VolumeData& out)>;
    struct Stats {
        size_t events = 0, creates = 0, deletes = 0, modifies = 0, renames = 0, dropped = 0, notFound = 0;
        size_t removedDirs = 0, removedFiles = 0, inserted = 0, removed = 0, chunkClones = 0, lookups = 0;
        size_t scans = 0, readdFiles = 0, readdFolders = 0;
        double subtreeMs = 0, scanMs = 0;
        double applyMs = 0, commitMs = 0, frnIndexMs = 0;
    };
    explicit LiveUpdater(std::shared_ptr<const IndexData> base) : base_(std::move(base)) {}
    void SetFilter(uint32_t vol, std::vector<std::wstring> excludes, std::vector<std::wstring> includeOnly);
    void SetScanner(Scanner s) { scanner_ = std::move(s); }
    void Apply(uint32_t vol, const std::vector<UsnEvent>& ev, const Lookup& lookup);
    bool Dirty() const { return work_ != nullptr; }
    // 生成新快照（增量合并 byName），并以其为新的基准
    std::shared_ptr<IndexData> Commit();
    void Rebase(std::shared_ptr<const IndexData> base);   // 外部整卷重建后
    const Stats& LastStats() const { return st_; }
    void ResetStats() { st_ = {}; }
    size_t Find(uint32_t vol, uint64_t frn) const;   // 返回条目下标或 SIZE_MAX（含已删除检查）

private:
    void EnsureWork();
    void EnsureFrnIndex(uint32_t vol);
    void MarkRemoved(uint32_t id);
    void Link(uint32_t child, uint32_t parent);
    void Unlink(uint32_t child, uint32_t parent);
    void RemoveTree(uint32_t idx, bool logIt);
    void ScanMovedIn(uint32_t vol, uint32_t dirIdx);
    uint32_t AddEntry(uint32_t vol, const std::wstring& name, uint32_t parent, bool dir, int64_t time, uint64_t frn);
    bool Filtered(uint32_t vol, uint32_t parent, const std::wstring& name, bool dir) const;

    std::shared_ptr<const IndexData> base_;
    std::shared_ptr<IndexData> work_;
    struct VolIdx {
        bool built = false;
        std::vector<std::pair<uint64_t, uint32_t>> sorted;
        std::unordered_map<uint64_t, uint32_t> added;
        std::vector<std::wstring> excludes, includeOnly;
    };
    std::vector<VolIdx> vols_;
    std::vector<uint32_t> removed_, insert_;
    // 反向索引：父 -> 子（单链表，firstChild/nextSibling 各 4 字节/对象，约 18 MB / 226 万对象）
    std::vector<uint32_t> firstChild_, nextSib_;
    bool childBuilt_ = false;
    Scanner scanner_;
    int64_t folderDelta_ = 0, fileDelta_ = 0;
    Stats st_;
};

} // namespace ff
