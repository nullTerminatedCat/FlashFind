#pragma once
// 进程内索引（M1）：遍历文件夹建立内存索引。
// 后续里程碑由 MftIndexer（服务端读取 MFT + USN）提供同样的数据，通过 ServiceClient 传给界面。
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

// 每个对象 32 字节；名称存于连续字符串池，无逐文件堆分配
struct Entry {
    uint32_t nameOff = 0;   // names / lowerNames 中的偏移
    uint32_t nameLen = 0;
    uint32_t parent = kNoParent;
    uint32_t flags = 0;     // bit0 = 文件夹
    uint64_t size = 0;
    uint64_t mtime = 0;     // FILETIME
    bool IsDir() const { return flags & 1; }
};

// 不可变快照：建好之后只读，搜索线程可无锁访问
struct IndexData {
    std::vector<Entry> entries;
    std::vector<wchar_t> names;
    std::vector<wchar_t> lowerNames;
    std::vector<uint32_t> byName;   // 按名称（不区分大小写）预排序的下标

    std::wstring_view Name(uint32_t i) const { auto& e = entries[i]; return {names.data() + e.nameOff, e.nameLen}; }
    std::wstring_view LowerName(uint32_t i) const { auto& e = entries[i]; return {lowerNames.data() + e.nameOff, e.nameLen}; }
    // 写入调用方缓冲区，不分配内存；返回长度（超长截断）
    size_t ParentPathInto(uint32_t i, wchar_t* buf, size_t cap) const;
    size_t FullPathInto(uint32_t i, wchar_t* buf, size_t cap) const;
    std::wstring ParentPath(uint32_t i) const;  // 便捷版（会分配），仅用于低频操作
    std::wstring FullPath(uint32_t i) const;
};

class Indexer {
public:
    // notifyHwnd 接收 WM_APP_INDEX_PROGRESS(wParam=已发现数量) 和 WM_APP_INDEX_DONE
    void Start(HWND notifyHwnd, std::vector<std::wstring> roots, std::vector<std::wstring> excludes);
    void Stop();
    ~Indexer() { Stop(); }

    std::shared_ptr<const IndexData> Snapshot() const;
    bool Busy() const { return busy_; }
    std::wstring CurrentFolder() const;

    static std::vector<std::wstring> DefaultRoots(); // 所有固定磁盘

private:
    void Run(HWND hwnd, std::vector<std::wstring> roots, std::vector<std::wstring> excludes);

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> busy_{false};
    mutable std::mutex mu_;
    std::shared_ptr<const IndexData> data_ = std::make_shared<IndexData>();
    std::wstring current_;
};

} // namespace ff
