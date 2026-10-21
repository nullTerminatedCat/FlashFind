#pragma once
// 结果列表：SysListView32（LVS_OWNERDATA）负责滚动、选择、键盘、命中测试；
// 子类化后自己处理 WM_PAINT（GDI 双缓冲逐行绘制），WM_ERASEBKGND 返回 1。行高由我们控制。
#include "Search.h"
#include <commctrl.h>
#include <unordered_map>
#include <unordered_set>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace ff {

enum Column { ColName = 0, ColPath, ColSize, ColDate, ColCount };

class ResultList {
public:
    HWND Create(HWND parent, int id);
    HWND Hwnd() const { return hwnd_; }
    void SetResults(std::shared_ptr<SearchResult> r);
    const SearchResult* Results() const { return res_.get(); }
    void ApplyTheme();
    SortSpec Sort() const { return sort_; }
    void SetSort(SortSpec s);
    void SetFont(HFONT f, HFONT bold, int rowHeight);
    bool OnNotify(NMHDR* nm, LRESULT* out);

    int SelectedCount() const;
    std::vector<uint32_t> Selection() const;
    int FocusedIndex() const;
    std::vector<int> ColumnWidths() const;
    ~ResultList();

private:
    static LRESULT CALLBACK SubclassProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    void Paint(HDC target, const RECT& clip);
    void DrawRow(HDC dc, int row, const RECT& rowRc, const RECT* cols, bool focused);
    void DrawName(HDC dc, const RECT& rc, uint32_t i, bool selected);
    int IconFor(uint32_t i);
    void IconWorker();
    static bool WantsRealIcon(std::wstring_view ext);

    HWND hwnd_ = nullptr;
    SortSpec sort_;
    std::shared_ptr<SearchResult> res_;
    static constexpr size_t kRowBuf = 4096;
    wchar_t rowBuf_[kRowBuf];
    char mask_[kRowBuf];
    std::unordered_map<uint64_t, int> iconCache_;   // 扩展名哈希 -> 图标序号
    int folderIcon_ = -1;
    // 真实图标（exe/lnk/ico 等）：只为可见行请求，后台线程 SHGetFileInfo，完成后重绘
    std::mutex iconMu_;
    std::condition_variable iconCv_;
    std::vector<std::wstring> iconQ_;                 // 栈：最新请求优先
    std::unordered_set<std::wstring> iconPending_;
    std::unordered_map<std::wstring, int> realIcon_;  // 完整路径 -> 系统图标序号
    std::vector<std::thread> iconTh_;   // 4 个工作线程（冷启动每个 exe 约 70 ms，磁盘/签名读取）
    bool iconQuit_ = false;
    std::atomic<bool> iconPosted_{false};
    HFONT font_ = nullptr, bold_ = nullptr;
    int rowH_ = 26;
    HDC memDc_ = nullptr; HBITMAP memBmp_ = nullptr; HGDIOBJ memOld_ = nullptr; int memW_ = 0, memH_ = 0;
    HIMAGELIST rowHeightIl_ = nullptr;
};

} // namespace ff
