#include "ResultList.h"
#include "Dpi.h"
#include "Fold.h"
#include "Settings.h"
#include "Theme.h"
#include "Util.h"
#include "App.h"
#include <shellapi.h>
#include <uxtheme.h>
#include <algorithm>

namespace ff {

static HIMAGELIST g_sysImages = nullptr;

HWND ResultList::Create(HWND parent, int id) {
    hwnd_ = CreateWindowExW(0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPCHILDREN | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS,
        0, 0, 100, 100, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    ListView_SetExtendedListViewStyle(hwnd_, LVS_EX_FULLROWSELECT | LVS_EX_HEADERDRAGDROP);
    SetWindowSubclass(hwnd_, SubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));

    SHFILEINFOW sfi{};
    g_sysImages = reinterpret_cast<HIMAGELIST>(SHGetFileInfoW(L"C:\\", 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON));

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
    SetSort({static_cast<SortCol>(s.sortCol), s.sortDesc});
    ApplyTheme();
    return hwnd_;
}

void ResultList::SetSort(SortSpec s) {
    sort_ = s;
    HWND hdr = ListView_GetHeader(hwnd_);
    for (int c = 0; c < ColCount; ++c) {
        HDITEMW hi{HDI_FORMAT};
        Header_GetItem(hdr, c, &hi);
        hi.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (c == static_cast<int>(s.col)) hi.fmt |= s.desc ? HDF_SORTDOWN : HDF_SORTUP;
        Header_SetItem(hdr, c, &hi);
    }
}

ResultList::~ResultList() {
    if (memDc_) { SelectObject(memDc_, memOld_); DeleteObject(memBmp_); DeleteDC(memDc_); }
    if (rowHeightIl_) ImageList_Destroy(rowHeightIl_);
}

LRESULT CALLBACK ResultList::SubclassProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) {
    auto* self = reinterpret_cast<ResultList*>(ref);
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        if (w) { RECT rc; GetClientRect(h, &rc); self->Paint(reinterpret_cast<HDC>(w), rc); return 0; }
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        self->Paint(dc, ps.rcPaint);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_PRINTCLIENT: { RECT rc; GetClientRect(h, &rc); self->Paint(reinterpret_cast<HDC>(w), rc); return 0; }
    case WM_SETFOCUS: case WM_KILLFOCUS: InvalidateRect(h, nullptr, FALSE); break;
    case WM_NCDESTROY: RemoveWindowSubclass(h, SubclassProc, 1); break;
    }
    return DefSubclassProc(h, m, w, l);
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
    SetWindowTheme(ListView_GetHeader(hwnd_), t.Dark() ? L"DarkMode_ItemsView" : L"ItemsView", nullptr);
    ListView_SetBkColor(hwnd_, t.P().surface);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ResultList::SetFont(HFONT f, HFONT bold, int rowHeight) {
    font_ = f; bold_ = bold; rowH_ = rowHeight;
    SendMessageW(hwnd_, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
    // 让 ListView 内部的行高（滚动/命中测试）与我们绘制的一致
    if (rowHeightIl_) ImageList_Destroy(rowHeightIl_);
    rowHeightIl_ = ImageList_Create(1, rowHeight, ILC_COLOR, 1, 0);
    ListView_SetImageList(hwnd_, rowHeightIl_, LVSIL_SMALL);
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
    auto name = d.Name(i);
    auto dot = name.rfind(L'.');
    std::wstring_view ext = dot == std::wstring_view::npos ? std::wstring_view{} : name.substr(dot);
    if (ext.size() > 16) ext = {};
    uint64_t h = 1469598103934665603ULL;
    for (wchar_t c : ext) { h ^= static_cast<uint16_t>(Fold(c)); h *= 1099511628211ULL; }
    auto it = iconCache_.find(h);
    if (it != iconCache_.end()) return it->second;
    wchar_t fake[24] = L"x";
    wmemcpy(fake + 1, ext.data(), ext.size()); fake[1 + ext.size()] = 0;
    SHFILEINFOW sfi{};
    SHGetFileInfoW(fake, FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
    iconCache_.emplace(h, sfi.iIcon);
    return sfi.iIcon;
}

void ResultList::DrawName(HDC dc, const RECT& rc, uint32_t i, bool selected) {
    const auto& d = *res_->data;
    const auto& pal = GetTheme().P();
    std::wstring_view name = d.Name(i);
    if (name.size() > kRowBuf) name = name.substr(0, kRowBuf);
    char* mask = mask_;
    memset(mask, 0, name.size());
    for (auto& t : res_->terms) {
        if (t.empty() || t.find(L'\\') != std::wstring::npos) continue;
        // 通配符词：按 * ? 拆成字面片段分别高亮
        std::wstring_view tv(t);
        for (size_t b = 0; b <= tv.size();) {
            size_t e = tv.find_first_of(L"*?", b); if (e == std::wstring_view::npos) e = tv.size();
            std::wstring_view lit = tv.substr(b, e - b);
            if (!lit.empty())
                for (size_t p = FindFold(name, lit, 0); p != std::wstring_view::npos; p = FindFold(name, lit, p + 1))
                    memset(mask + p, 1, std::min(lit.size(), name.size() - p));
            b = e + 1;
        }
    }
    TEXTMETRICW tm; GetTextMetricsW(dc, &tm);
    int x = rc.left;
    int ty = rc.top + ((rc.bottom - rc.top) - tm.tmHeight) / 2;
    size_t k = 0;
    while (k < name.size() && x < rc.right) {
        size_t e = k;
        while (e < name.size() && mask[e] == mask[k]) ++e;
        bool hl = mask[k] != 0;
        HGDIOBJ old = SelectObject(dc, hl && bold_ ? bold_ : font_);
        SIZE sz; GetTextExtentPoint32W(dc, name.data() + k, static_cast<int>(e - k), &sz);
        if (hl && !selected) {
            RECT seg{x, ty, std::min<LONG>(x + sz.cx, rc.right), ty + tm.tmHeight};
            SetBkColor(dc, pal.hl);
            ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &seg, nullptr, 0, nullptr);
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

void ResultList::DrawRow(HDC dc, int row, const RECT& rr, const RECT* cols, bool focused) {
    const auto& pal = GetTheme().P();
    uint32_t i = res_->items[static_cast<size_t>(row)];
    const auto& d = *res_->data;
    const auto& e = d.entries[i];
    bool sel = ListView_GetItemState(hwnd_, row, LVIS_SELECTED) != 0;
    COLORREF bg = sel ? (focused ? pal.sel : pal.hover) : ((row & 1) ? pal.alt : pal.surface);
    SetBkColor(dc, bg);
    ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &rr, nullptr, 0, nullptr);
    UINT dpi = DpiOf(hwnd_);
    if (sel) {
        RECT bar{rr.left, rr.top + Scale(5, dpi), rr.left + Scale(3, dpi), rr.bottom - Scale(5, dpi)};
        SetBkColor(dc, pal.accent);
        ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &bar, nullptr, 0, nullptr);
    }
    SetBkMode(dc, TRANSPARENT);
    int pad = Scale(6, dpi);
    for (int c = 0; c < ColCount; ++c) {
        RECT r{cols[c].left, rr.top, cols[c].right, rr.bottom};
        if (r.right <= r.left) continue;
        if (c == ColName) {
            int isz = 16, isy = 16; ImageList_GetIconSize(g_sysImages, &isz, &isy);
            int ix = r.left + pad;
            ImageList_Draw(g_sysImages, IconFor(i), dc, ix, r.top + (rowH_ - isy) / 2, ILD_TRANSPARENT);
            RECT t{ix + isz + pad, r.top, r.right - pad, r.bottom};
            DrawName(dc, t, i, sel);
            continue;
        }
        int len = 0;
        if (c == ColPath) len = static_cast<int>(d.ParentPathInto(i, rowBuf_, kRowBuf));
        else if (c == ColSize) len = e.IsDir() ? 0 : FormatSizeInto(e.size, rowBuf_, kRowBuf);
        else len = FormatTimeInto(e.mtime, rowBuf_, kRowBuf);
        RECT t{r.left + pad, r.top, r.right - pad, r.bottom};
        SetTextColor(dc, pal.text2);
        DrawTextW(dc, rowBuf_, len, &t, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS | (c == ColSize ? DT_RIGHT : DT_LEFT));
    }
    SetBkMode(dc, OPAQUE);
}

void ResultList::Paint(HDC target, const RECT& clip) {
    RECT cl; GetClientRect(hwnd_, &cl);
    int w = std::max<int>(1, cl.right), h = std::max<int>(1, cl.bottom);
    if (!memDc_ || w > memW_ || h > memH_) {   // 双缓冲位图只在变大时重建
        if (memDc_) { SelectObject(memDc_, memOld_); DeleteObject(memBmp_); DeleteDC(memDc_); }
        memW_ = std::max(w, memW_); memH_ = std::max(h, memH_);
        memDc_ = CreateCompatibleDC(target);
        memBmp_ = CreateCompatibleBitmap(target, memW_, memH_);
        memOld_ = SelectObject(memDc_, memBmp_);
    }
    HDC dc = memDc_;
    const auto& pal = GetTheme().P();
    SetBkColor(dc, pal.surface);
    ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &cl, nullptr, 0, nullptr);

    HWND hdr = ListView_GetHeader(hwnd_);
    RECT hr{}; GetWindowRect(hdr, &hr); MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&hr), 2);
    int top = hr.bottom;
    RECT cols[ColCount]{}; int right = cl.right;
    for (int c = 0; c < ColCount; ++c) {
        RECT r{}; Header_GetItemRect(hdr, c, &r);
        cols[c] = {r.left + hr.left, 0, r.right + hr.left, 0};
        right = std::max<int>(right, cols[c].right);
    }
    HGDIOBJ of = SelectObject(dc, font_);
    if (res_ && res_->data && !res_->items.empty()) {
        int count = static_cast<int>(res_->items.size());
        int first = ListView_GetTopIndex(hwnd_);
        bool focused = GetFocus() == hwnd_;
        for (int row = first; row < count; ++row) {
            int y = top + (row - first) * rowH_;
            if (y >= cl.bottom) break;
            RECT rr{hr.left, y, right, y + rowH_};
            if (rr.bottom <= clip.top || rr.top >= clip.bottom) continue;
            DrawRow(dc, row, rr, cols, focused);
        }
    }
    SelectObject(dc, of);
    BitBlt(target, clip.left, std::max<int>(clip.top, top), clip.right - clip.left,
           clip.bottom - std::max<int>(clip.top, top), dc, clip.left, std::max<int>(clip.top, top), SRCCOPY);
}

