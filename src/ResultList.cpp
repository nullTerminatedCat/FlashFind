#include "ResultList.h"
#include "Dpi.h"
#include "Fold.h"
#include "Settings.h"
#include "Theme.h"
#include "Util.h"
#include "App.h"
#include <shellapi.h>
#include <objbase.h>
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
        {L"扩展名", s.colExt, LVCFMT_LEFT},
        {L"大小", s.colSize, LVCFMT_RIGHT}, {L"修改时间", s.colDate, LVCFMT_LEFT}};
    for (int i = 0; i < ColCount; ++i) {
        LVCOLUMNW c{LVCF_TEXT | LVCF_WIDTH | LVCF_FMT};
        c.pszText = const_cast<wchar_t*>(cols[i].t);
        c.cx = Scale(cols[i].w, dpi);
        c.fmt = cols[i].fmt;
        ListView_InsertColumn(hwnd_, i, &c);
    }
    SetSort({static_cast<SortCol>(s.sortCol), s.sortDesc});
    UpdateHiddenColumns();
    ApplyTheme();
    return hwnd_;
}

// 同列切换升/降序；新列：名称/路径升序，大小/时间降序
void ResultList::ClickColumn(int c) {
    if (c < 0 || c >= ColCount) return;
    SortSpec s = sort_;
    SortCol col = static_cast<SortCol>(c);
    if (s.col == col) s.desc = !s.desc; else { s.col = col; s.desc = col == SortCol::Size || col == SortCol::Date; }
    SetSort(s);
    auto& st = GetSettings(); st.sortCol = static_cast<int>(s.col); st.sortDesc = s.desc;
    ++sortClicks_;
    PostMessageW(GetParent(hwnd_), WM_APP_SORT, 0, 0);
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

bool ResultList::GetSettingsVisible(int c) { return c <= 0 || c >= ColCount || GetSettings().colVisible[c]; }

// 隐藏列 = 宽度 0 + HDF_FIXEDWIDTH（不可拖宽），绘制时宽度 0 的列自动跳过；记住的宽度保存在设置里
void ResultList::UpdateHiddenColumns() {
    HWND hdr = ListView_GetHeader(hwnd_);
    auto& s = GetSettings();
    int saved[ColCount] = {s.colName, s.colPath, s.colExt, s.colSize, s.colDate};
    UINT dpi = DpiOf(hwnd_);
    for (int c = 1; c < ColCount; ++c) {
        bool vis = s.colVisible[c];
        HDITEMW hi{HDI_FORMAT}; Header_GetItem(hdr, c, &hi);
        hi.fmt = vis ? (hi.fmt & ~HDF_FIXEDWIDTH) : (hi.fmt | HDF_FIXEDWIDTH);
        Header_SetItem(hdr, c, &hi);
        int cur = ListView_GetColumnWidth(hwnd_, c);
        if (!vis && cur != 0) ListView_SetColumnWidth(hwnd_, c, 0);
        if (vis && cur == 0) ListView_SetColumnWidth(hwnd_, c, Scale(std::max(saved[c], 40), dpi));
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ResultList::SetColumnVisible(int c, bool v) {
    if (c <= ColName || c >= ColCount) return;
    auto& s = GetSettings();
    if (!v) {   // 记住当前宽度
        int w = MulDiv(ListView_GetColumnWidth(hwnd_, c), 96, static_cast<int>(DpiOf(hwnd_)));
        if (w > 0) {
            if (c == ColPath) s.colPath = w; else if (c == ColExt) s.colExt = w;
            else if (c == ColSize) s.colSize = w; else if (c == ColDate) s.colDate = w;
        }
    }
    s.colVisible[c] = v;
    UpdateHiddenColumns();
}

void ResultList::ColumnMenu(POINT pt) {
    HMENU m = CreatePopupMenu();
    const wchar_t* names[] = {L"名称", L"路径", L"扩展名", L"大小", L"修改时间"};
    for (int c = 0; c < ColCount; ++c)
        AppendMenuW(m, MF_STRING | (ColumnVisible(c) ? MF_CHECKED : 0) | (c == ColName ? MF_GRAYED : 0), 1 + c, names[c]);
    int cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(m);
    if (cmd > 1) SetColumnVisible(cmd - 1, !ColumnVisible(cmd - 1));
}

RECT ResultList::NameTextRect(int row) const {
    HWND hdr = ListView_GetHeader(hwnd_);
    RECT hr{}; GetWindowRect(hdr, &hr); MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&hr), 2);
    RECT c{}; Header_GetItemRect(hdr, ColName, &c);
    RECT ir{}; ListView_GetItemRect(hwnd_, row, &ir, LVIR_BOUNDS);
    UINT dpi = DpiOf(hwnd_);
    int isz = 16, isy = 16; if (g_sysImages) ImageList_GetIconSize(g_sysImages, &isz, &isy);
    int pad = Scale(6, dpi);
    return {c.left + hr.left + pad + isz + pad - Scale(3, dpi), ir.top, c.right + hr.left - pad, ir.bottom};
}

bool ResultList::WantsRealIcon(std::wstring_view ext) {
    static const wchar_t* k[] = {L".exe", L".lnk", L".ico", L".url", L".cur", L".ani", L".scr", L".msc", L".cpl", L".appref-ms"};
    for (auto e : k) { std::wstring_view v(e); if (v.size() == ext.size() && CompareFold(v, ext) == 0) return true; }
    return false;
}

void ResultList::IconWorker() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    for (;;) {
        std::wstring path;
        {
            std::unique_lock lk(iconMu_);
            iconCv_.wait(lk, [&] { return iconQuit_ || !iconQ_.empty(); });
            if (iconQuit_) break;
            path = std::move(iconQ_.back()); iconQ_.pop_back();
        }
        SHFILEINFOW sfi{};
        int idx = SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON) ? sfi.iIcon : -2;
        {
            std::lock_guard lk(iconMu_);
            iconPending_.erase(path);
            if (realIcon_.size() > 100000) realIcon_.clear();
            realIcon_[path] = idx;
        }
        if (!iconPosted_.exchange(true)) PostMessageW(hwnd_, WM_APP_ICONS, 0, 0);
    }
    CoUninitialize();
}

