#pragma once
// 索引数据结构 + 后台索引器（加载数据库 / USN 增量 / 重建 / 保存）。
// 快照不可变；增量更新用写时复制的分块数组：一批变化只复制被改动的 64K 条目块，
// 名称池只追加（分块，名称不跨块），因此发布新快照不需要整体复制。
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ff {

constexpr uint32_t kNoParent = 0xFFFFFFFFu;

// 每个对象 32 字节。名称只存一份原始 UTF-16，无逐文件堆分配。
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

// 写时复制分块数组：快照之间共享块；修改时只克隆被改的块（use_count > 1）。
// 追加写到快照长度之外的位置，旧快照永远不会读取，因此无需克隆。
template <class T>
class CowArray {
public:
    static constexpr unsigned kShift = 16;
    static constexpr size_t kChunk = size_t(1) << kShift, kMask = kChunk - 1;
    size_t size() const { return n_; }
    bool empty() const { return n_ == 0; }
    const T& operator[](size_t i) const { return p_[i >> kShift][i & kMask]; }
    T& Mut(size_t i) {
        size_t c = i >> kShift;
        if (ch_[c].use_count() > 1) {
            auto nc = std::make_shared_for_overwrite<T[]>(kChunk);
            memcpy(nc.get(), ch_[c].get(), kChunk * sizeof(T));
            ch_[c] = std::move(nc); p_[c] = ch_[c].get();
            ++clones_;
        }
        return p_[c][i & kMask];
    }
    void push_back(const T& v) {
        if ((n_ & kMask) == 0) AddChunk();
        p_[n_ >> kShift][n_ & kMask] = v;
        ++n_;
    }
    void clear() { ch_.clear(); p_.clear(); n_ = 0; }
    void resize(size_t n, const T& fill = T{}) {
        while (ch_.size() * kChunk < n) AddChunk();
        for (size_t i = n_; i < n; ++i) p_[i >> kShift][i & kMask] = fill;
        n_ = n;
    }
    void assign(const T* src, size_t n) {
        clear(); resize(n);
        for (size_t c = 0; c * kChunk < n; ++c) memcpy(p_[c], src + c * kChunk, std::min(kChunk, n - c * kChunk) * sizeof(T));
    }
    // f(const T* p, size_t count)
    template <class F> void ForEachChunk(F&& f) const {
        for (size_t c = 0; c * kChunk < n_; ++c) f(p_[c], std::min(kChunk, n_ - c * kChunk));
    }
    T* ChunkPtr(size_t c) { return p_[c]; }
    size_t Bytes() const { return ch_.size() * kChunk * sizeof(T); }
    size_t Clones() const { return clones_; }
    void ResetClones() { clones_ = 0; }
private:
    void AddChunk() { ch_.push_back(std::make_shared_for_overwrite<T[]>(kChunk)); p_.push_back(ch_.back().get()); }
    std::vector<std::shared_ptr<T[]>> ch_;
    std::vector<T*> p_;
    size_t n_ = 0;
    size_t clones_ = 0;
};

// 只追加的名称池：块大小 1M 字符（2 MB），一个名称不跨块；偏移 = 块号 << 20 | 块内偏移。
class NamePool {
public:
    static constexpr unsigned kShift = 20;
    static constexpr size_t kChunk = size_t(1) << kShift, kMask = kChunk - 1;
    const wchar_t* At(uint32_t off) const { return p_[off >> kShift] + (off & kMask); }
    uint32_t Append(const wchar_t* s, size_t len);   // len <= 0xFFFF
    void clear() { ch_.clear(); p_.clear(); used_ = 0; }
    size_t Chunks() const { return ch_.size(); }
    size_t Used() const { return used_; }           // 逻辑长度（含块尾填充）
    size_t Bytes() const { return ch_.size() * kChunk * 2; }
    wchar_t* ChunkPtr(size_t c) { return p_[c]; }
    const wchar_t* ChunkPtr(size_t c) const { return p_[c]; }
    void AddChunk() { ch_.push_back(std::make_shared_for_overwrite<wchar_t[]>(kChunk)); p_.push_back(ch_.back().get()); }
    void SetUsed(size_t u) { used_ = u; }
    void SetLogGrowth(bool b) { logGrowth_ = b; }   // 只在增量更新时记录 name array 增长
private:
    bool logGrowth_ = false;
    std::vector<std::shared_ptr<wchar_t[]>> ch_;
    std::vector<wchar_t*> p_;
    size_t used_ = 0;
};

struct VolumeInfo {
    std::wstring root;         // "C:"
    uint32_t serial = 0;
    bool ntfs = false;
    bool mft = false;          // 条目带完整 64 位 FRN（MFT 建立），可做 USN 增量
    uint64_t journalId = 0;    // 0 = 未跟踪
    int64_t nextUsn = 0;       // 已应用到的 USN 游标
};

// 每卷设置（ini 中的并行数组）
struct VolumeConfig {
    std::wstring path;                      // "C:"
    std::wstring guid;                      // 卷 GUID 路径（GetVolumeNameForVolumeMountPoint）
    bool include = true;                    // ntfs_volume_includes
    bool monitor = true;                    // ntfs_volume_monitors
    bool loadRecentChanges = false;         // ntfs_volume_load_recent_changes
    bool enableUsnJournal = true;           // ntfs_volume_enable_usn_journals
    std::vector<std::wstring> includeOnly;  // ntfs_volume_include_onlys（空 = 整卷）
};

// 不可变快照
struct IndexData {
    CowArray<Entry> entries;
    NamePool names;
    std::vector<uint32_t> byName;     // 按名称（折叠）排序，不含已删除
    CowArray<uint64_t> frn;           // 与 entries 等长或为空
    std::vector<VolumeInfo> volumes;
    uint64_t configHash = 0;
    uint32_t dbVersion = 0;
    size_t folders = 0, files = 0;

    std::wstring_view Name(uint32_t i) const { const Entry& e = entries[i]; return {names.At(e.nameOff), e.nameLen}; }
    size_t ParentPathInto(uint32_t i, wchar_t* buf, size_t cap) const;
    size_t FullPathInto(uint32_t i, wchar_t* buf, size_t cap) const;
    std::wstring ParentPath(uint32_t i) const;
    std::wstring FullPath(uint32_t i) const;
    size_t MemoryBytes() const { return entries.Bytes() + names.Bytes() + byName.capacity() * 4 + frn.Bytes(); }
};

// 单卷的可变构建数据（局部下标）
struct VolumeData {
    VolumeInfo info;
    std::vector<Entry> entries;       // [0] 是卷根
    std::vector<wchar_t> names;
    std::vector<uint64_t> frn;
    uint32_t Add(const wchar_t* s, size_t len, uint32_t parent, uint16_t flags, uint64_t size, uint64_t mtime);
};

std::shared_ptr<IndexData> Merge(std::vector<VolumeData>& vols, uint64_t configHash);   // 建立时：整体排序
VolumeData ExtractVolume(const IndexData& d, uint32_t vol);
uint64_t ConfigHash(const std::vector<std::wstring>& excludes, const std::vector<VolumeConfig>& vols);
bool PathUnder(std::wstring_view path, std::wstring_view dir);   // 不区分大小写；path == dir 也算
// 路径过滤：排除列表 + 卷的仅包含列表。dir=true 时允许“仅包含目录的祖先”
bool PathFiltered(std::wstring_view path, bool dir, const std::vector<std::wstring>& excludes, const std::vector<std::wstring>& includeOnly);

enum class IndexPhase { Idle, Loading, Creating, Updating, Saving };

struct IndexStats {
    double loadSec = -1, rebuildSec = -1, updateSec = -1, saveSec = -1, sortSec = -1;
    uint64_t dbBytes = 0;
    bool loadedFromDb = false;
    std::wstring note;          // 需要提示用户的信息（例如需要管理员/服务）
    size_t usnChanges = 0;
    bool headerOnlyUpdate = false, fullSave = false;
};

class Indexer {
public:
    struct Options {
        std::vector<std::wstring> roots, excludes;   // roots：要索引的卷/文件夹（"C:"）
        std::vector<VolumeConfig> volumes;           // NTFS 卷设置（按 path 匹配 roots）
        std::wstring dbPath;
        bool compress = false;
        bool updateAfterLoad = true;
        bool forceRebuild = false;
        bool monitor = true;            // 总开关；每卷另有 monitor
        int monitorDelayMs = 1000;      // monitor_update_delay：批量合并间隔
        const VolumeConfig* Vol(const std::wstring& root) const;
    };
    void Start(HWND notifyHwnd, Options opt);
    void Stop();
    ~Indexer() { Stop(); }

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

    std::thread thread_;
    std::atomic<bool> stop_{false}, busy_{false}, ready_{false};
    std::atomic<IndexPhase> phase_{IndexPhase::Idle};
    std::atomic<size_t> progress_{0};
    mutable std::mutex mu_;
    std::shared_ptr<const IndexData> data_;
    std::wstring current_;
    IndexStats stats_;
    std::wstring adminVols_;
    HWND hwnd_ = nullptr;
};

} // namespace ff