bool ResultList::OnNotify(NMHDR* nm, LRESULT* out) {
    if (nm->hwndFrom != hwnd_) return false;
    switch (nm->code) {
    case LVN_GETDISPINFOW: {   // 供无障碍/提示使用；绘制不走这里
        auto* di = reinterpret_cast<NMLVDISPINFOW*>(nm);
        *out = 0;
        if (!res_ || di->item.iItem < 0 || static_cast<size_t>(di->item.iItem) >= res_->items.size()) return true;
        uint32_t i = res_->items[static_cast<size_t>(di->item.iItem)];
        const auto& d = *res_->data;
        if ((di->item.mask & LVIF_TEXT) && di->item.cchTextMax > 0) {
            wchar_t* o = di->item.pszText; size_t cap = static_cast<size_t>(di->item.cchTextMax);
            switch (di->item.iSubItem) {
            case ColName: { auto nv = d.Name(i); size_t c = std::min(nv.size(), cap - 1); wmemcpy(o, nv.data(), c); o[c] = 0; break; }
            case ColPath: d.ParentPathInto(i, o, cap); break;
            case ColSize: if (d.entries[i].IsDir()) o[0] = 0; else FormatSizeInto(d.entries[i].size, o, cap); break;
            case ColDate: FormatTimeInto(d.entries[i].mtime, o, cap); break;
            }
        }
        return true;
    }
    case LVN_COLUMNCLICK: {   // 同列切换升/降序；新列：名称/路径升序，大小/时间降序
        auto* lv = reinterpret_cast<NMLISTVIEW*>(nm);
        SortCol c = static_cast<SortCol>(lv->iSubItem);
        SortSpec s = sort_;
        if (s.col == c) s.desc = !s.desc; else { s.col = c; s.desc = c == SortCol::Size || c == SortCol::Date; }
        SetSort(s);
        auto& st = GetSettings(); st.sortCol = static_cast<int>(s.col); st.sortDesc = s.desc;
        PostMessageW(GetParent(hwnd_), WM_APP_SORT, 0, 0);
        *out = 0;
        return true;
    }
    case LVN_ODFINDITEMW: {   // 键入跳转：名称前缀（折叠比较）
        auto* fi = reinterpret_cast<NMLVFINDITEMW*>(nm);
        *out = -1;
        if (!res_ || !(fi->lvfi.flags & (LVFI_STRING | LVFI_PARTIAL)) || res_->items.empty()) return true;
        std::wstring_view pre(fi->lvfi.psz);
        size_t n = res_->items.size();
        for (size_t k = 0; k < n; ++k) {
            size_t row = (static_cast<size_t>(std::max(fi->iStart, 0)) + k) % n;
            auto nm2 = res_->data->Name(res_->items[row]);
            if (nm2.size() >= pre.size() && CompareFold(nm2.substr(0, pre.size()), pre) == 0) { *out = static_cast<LRESULT>(row); break; }
        }
        return true;
    }
    }
    return false;
}

} // namespace ff
