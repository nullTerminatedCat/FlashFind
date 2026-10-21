#pragma once
// 索引数据结构 + 后台索引器（加载数据库 / 增量更新 / 重建 / 保存）。
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ff {

constexpr uint32_t kNoParent = 0xFFFFFFFFu;

// 每个对象 32 字节。名称只存一份原始 UTF-16（连续名称池 + 偏移），无逐文件堆分配。
struct Entry {
    uint32_t nameOff = 0;
    uint16_t nameLen = 0;
    uint16_t flags = 0;      // bit0 文件夹，bit1 已删除，bit8-15 卷序号
    uint32_t parent = kNoParent;
    uint32_t reserved = 0;
    uint64_t size = 0;
    uint64_t mtime = 0;      // FILETIME
    bool IsDir() const { return flags & 1; }
    bool IsDeleted() const { return flags & 2; }
    uint32_t Vol() const { return flags >> 8; }
};
static_assert(sizeof(Entry) == 32);

enum : uint16_t { kFlagDir = 1, kFlagDeleted = 2 };

struct VolumeInfo {
    std::wstring root;         // "C:"
    uint32_t serial = 0;
    bool ntfs = false;
    bool mft = false;          // 条目带完整 64 位 FRN（MFT 建立或带 FileId 的枚举），可做 USN 增量
    uint64_t journalId = 0;    // 0 = 未跟踪
    int64_t nextUsn = 0;       // 已应用到的 USN 游标
};

// 不可变快照
struct IndexData {
    std::vector<Entry> entries;
    std::vector<wchar_t> names;
    std::vector<uint32_t> byName;     // 按名称（折叠）预排序，不含已删除
    std::vector<uint64_t> frn;        // 与 entries 等长或为空（无 MFT 卷时）
    std::vector<VolumeInfo> volumes;
    uint64_t configHash = 0;
    uint32_t dbVersion = 0;
    size_t folders = 0, files = 0;

    std::wstring_view Name(uint32_t i) const { auto& e = entries[i]; return {names.data() + e.nameOff, e.nameLen}; }
    size_t ParentPathInto(uint32_t i, wchar_t* buf, size_t cap) const;
    size_t FullPathInto(uint32_t i, wchar_t* buf, size_t cap) const;
    std::wstring ParentPath(uint32_t i) const;
    std::wstring FullPath(uint32_t i) const;
    size_t MemoryBytes() const {
        return entries.capacity() * sizeof(Entry) + names.capacity() * 2 + byName.capacity() * 4 + frn.capacity() * 8;
    }
};

// 单卷的可变构建数据（局部下标）
struct VolumeData {
    VolumeInfo info;
    std::vector<Entry> entries;       // [0] 是卷根
    std::vector<wchar_t> names;
    std::vector<uint64_t> frn;        // MFT 卷才有
    uint32_t Add(const wchar_t* s, size_t len, uint32_t parent, uint16_t flags, uint64_t size, uint64_t mtime);
};

std::shared_ptr<IndexData> Merge(std::vector<VolumeData>& vols, uint64_t configHash);
VolumeData ExtractVolume(const IndexData& d, uint32_t vol);
uint64_t ConfigHash(const std::vector<std::wstring>& roots, const std::vector<std::wstring>& excludes, uint32_t version);

enum class IndexPhase { Idle, Loading, Creating, Updating, Saving };

struct IndexStats {
    double loadSec = -1, rebuildSec = -1, updateSec = -1, saveSec = -1, sortSec = -1;
    uint64_t dbBytes = 0;
    bool loadedFromDb = false;
    std::wstring note;          // 需要提示用户的信息（例如需要管理员/服务）
    size_t usnChanges = 0, usnUnresolved = 0;
    bool headerOnlyUpdate = false, fullSave = false;
};

class Indexer {
public:
    struct Options {
        std::vector<std::wstring> roots, excludes;
        std::wstring dbPath;
        bool compress = false;
        bool updateAfterLoad = true;   // 加载后是否读 USN 增量（自检测加载速度时关掉）
        bool forceRebuild = false;     // 强制重建（NTFS 卷需管理员读 MFT）
        bool monitor = true;           // 启动后持续读 USN（ntfs_volume_monitors）
        bool enableUsnJournal = true;  // Journal 未启用时尝试创建
        std::vector<std::wstring> seedTraverse;  // 仅自检：允许以带 FileId 的枚举建立这些卷（用于中完整性测试 USN 路径）
    };
    // notifyHwnd 收 WM_APP_INDEX_PROGRESS / WM_APP_INDEX_DONE（每次发布新快照）
    void Start(HWND notifyHwnd, Options opt);
    void Stop();
    ~Indexer() { Stop(); }

    // 未就绪返回 nullptr。就绪标志与快照在同一把锁下原子发布。
    std::shared_ptr<const IndexData> Snapshot() const;
    bool Ready() const { return ready_; }
    bool Busy() const { return busy_; }
    IndexPhase Phase() const { return phase_; }
    size_t Progress() const { return progress_; }
    std::wstring CurrentFolder() const;
    IndexStats Stats() const { std::lock_guard lk(mu_); return stats_; }

    static std::vector<std::wstring> DefaultRoots();
    static std::wstring DefaultDbPath();

private:
    void Run(HWND hwnd, Options opt);
    void Publish(HWND hwnd, std::shared_ptr<IndexData> d);
    bool BuildVolume(VolumeData& v, const Options& opt, std::wstring& note);
    // 读各卷 USN 增量并应用；返回是否有记录变化。cursorMoved 表示游标前进（需要至少更新头部）
    bool PollUsn(std::shared_ptr<IndexData>& cur, const Options& opt, bool& cursorMoved, std::vector<uint32_t>* staleVols);

    std::thread thread_;
    std::atomic<bool> stop_{false}, busy_{false}, ready_{false};
    std::atomic<IndexPhase> phase_{IndexPhase::Idle};
    std::atomic<size_t> progress_{0};
    mutable std::mutex mu_;
    std::shared_ptr<const IndexData> data_;
    std::wstring current_;
    IndexStats stats_;
    std::vector<VolumeInfo> volState_;   // 索引线程独占：各卷当前游标
    std::vector<uint8_t> stale_;
    std::wstring adminVols_;
    HWND hwnd_ = nullptr;
};

} // namespace ff