ResultList::~ResultList() {
    if (!iconTh_.empty()) { { std::lock_guard lk(iconMu_); iconQuit_ = true; } iconCv_.notify_all(); for (auto& t : iconTh_) t.join(); }
    if (memDc_) { SelectObject(memDc_, memOld_); DeleteObject(memBmp_); DeleteDC(memDc_); }
    if (rowHeightIl_) ImageList_Destroy(rowHeightIl_);
}

LRESULT CALLBACK ResultList::SubclassProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) {
    auto* self = reinterpret_cast<ResultList*>(ref);
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_APP_ICONS: self->iconPosted_ = false; InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_PAINT: {
        if (w) { RECT rc; GetClientRect(h, &rc); self->Paint(reinterpret_cast<HDC>(w), rc); return 0; }
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        self->Paint(dc, ps.rcPaint);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_PRINTCLIENT: { RECT rc; GetClientRect(h, &rc); self->Paint(reinterpret_cast<HDC>(w), rc); return 0; }
    // 滚动：ListView 内部用 ScrollWindowEx 搬移旧像素，只让露出的条带失效；我们自绘时旧像素与新顶行
    // 可能错位（平滑滚动/部分行）造成重影。滚动后整块客户区重绘（双缓冲，单帧约 1 ms）。
    case WM_VSCROLL: case WM_HSCROLL: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: case LVM_SCROLL: case LVM_ENSUREVISIBLE: {
        int top = ListView_GetTopIndex(h); int hx = GetScrollPos(h, SB_HORZ);
        LRESULT r = DefSubclassProc(h, m, w, l);
        if (top != ListView_GetTopIndex(h) || hx != GetScrollPos(h, SB_HORZ)) InvalidateRect(h, nullptr, FALSE);
        return r;
    }
    case WM_KEYDOWN: {
        // 选中高亮：Invalidate 即可（与 ListView 局部失效错位）；勿 UpdateWindow，否则方向键连按时同步全表重绘卡顿
        LRESULT r = DefSubclassProc(h, m, w, l);
        InvalidateRect(h, nullptr, FALSE);
        return r;
    }
    // 鼠标改选中：ListView 按内部行矩形局部失效，与自绘行高/顶行可能错位 → 旧高亮残留
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_RBUTTONDOWN: {
        LRESULT r = DefSubclassProc(h, m, w, l);
        InvalidateRect(h, nullptr, FALSE);
        return r;
    }
    case WM_SETFOCUS: case WM_KILLFOCUS: InvalidateRect(h, nullptr, FALSE); break;
    case WM_NOTIFY: {   // 列头通知先到列表本身
        auto* nm = reinterpret_cast<NMHDR*>(l);
        if (nm->hwndFrom == ListView_GetHeader(h)) {
            if (nm->code == NM_RCLICK) { POINT pt; GetCursorPos(&pt); self->ColumnMenu(pt); return TRUE; }
            if (nm->code == HDN_ITEMCLICKW || nm->code == HDN_ITEMCLICKA || nm->code == HDN_ITEMDBLCLICKW || nm->code == HDN_ITEMDBLCLICKA) {   // 快速连点第二下是双击   // 列头点击排序（不依赖 LVN_COLUMNCLICK 是否转发到主窗口）
                auto* hn = reinterpret_cast<NMHEADERW*>(l);
                // 双击时：第二下按下发 ITEMDBLCLICK，松开可能再发 ITEMCLICK —— 只算一次
                bool dbl = nm->code == HDN_ITEMDBLCLICKW || nm->code == HDN_ITEMDBLCLICKA;
                ULONGLONG now = GetTickCount64();
                bool dup = !dbl && hn->iItem == self->lastDblCol_ && now - self->lastDblTick_ < 300;
                if (dbl) { self->lastDblCol_ = hn->iItem; self->lastDblTick_ = now; }
                if (hn->iButton == 0 && !dup) self->ClickColumn(hn->iItem);
                return 0;
            }
            if (nm->code == HDN_BEGINTRACKW || nm->code == HDN_BEGINTRACKA || nm->code == HDN_DIVIDERDBLCLICKW || nm->code == HDN_DIVIDERDBLCLICKA) {
                int c = reinterpret_cast<NMHEADERW*>(l)->iItem;
                if (!self->ColumnVisible(c)) return TRUE;   // 隐藏列不可拖宽
            }
        }
        break;
    }
    case WM_NCDESTROY: RemoveWindowSubclass(h, SubclassProc, 1); break;
    }
    return DefSubclassProc(h, m, w, l);
}

