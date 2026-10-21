#pragma once
// 结果列表：虚拟 ListView（LVS_OWNERDATA），百万行无压力。
// M1 用 GDI custom draw 做配色/隔行/匹配高亮；M2 换成 Direct2D/DirectWrite 绘制（圆角选中、强调条）。
#include "Search.h"
#include <commctrl.h>
#include <unordered_map>

namespace ff {

enum Column { ColName = 0, ColPath, ColSize, ColDate, ColCount };

class ResultList {
public:
    HWND Create(HWND parent, int id);
    HWND Hwnd() const { return hwnd_; }
    void SetResults(std::shared_ptr<SearchResult> r);
    const SearchResult* Results() const { return res_.get(); }
    void ApplyTheme();
    void SetFont(HFONT f, HFONT bold);
    // 处理父窗口转发的 WM_NOTIFY；返回 true 表示已处理并写入 *out
    bool OnNotify(NMHDR* nm, LRESULT* out);

    int SelectedCount() const;
    std::vector<uint32_t> Selection() const;
    int FocusedIndex() const;
    uint32_t ItemAt(int row) const { return res_->items[static_cast<size_t>(row)]; }
    std::vector<int> ColumnWidths() const;

private:
    int IconFor(uint32_t i);
    LRESULT CustomDraw(NMLVCUSTOMDRAW* cd);
    void DrawName(HDC dc, const RECT& rc, uint32_t i, bool selected);

    HWND hwnd_ = nullptr;
    std::shared_ptr<SearchResult> res_;
    static constexpr size_t kRowBuf = 4096;
    wchar_t rowBuf_[kRowBuf];
    char mask_[kRowBuf];
    std::unordered_map<std::wstring, int> iconCache_;  // 按扩展名缓存，仅首次遇到新扩展名时分配
    int folderIcon_ = -1;
    HFONT font_ = nullptr, bold_ = nullptr;
};

} // namespace ff
