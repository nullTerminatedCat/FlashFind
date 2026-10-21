#include "ResultList.h"
#include "Settings.h"
#include "Theme.h"
#include "Util.h"
#include "Dpi.h"
#include <shellapi.h>
#include <uxtheme.h>
#include <algorithm>

namespace ff {

static HIMAGELIST g_sysImages = nullptr;

HWND ResultList::Create(HWND parent, int id) {
    hwnd_ = CreateWindowExW(0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS,
        0, 0, 100, 100, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    ListView_SetExtendedListViewStyle(hwnd_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP | LVS_EX_INFOTIP);

    SHFILEINFOW sfi{};
    g_sysImages = reinterpret_cast<HIMAGELIST>(SHGetFileInfoW(L"C:\\", 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON));
    ListView_SetImageList(hwnd_, g_sysImages, LVSIL_SMALL);

    auto& s = GetSettings();
    UINT dpi = DpiOf(parent);
    struct { const wchar_t* t; int w; int fmt; } cols[] = {
        {L"名称", s.colName, LVCFMT_LEFT}, {L"路径", s.colPath, LVCFMT_LEFT},
        {L"大小", s.colSize, LVCFMT_RIGHT}, {L"修改时间", s.colDate, LVCFMT_LEFT}};
    for (int i = 0; i < ColCount; ++i) {
        LVCOLUMNW c{LVCF_TEXT | LVCF_WIDTH | LVCF_FMT};
        c.pszText = const_cast<wchar_t*>(cols[i].t);
        c.cx = Scale(cols[i].w, dpi);
        c.fmt = cols[i].fmt;
        ListView_InsertColumn(hwnd_, i, &c);
    }
    // 名称列显示升序箭头（M1 固定按名称排序）
    HWND hdr = ListView_GetHeader(hwnd_);
    HDITEMW hi{HDI_FORMAT};
    Header_GetItem(hdr, 0, &hi);
    hi.fmt |= HDF_SORTUP;
    Header_SetItem(hdr, 0, &hi);
    ApplyTheme();
    return hwnd_;
}

std::vector<int> ResultList::ColumnWidths() const {
    std::vector<int> w;
    UINT dpi = DpiOf(hwnd_);
    for (int i = 0; i < ColCount; ++i) w.push_back(MulDiv(ListView_GetColumnWidth(hwnd_, i), 96, static_cast<int>(dpi)));
    return w;
}

void ResultList::ApplyTheme() {
    auto& t = GetTheme();
    SetWindowTheme(hwnd_, t.Dark() ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    HWND hdr = ListView_GetHeader(hwnd_);
    SetWindowTheme(hdr, t.Dark() ? L"DarkMode_ItemsView" : L"ItemsView", nullptr);
    ListView_SetBkColor(hwnd_, t.P().surface);
    ListView_SetTextBkColor(hwnd_, t.P().surface);
    ListView_SetTextColor(hwnd_, t.P().text);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void ResultList::SetFont(HFONT f, HFONT bold) {
    font_ = f; bold_ = bold;
    SendMessageW(hwnd_, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
}

void ResultList::SetResults(std::shared_ptr<SearchResult> r) {
    res_ = std::move(r);
    int n = res_ ? static_cast<int>(res_->items.size()) : 0;
    ListView_SetItemState(hwnd_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemCountEx(hwnd_, n, LVSICF_NOSCROLL);
    if (n) ListView_EnsureVisible(hwnd_, 0, FALSE);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

int ResultList::SelectedCount() const { return ListView_GetSelectedCount(hwnd_); }
int ResultList::FocusedIndex() const { return ListView_GetNextItem(hwnd_, -1, LVNI_FOCUSED); }

std::vector<uint32_t> ResultList::Selection() const {
    std::vector<uint32_t> r;
    if (!res_) return r;
    for (int i = ListView_GetNextItem(hwnd_, -1, LVNI_SELECTED); i >= 0; i = ListView_GetNextItem(hwnd_, i, LVNI_SELECTED))
        r.push_back(res_->items[static_cast<size_t>(i)]);
    return r;
}

int ResultList::IconFor(uint32_t i) {
    const auto& d = *res_->data;
    if (d.entries[i].IsDir()) {
        if (folderIcon_ < 0) {
            SHFILEINFOW sfi{};
            SHGetFileInfoW(L"folder", FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
            folderIcon_ = sfi.iIcon;
        }
        return folderIcon_;
    }
    auto name = d.LowerName(i);
    auto dot = name.rfind(L'.');
    std::wstring ext = dot == std::wstring_view::npos ? L"" : std::wstring(name.substr(dot));
    auto it = iconCache_.find(ext);
    if (it != iconCache_.end()) return it->second;
    // 按扩展名取图标（不访问磁盘）；exe/lnk 的专属图标留给 M2 后台加载
    SHFILEINFOW sfi{};
    std::wstring fake = L"x" + ext;
    SHGetFileInfoW(fake.c_str(), FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
    iconCache_[ext] = sfi.iIcon;
    return sfi.iIcon;
}

void ResultList::DrawName(HDC dc, const RECT& rc, uint32_t i, bool selected) {
    const auto& d = *res_->data;
    const auto& pal = GetTheme().P();
    std::wstring_view name = d.Name(i);
    std::wstring_view lower = d.LowerName(i);
    // 匹配掩码（成员缓冲区，无逐行分配）
    if (name.size() > kRowBuf) { name = name.substr(0, kRowBuf); lower = lower.substr(0, kRowBuf); }
    char* mask = mask_;
    memset(mask, 0, name.size());
    for (auto& t : res_->terms) {
        if (t.empty() || t.find(L'\\') != std::wstring::npos) continue;
        for (size_t p = lower.find(t); p != std::wstring_view::npos; p = lower.find(t, p + 1))
            memset(mask + p, 1, std::min(t.size(), name.size() - p));
    }
    int x = rc.left;
    int y = rc.top;
    TEXTMETRICW tm; GetTextMetricsW(dc, &tm);
    int ty = y + ((rc.bottom - rc.top) - tm.tmHeight) / 2;
    size_t k = 0;
    while (k < name.size() && x < rc.right) {
        size_t e = k;
        while (e < name.size() && mask[e] == mask[k]) ++e;
        bool hl = mask[k] != 0;
        HGDIOBJ old = SelectObject(dc, hl && bold_ ? bold_ : font_);
        SIZE sz; GetTextExtentPoint32W(dc, name.data() + k, static_cast<int>(e - k), &sz);
        RECT seg{x, ty, std::min<LONG>(x + sz.cx, rc.right), ty + tm.tmHeight};
        if (hl && !selected) {
            HBRUSH b = CreateSolidBrush(pal.hl); FillRect(dc, &seg, b); DeleteObject(b);
            SetTextColor(dc, pal.hlText);
        } else {
            SetTextColor(dc, selected ? pal.selText : pal.text);
        }
        ExtTextOutW(dc, x, ty, ETO_CLIPPED, &rc, name.data() + k, static_cast<UINT>(e - k), nullptr);
        SelectObject(dc, old);
        x += sz.cx;
        k = e;
    }
}

LRESULT ResultList::CustomDraw(NMLVCUSTOMDRAW* cd) {
    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT: {
        if (!res_) return CDRF_DODEFAULT;
        int row = static_cast<int>(cd->nmcd.dwItemSpec);
        if (row < 0 || static_cast<size_t>(row) >= res_->items.size()) return CDRF_DODEFAULT;
        uint32_t i = res_->items[static_cast<size_t>(row)];
        const auto& d = *res_->data;
        const auto& pal = GetTheme().P();
        HDC dc = cd->nmcd.hdc;
        bool sel = ListView_GetItemState(hwnd_, row, LVIS_SELECTED) != 0;
        bool focusList = GetFocus() == hwnd_;

        RECT row_rc; ListView_GetItemRect(hwnd_, row, &row_rc, LVIR_BOUNDS);
        COLORREF bg = sel ? pal.sel : ((row & 1) ? pal.alt : pal.surface);
        if (sel && !focusList) bg = pal.hover;
        HBRUSH b = CreateSolidBrush(bg); FillRect(dc, &row_rc, b); DeleteObject(b);
        if (sel) { // 左侧强调条
            UINT dpi = DpiOf(hwnd_);
            RECT bar{row_rc.left, row_rc.top + Scale(5, dpi), row_rc.left + Scale(3, dpi), row_rc.bottom - Scale(5, dpi)};
            HBRUSH ab = CreateSolidBrush(pal.accent); FillRect(dc, &bar, ab); DeleteObject(ab);
        }
        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ oldFont = SelectObject(dc, font_);

        // 名称列：图标 + 高亮文字
        RECT ic; ListView_GetSubItemRect(hwnd_, row, 0, LVIR_ICON, &ic);
        int iconIdx = IconFor(i);
        int isz = 16; ImageList_GetIconSize(g_sysImages, &isz, &isz);
        ImageList_Draw(g_sysImages, iconIdx, dc, ic.left + 4, ic.top + ((ic.bottom - ic.top) - isz) / 2, ILD_TRANSPARENT);
        RECT lab; ListView_GetSubItemRect(hwnd_, row, 0, LVIR_LABEL, &lab);
        lab.left += 6; lab.right -= 4;
        DrawName(dc, lab, i, false);

        auto& e = d.entries[i];
        SetTextColor(dc, pal.text2);
        for (int c = 1; c < ColCount; ++c) {
            wchar_t* buf = rowBuf_; int len = 0;
            if (c == ColPath) len = static_cast<int>(d.ParentPathInto(i, rowBuf_, kRowBuf));
            else if (c == ColSize) len = e.IsDir() ? 0 : FormatSizeInto(e.size, rowBuf_, kRowBuf);
            else len = FormatTimeInto(e.mtime, rowBuf_, kRowBuf);
            RECT r; ListView_GetSubItemRect(hwnd_, row, c, LVIR_BOUNDS, &r);
            r.left += 6; r.right -= 6;
            DrawTextW(dc, buf, len, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS | (c == ColSize ? DT_RIGHT : DT_LEFT));
        }
        SelectObject(dc, oldFont);
        return CDRF_SKIPDEFAULT;
    }
    }
    return CDRF_DODEFAULT;
}

bool ResultList::OnNotify(NMHDR* nm, LRESULT* out) {
    if (nm->hwndFrom != hwnd_) return false;
    switch (nm->code) {
    case LVN_GETDISPINFOW: {
        auto* di = reinterpret_cast<NMLVDISPINFOW*>(nm);
        if (!res_ || di->item.iItem < 0 || static_cast<size_t>(di->item.iItem) >= res_->items.size()) return true;
        uint32_t i = res_->items[static_cast<size_t>(di->item.iItem)];
        const auto& d = *res_->data;
        if (di->item.mask & LVIF_TEXT) {
            wchar_t* o = di->item.pszText; size_t cap = static_cast<size_t>(di->item.cchTextMax);
            if (cap) {
                switch (di->item.iSubItem) {
                case ColName: { auto nv = d.Name(i); size_t c = std::min(nv.size(), cap - 1); wmemcpy(o, nv.data(), c); o[c] = 0; break; }
                case ColPath: d.ParentPathInto(i, o, cap); break;
                case ColSize: if (d.entries[i].IsDir()) o[0] = 0; else FormatSizeInto(d.entries[i].size, o, cap); break;
                case ColDate: FormatTimeInto(d.entries[i].mtime, o, cap); break;
                }
            }
        }
        if (di->item.mask & LVIF_IMAGE) di->item.iImage = IconFor(i);
        *out = 0;
        return true;
    }
    case LVN_ODFINDITEMW: {
        // 键入跳转：按名称前缀查找
        auto* fi = reinterpret_cast<NMLVFINDITEMW*>(nm);
        *out = -1;
        if (!res_ || !(fi->lvfi.flags & (LVFI_STRING | LVFI_PARTIAL))) return true;
        std::wstring pre = ToLower(fi->lvfi.psz);
        size_t n = res_->items.size();
        for (size_t k = 0; k < n; ++k) {
            size_t row = (static_cast<size_t>(std::max(fi->iStart, 0)) + k) % n;
            if (res_->data->LowerName(res_->items[row]).starts_with(pre)) { *out = static_cast<LRESULT>(row); break; }
        }
        return true;
    }
    case NM_CUSTOMDRAW:
        *out = CustomDraw(reinterpret_cast<NMLVCUSTOMDRAW*>(nm));
        return true;
    }
    return false;
}

} // namespace ff