std::vector<int> ResultList::ColumnWidths() const {
    std::vector<int> w;
    UINT dpi = DpiOf(hwnd_);
    auto& s = GetSettings();
    int saved[ColCount] = {s.colName, s.colPath, s.colExt, s.colSize, s.colDate};
    for (int i = 0; i < ColCount; ++i) w.push_back(ColumnVisible(i) ? MulDiv(ListView_GetColumnWidth(hwnd_, i), 96, static_cast<int>(dpi)) : saved[i]);
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
    std::unordered_set<uint32_t> keep;
    uint32_t focusEntry = UINT32_MAX;
    if (res_ && !sel_.empty()) {
        for (size_t row = 0; row < sel_.size() && row < res_->items.size(); ++row)
            if (sel_[row]) keep.insert(res_->items[row]);
        if (focusRow_ >= 0 && static_cast<size_t>(focusRow_) < res_->items.size())
            focusEntry = res_->items[static_cast<size_t>(focusRow_)];
    }

    {
        std::lock_guard lk(iconMu_);
        for (auto& pth : iconQ_) iconPending_.erase(pth);
        iconQ_.clear();
    }
    res_ = std::move(r);
    // 保留仍出现在新结果中的磁盘覆盖（F5 软刷新后索引重搜不应丢掉刚读的 size/mtime）
    if (!attrOv_.empty()) {
        if (!res_ || !res_->data) attrOv_.clear();
        else {
            std::unordered_set<uint32_t> alive(res_->items.begin(), res_->items.end());
            for (auto it = attrOv_.begin(); it != attrOv_.end(); ) {
                if (!alive.count(it->first)) it = attrOv_.erase(it);
                else ++it;
            }
        }
    }
    int n = res_ ? static_cast<int>(res_->items.size()) : 0;
    sel_.assign(static_cast<size_t>((std::max)(0, n)), 0);
    focusRow_ = -1;
    ListView_SetItemCountEx(hwnd_, n, LVSICF_NOSCROLL);

    int restored = 0;
    if (res_ && !keep.empty()) {
        for (int row = 0; row < n; ++row) {
            uint32_t e = res_->items[static_cast<size_t>(row)];
            if (!keep.count(e)) continue;
            sel_[static_cast<size_t>(row)] = 1;
            ++restored;
            if (e == focusEntry) focusRow_ = row;
        }
        if (focusRow_ < 0 && restored > 0) {
            for (int row = 0; row < n; ++row)
                if (sel_[static_cast<size_t>(row)]) { focusRow_ = row; break; }
        }
        ApplySelToListView();
    } else {
        ListView_SetItemState(hwnd_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        if (n) ListView_EnsureVisible(hwnd_, 0, FALSE);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

int ResultList::SelectedCount() const {
    int c = 0;
    for (uint8_t v : sel_) if (v) ++c;
    return c;
}

int ResultList::FocusedIndex() const { return focusRow_; }

std::vector<uint32_t> ResultList::Selection() const {
    std::vector<uint32_t> out;
    if (!res_) return out;
    out.reserve(static_cast<size_t>(SelectedCount()));
    for (size_t i = 0; i < sel_.size() && i < res_->items.size(); ++i)
        if (sel_[i]) out.push_back(res_->items[i]);
    return out;
}

void ResultList::SelectAll() {
    if (sel_.empty()) return;
    std::fill(sel_.begin(), sel_.end(), static_cast<uint8_t>(1));
    if (focusRow_ < 0) focusRow_ = 0;
    // 大数据量时逐条 SetItemState 会卡 UI；OWNERDATA 支持一次全选
    applyingSel_ = true;
    ListView_SetItemState(hwnd_, -1, LVIS_SELECTED, LVIS_SELECTED);
    ListView_SetItemState(hwnd_, focusRow_, LVIS_FOCUSED | LVIS_SELECTED, LVIS_FOCUSED | LVIS_SELECTED);
    applyingSel_ = false;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ResultList::ClearSelection() {
    if (sel_.empty()) return;
    std::fill(sel_.begin(), sel_.end(), static_cast<uint8_t>(0));
    applyingSel_ = true;
    ListView_SetItemState(hwnd_, -1, 0, LVIS_SELECTED);
    if (focusRow_ >= 0 && static_cast<size_t>(focusRow_) < sel_.size())
        ListView_SetItemState(hwnd_, focusRow_, LVIS_FOCUSED, LVIS_FOCUSED);
    applyingSel_ = false;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ResultList::SyncSelFromNotify(int row, UINT oldSt, UINT newSt) {
    if (applyingSel_) return;
    if (row < 0 || static_cast<size_t>(row) >= sel_.size()) return;
    if ((oldSt ^ newSt) & LVIS_SELECTED)
        sel_[static_cast<size_t>(row)] = (newSt & LVIS_SELECTED) ? 1 : 0;
    if ((oldSt ^ newSt) & LVIS_FOCUSED) {
        if (newSt & LVIS_FOCUSED) focusRow_ = row;
        else if (focusRow_ == row) focusRow_ = -1;
    }
}

void ResultList::SyncSelRange(int from, int to, UINT oldSt, UINT newSt) {
    if (applyingSel_) return;
    if (sel_.empty() || from < 0) return;
    int a = from, b = to;
    if (b < a) std::swap(a, b);
    if (a >= static_cast<int>(sel_.size())) return;
    b = (std::min)(b, static_cast<int>(sel_.size()) - 1);
    if ((oldSt ^ newSt) & LVIS_SELECTED) {
        const uint8_t v = (newSt & LVIS_SELECTED) ? 1 : 0;
        for (int i = a; i <= b; ++i) sel_[static_cast<size_t>(i)] = v;
    }
}

void ResultList::ApplySelToListView() {
    // ListView_SetItemState 会触发 LVN；applyingSel_ 期间忽略回写，避免清空 sel_。
    applyingSel_ = true;
    ListView_SetItemState(hwnd_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    int nSel = 0;
    for (uint8_t v : sel_) if (v) ++nSel;
    if (nSel > 0 && nSel == static_cast<int>(sel_.size())) {
        ListView_SetItemState(hwnd_, -1, LVIS_SELECTED, LVIS_SELECTED);
    } else if (nSel > 0) {
        // 稀疏选中：逐条同步（恢复选中场景）；全选已走上面的 -1
        for (size_t i = 0; i < sel_.size(); ++i) {
            if (!sel_[i]) continue;
            ListView_SetItemState(hwnd_, static_cast<int>(i), LVIS_SELECTED, LVIS_SELECTED);
        }
    }
    if (focusRow_ >= 0 && static_cast<size_t>(focusRow_) < sel_.size()) {
        UINT st = LVIS_FOCUSED | (sel_[static_cast<size_t>(focusRow_)] ? LVIS_SELECTED : 0);
        ListView_SetItemState(hwnd_, focusRow_, st, LVIS_FOCUSED | LVIS_SELECTED);
    }
    applyingSel_ = false;
}

uint64_t ResultList::DisplaySize(uint32_t entry) const {
    auto it = attrOv_.find(entry);
    if (it != attrOv_.end()) return it->second.size;
    return res_ && res_->data ? res_->data->entries[entry].size : 0;
}

uint64_t ResultList::DisplayMtime(uint32_t entry) const {
    auto it = attrOv_.find(entry);
    if (it != attrOv_.end()) return it->second.mtime;
    return res_ && res_->data ? res_->data->entries[entry].mtime : 0;
}

void ResultList::RefreshAttrsFromDisk(int maxItems) {
    if (!res_ || !res_->data || maxItems <= 0) return;
    const size_t n = (std::min)(res_->items.size(), static_cast<size_t>(maxItems));
    for (size_t k = 0; k < n; ++k) {
        uint32_t id = res_->items[k];
        if (res_->data->entries[id].IsDir()) continue;
        std::wstring path = res_->data->FullPath(id);
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) continue;
        if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        ULARGE_INTEGER sz; sz.HighPart = fad.nFileSizeHigh; sz.LowPart = fad.nFileSizeLow;
        ULARGE_INTEGER mt; mt.HighPart = fad.ftLastWriteTime.dwHighDateTime; mt.LowPart = fad.ftLastWriteTime.dwLowDateTime;
        attrOv_[id] = AttrOv{sz.QuadPart, mt.QuadPart};
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
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
    if (WantsRealIcon(ext)) {
        std::wstring path = d.FullPath(i);
        std::lock_guard lk(iconMu_);
        auto rit = realIcon_.find(path);
        if (rit != realIcon_.end()) { if (rit->second >= 0) return rit->second; }
        else if (iconPending_.insert(path).second) {
            iconQ_.push_back(std::move(path));
            if (iconQ_.size() > 512) { for (size_t k = 0; k < 256; ++k) iconPending_.erase(iconQ_[k]); iconQ_.erase(iconQ_.begin(), iconQ_.begin() + 256); }
            if (iconTh_.empty()) for (int k = 0; k < 4; ++k) iconTh_.emplace_back([this] { IconWorker(); });
            iconCv_.notify_one();
        }
    }
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
    bool sel = row >= 0 && static_cast<size_t>(row) < sel_.size() && sel_[static_cast<size_t>(row)] != 0;
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
        else if (c == ColExt) {
            if (!e.IsDir()) {
                auto nm = d.Name(i); size_t dot = nm.rfind(L'.');
                if (dot != std::wstring_view::npos && dot + 1 < nm.size()) {
                    size_t n = std::min(nm.size() - dot - 1, kRowBuf - 1);
                    wmemcpy(rowBuf_, nm.data() + dot + 1, n); rowBuf_[n] = 0; len = static_cast<int>(n);
                }
            }
        } else if (c == ColSize) len = e.IsDir() ? 0 : FormatSizeInto(DisplaySize(i), rowBuf_, kRowBuf);
        else if (c == ColDate) len = FormatTimeInto(DisplayMtime(i), rowBuf_, kRowBuf);
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
    // 水平偏移取滚动条位置（列头窗口的移动可能晚于本次绘制）
    const int hx = -GetScrollPos(hwnd_, SB_HORZ);
    hr.left = hx;
    for (int c = 0; c < ColCount; ++c) {
        RECT r{}; Header_GetItemRect(hdr, c, &r);
        cols[c] = {r.left + hx, 0, r.right + hx, 0};
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
            DrawRow(dc, row, rr, cols, focused);
        }
    }
    SelectObject(dc, of);
    (void)clip;   // 总是按当前顶行重画并输出整个列表区（不信任残留像素）
    BitBlt(target, 0, top, cl.right, cl.bottom - top, dc, 0, top, SRCCOPY);
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
            case ColExt: {
                o[0] = 0;
                if (!d.entries[i].IsDir()) {
                    auto nm = d.Name(i); size_t dot = nm.rfind(L'.');
                    if (dot != std::wstring_view::npos && dot + 1 < nm.size()) {
                        size_t n = std::min(nm.size() - dot - 1, cap - 1);
                        wmemcpy(o, nm.data() + dot + 1, n); o[n] = 0;
                    }
                }
                break;
            }
            case ColSize: if (d.entries[i].IsDir()) o[0] = 0; else FormatSizeInto(d.entries[i].size, o, cap); break;
            case ColDate: FormatTimeInto(d.entries[i].mtime, o, cap); break;
            }
        }
        return true;
    }
    case LVN_COLUMNCLICK: *out = 0; return true;   // 已在列表子类化中按 HDN_ITEMCLICK 处理
    case LVN_ITEMCHANGED: {
        auto* lv = reinterpret_cast<NMLISTVIEW*>(nm);
        if ((lv->uChanged & LVIF_STATE) &&
            ((lv->uOldState ^ lv->uNewState) & (LVIS_SELECTED | LVIS_FOCUSED))) {
            SyncSelFromNotify(lv->iItem, lv->uOldState, lv->uNewState);
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        *out = 0; return true;
    }
    case LVN_ODSTATECHANGED: {
        auto* od = reinterpret_cast<NMLVODSTATECHANGE*>(nm);
        SyncSelRange(od->iFrom, od->iTo, od->uOldState, od->uNewState);
        InvalidateRect(hwnd_, nullptr, FALSE);
        *out = 0; return true;
    }
    case LVN_ENDSCROLL: InvalidateRect(hwnd_, nullptr, FALSE); *out = 0; return true;
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
