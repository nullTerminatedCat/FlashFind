#include "Fold.h"
#include "MainWindow.h"
#include "Dpi.h"
#include "Settings.h"
#include "Theme.h"
#include "Util.h"
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

namespace ff {

enum : int {
    IDC_SEARCH = 100, IDC_LIST,
    IDM_EXIT = 1000, IDM_REINDEX, IDM_THEME_SYSTEM, IDM_THEME_LIGHT, IDM_THEME_DARK,
    IDM_ABOUT, IDM_SHOW, IDM_OPEN, IDM_OPEN_PATH, IDM_COPY_PATH, IDM_COPY_NAME, IDM_PROPERTIES,
    IDM_DOUBLE_CTRL,
};

bool MainWindow::Create(int show) {
    HINSTANCE inst = App::Instance();
    iconBig_ = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(101), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
    iconSmall_ = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(101), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = iconBig_;
    wc.hIconSm = iconSmall_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kMainClass;
    RegisterClassExW(&wc);

    auto& s = GetSettings();
    UINT sdpi = GetDpiForSystem();
    int x = s.x == Settings::CW_USEDEFAULT_SENTINEL ? CW_USEDEFAULT : s.x;
    int y = s.x == Settings::CW_USEDEFAULT_SENTINEL ? CW_USEDEFAULT : s.y;
    hwnd_ = CreateWindowExW(0, kMainClass, kAppName, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            x, y, Scale(s.w, sdpi), Scale(s.h, sdpi), nullptr, nullptr, inst, this);
    if (!hwnd_) return false;
    if (show != SW_HIDE) {
        ShowWindow(hwnd_, s.maximized ? SW_SHOWMAXIMIZED : show);
        UpdateWindow(hwnd_);
        SetFocus(edit_);
    }
    return true;
}

LRESULT CALLBACK MainWindow::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    MainWindow* self = nullptr;
    if (m == WM_NCCREATE) {
        self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        self->hwnd_ = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    }
    return self ? self->Handle(m, w, l) : DefWindowProcW(h, m, w, l);
}

void MainWindow::OnCreate() {
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    edit_ = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                            0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(IDC_SEARCH), App::Instance(), nullptr);
    SendMessageW(edit_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"输入文件名搜索…（空格分隔多个词，含 \\ 则匹配路径）"));
    list_.Create(hwnd_, IDC_LIST);
    searcher_ = std::make_unique<Searcher>(hwnd_);
    BuildMenu();
    UpdateFonts();
    ApplyTheme();

    tray_.Add(hwnd_, iconSmall_);
    auto& s = GetSettings();
    if (!hotkeys_.RegisterToggle(hwnd_, s.toggleKey)) {
        hotkeyFailed_ = true;
        std::wstring msg = L"全局热键 " + s.toggleKey + L" 已被其他程序占用，请在设置中更换。";
        tray_.ShowBalloon(kAppName, msg.c_str());
    }
    if (s.doubleCtrl) hotkeys_.EnableDoubleCtrl(hwnd_);

    StartIndex(false);
}

void MainWindow::BuildMenu() {
    HMENU bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDM_REINDEX, L"重建索引(&R)\tF5");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_EXIT, L"退出(&X)\tCtrl+Q");
    HMENU view = CreatePopupMenu();
    HMENU theme = CreatePopupMenu();
    AppendMenuW(theme, MF_STRING, IDM_THEME_SYSTEM, L"跟随系统");
    AppendMenuW(theme, MF_STRING, IDM_THEME_LIGHT, L"浅色");
    AppendMenuW(theme, MF_STRING, IDM_THEME_DARK, L"深色");
    AppendMenuW(view, MF_POPUP, reinterpret_cast<UINT_PTR>(theme), L"主题(&T)");
    HMENU tools = CreatePopupMenu();
    AppendMenuW(tools, MF_STRING, IDM_DOUBLE_CTRL, L"双击 Ctrl 显示/隐藏");
    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, IDM_ABOUT, L"关于闪寻(&A)…");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"文件(&F)");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"视图(&V)");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(tools), L"工具(&T)");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"帮助(&H)");
    SetMenu(hwnd_, bar);
}

void MainWindow::UpdateFonts() {
    UINT dpi = DpiOf(hwnd_);
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfQuality = CLEARTYPE_QUALITY;
    if (font_) DeleteObject(font_);
    if (bold_) DeleteObject(bold_);
    font_ = CreateFontIndirectW(&lf);
    lf.lfWeight = FW_BOLD;
    bold_ = CreateFontIndirectW(&lf);
    SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    list_.SetFont(font_, bold_, Scale(26, dpi));
    Layout();
}

void MainWindow::ApplyTheme() {
    auto& t = GetTheme();
    t.Resolve(GetSettings().theme);
    t.ApplyToWindow(hwnd_);
    list_.ApplyTheme();
    HMENU bar = GetMenu(hwnd_);
    int sel = IDM_THEME_SYSTEM + static_cast<int>(GetSettings().theme);
    CheckMenuRadioItem(bar, IDM_THEME_SYSTEM, IDM_THEME_DARK, sel, MF_BYCOMMAND);
    CheckMenuItem(bar, IDM_DOUBLE_CTRL, MF_BYCOMMAND | (GetSettings().doubleCtrl ? MF_CHECKED : MF_UNCHECKED));
    InvalidateRect(hwnd_, nullptr, TRUE);
    InvalidateRect(edit_, nullptr, TRUE);
}

void MainWindow::Layout() {
    if (!edit_) return;
    RECT rc; GetClientRect(hwnd_, &rc);
    UINT dpi = DpiOf(hwnd_);
    int m = Scale(8, dpi);
    int sh = Scale(34, dpi);
    int stH = Scale(28, dpi);
    searchRc_ = {rc.left + m, rc.top + m, rc.right - m, rc.top + m + sh};
    HDC dc = GetDC(hwnd_);
    HGDIOBJ o = SelectObject(dc, font_);
    TEXTMETRICW tm; GetTextMetricsW(dc, &tm);
    SelectObject(dc, o); ReleaseDC(hwnd_, dc);
    int pad = Scale(10, dpi);
    int eh = tm.tmHeight;
    MoveWindow(edit_, searchRc_.left + pad, searchRc_.top + (sh - eh) / 2, (searchRc_.right - searchRc_.left) - 2 * pad, eh, TRUE);
    statusRc_ = {rc.left, rc.bottom - stH, rc.right, rc.bottom};
    MoveWindow(list_.Hwnd(), rc.left + m, searchRc_.bottom + m, (rc.right - rc.left) - 2 * m,
               statusRc_.top - (searchRc_.bottom + m), TRUE);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void MainWindow::Paint() {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd_, &ps);
    auto& t = GetTheme(); auto& p = t.P();
    RECT rc; GetClientRect(hwnd_, &rc);
    FillRect(dc, &rc, t.BgBrush());
    UINT dpi = DpiOf(hwnd_);

    // 搜索框：圆角面板 + 聚焦时底部强调线（M2 换成 Direct2D 抗锯齿）
    HPEN pen = CreatePen(PS_SOLID, 1, p.border);
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, t.SurfaceBrush());
    int r = Scale(8, dpi);
    RoundRect(dc, searchRc_.left, searchRc_.top, searchRc_.right, searchRc_.bottom, r, r);
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(pen);
    if (GetFocus() == edit_) {
        RECT u{searchRc_.left + r / 2, searchRc_.bottom - Scale(2, dpi), searchRc_.right - r / 2, searchRc_.bottom};
        HBRUSH ab = CreateSolidBrush(p.accent); FillRect(dc, &u, ab); DeleteObject(ab);
    }

    // 状态栏（自绘，便于深色模式）
    HBRUSH sb = CreateSolidBrush(p.bg); FillRect(dc, &statusRc_, sb); DeleteObject(sb);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ of = SelectObject(dc, font_);
    RECT tr = statusRc_; tr.left += Scale(12, dpi); tr.right -= Scale(12, dpi);
    // 指示点
    COLORREF dot = indexer_.Busy() ? RGB(0xE8, 0xA3, 0x17) : RGB(0x2F, 0xB3, 0x44);
    int ds = Scale(7, dpi), cy = (tr.top + tr.bottom) / 2;
    HBRUSH db = CreateSolidBrush(dot); HGDIOBJ odb = SelectObject(dc, db);
    HGDIOBJ onp = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, tr.left, cy - ds / 2, tr.left + ds, cy - ds / 2 + ds);
    SelectObject(dc, onp); SelectObject(dc, odb); DeleteObject(db);
    tr.left += ds + Scale(8, dpi);
    std::wstring right = hotkeyFailed_ ? (L"⚠ 热键 " + GetSettings().toggleKey + L" 被占用")
                                       : (GetSettings().toggleKey + L" 显示/隐藏");
    SIZE rsz{}; GetTextExtentPoint32W(dc, right.c_str(), static_cast<int>(right.size()), &rsz);
    RECT lr = tr; lr.right = tr.right - rsz.cx - Scale(16, dpi);
    SetTextColor(dc, p.text2);
    DrawTextW(dc, statusLeft_.c_str(), -1, &lr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (hotkeyFailed_) SetTextColor(dc, RGB(0xC4, 0x2B, 0x1C));
    DrawTextW(dc, right.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
    SelectObject(dc, of);
    EndPaint(hwnd_, &ps);
}

void MainWindow::StartIndex(bool force) {
    auto& s = GetSettings();
    Indexer::Options o;
    o.roots = s.indexRoots.empty() ? Indexer::DefaultRoots() : s.indexRoots;
    o.excludes = s.excludeFolders;
    o.dbPath = Indexer::DefaultDbPath();
    o.compress = s.compressDb;
    o.forceRebuild = force;
    o.monitor = s.ntfsVolumeMonitors;
    o.enableUsnJournal = s.ntfsEnableUsnJournal;
    g_fastAscii = s.fastAsciiSearch;
    noteShown_ = false;
    // TODO(服务): 需要特权的 MFT 读取将来交给服务进程；目前当前进程已提升时直接读取
    indexer_.Start(hwnd_, std::move(o));
    UpdateStatus();
}

void MainWindow::RunSearch() {
    UpdateTitle();
    auto snap = indexer_.Snapshot();
    if (!snap) return;   // 索引未就绪：丢弃本次刷新，就绪后 WM_APP_INDEX_DONE 会重新搜索
    int len = GetWindowTextLengthW(edit_);
    std::wstring q(static_cast<size_t>(len), L'\0');
    if (len) GetWindowTextW(edit_, q.data(), len + 1);
    searcher_->Request(q, std::move(snap));
}

void MainWindow::UpdateTitle() {
    int len = GetWindowTextLengthW(edit_);
    std::wstring q(static_cast<size_t>(len), L'\0');
    if (len) GetWindowTextW(edit_, q.data(), len + 1);
    std::wstring title = q.empty() ? std::wstring(kAppName) : q + L" - 闪寻";
    SetWindowTextW(hwnd_, title.c_str());
}

void MainWindow::UpdateStatus() {
    std::wstring s;
    auto r = list_.Results();
    if (r && r->data) {
        s = WithThousands(r->items.size()) + L" 个对象";
        int sel = list_.SelectedCount();
        if (sel > 0) {
            unsigned long long total = 0;
            for (auto i : list_.Selection()) total += r->data->entries[i].size;
            s += L"  ·  已选 " + WithThousands(static_cast<size_t>(sel)) + L" 个，" + FormatSize(total);
        }
        wchar_t ms[48]; swprintf_s(ms, L"  ·  %.0f ms", lastSearchMs_);
        s += ms;
    }
    std::wstring phase;
    switch (indexer_.Phase()) {
    case IndexPhase::Loading: phase = L"正在加载数据库…"; break;
    case IndexPhase::Creating: phase = L"正在创建数据库… " + WithThousands(indexer_.Progress()); break;
    case IndexPhase::Updating: phase = L"正在后台更新索引… " + WithThousands(indexer_.Progress()); break;
    case IndexPhase::Saving: phase = L"正在保存数据库…"; break;
    default: break;
    }
    if (!phase.empty()) {
        auto cur = indexer_.CurrentFolder();
        if (!cur.empty() && indexer_.Phase() != IndexPhase::Saving) phase += L"  " + cur;
        s = s.empty() ? phase : s + L"  ·  " + phase;
    }
    if (indexer_.Phase() == IndexPhase::Idle) {
        auto note = indexer_.Stats().note;
        if (!note.empty()) {
            s = s.empty() ? L"⚠ " + note : s + L"  ·  ⚠ " + note;
            if (!noteShown_) { noteShown_ = true; tray_.ShowBalloon(L"闪寻：部分 NTFS 卷未能更新", note.c_str()); }
        }
    }
    statusLeft_ = s;
    InvalidateRect(hwnd_, &statusRc_, FALSE);
}

// ---------- 显示 / 隐藏 ----------
void MainWindow::Toggle() {
    if (IsWindowVisible(hwnd_) && !IsIconic(hwnd_) && GetForegroundWindow() == hwnd_) HideToTray();
    else ShowAndFocus();
}

void MainWindow::ShowAndFocus() {
    // 多显示器：移到鼠标所在显示器
    POINT pt; GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST) != mon && !IsZoomed(hwnd_)) {
        MONITORINFO mi{sizeof(mi)}; GetMonitorInfoW(mon, &mi);
        RECT wr; GetWindowRect(hwnd_, &wr);
        int w = wr.right - wr.left, h = wr.bottom - wr.top;
        int mw = mi.rcWork.right - mi.rcWork.left, mh = mi.rcWork.bottom - mi.rcWork.top;
        if (w > mw) w = mw;
        if (h > mh) h = mh;
        SetWindowPos(hwnd_, nullptr, mi.rcWork.left + (mw - w) / 2, mi.rcWork.top + (mh - h) / 2, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
    if (!SetForegroundWindow(hwnd_)) {
        // 双击 Ctrl 触发时可能没有前台权限：临时挂接前台线程输入
        HWND fg = GetForegroundWindow();
        DWORD fgThread = GetWindowThreadProcessId(fg, nullptr), me = GetCurrentThreadId();
        AttachThreadInput(me, fgThread, TRUE);
        SetForegroundWindow(hwnd_);
        BringWindowToTop(hwnd_);
        AttachThreadInput(me, fgThread, FALSE);
    }
    SetFocus(edit_);
    if (GetSettings().selectAllOnShow) SendMessageW(edit_, EM_SETSEL, 0, -1);
}

void MainWindow::HideToTray() { ShowWindow(hwnd_, SW_HIDE); }

// ---------- 结果操作 ----------
void MainWindow::OpenItems(bool openPath) {
    auto r = list_.Results();
    if (!r) return;
    auto sel = list_.Selection();
    if (sel.empty() && !r->items.empty()) sel.push_back(r->items[0]);
    for (auto i : sel) {
        std::wstring full = r->data->FullPath(i);
        if (openPath) {
            if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(full.c_str())) {
                SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
                ILFree(pidl);
            }
        } else {
            ShellExecuteW(hwnd_, nullptr, full.c_str(), nullptr, r->data->ParentPath(i).c_str(), SW_SHOWNORMAL);
        }
    }
}

void MainWindow::CopyPaths(bool nameOnly) {
    auto r = list_.Results();
    if (!r) return;
    std::wstring text;
    for (auto i : list_.Selection()) {
        if (!text.empty()) text += L"\r\n";
        text += nameOnly ? std::wstring(r->data->Name(i)) : r->data->FullPath(i);
    }
    if (text.empty() || !OpenClipboard(hwnd_)) return;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        memcpy(GlobalLock(g), text.c_str(), bytes);
        GlobalUnlock(g);
        SetClipboardData(CF_UNICODETEXT, g);
    }
    CloseClipboard();
}

void MainWindow::ShowProperties() {
    auto r = list_.Results();
    if (!r) return;
    auto sel = list_.Selection();
    if (sel.empty()) return;
    std::wstring full = r->data->FullPath(sel[0]);
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_INVOKEIDLIST;
    sei.hwnd = hwnd_;
    sei.lpVerb = L"properties";
    sei.lpFile = full.c_str();
    ShellExecuteExW(&sei);
}

void MainWindow::ContextMenu(POINT pt) {
    // TODO(M2): 合并系统 IContextMenu（发送到、7-Zip 等），自定义动词
    if (list_.SelectedCount() == 0) return;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_OPEN, L"打开(&O)\tEnter");
    AppendMenuW(m, MF_STRING, IDM_OPEN_PATH, L"打开路径(&P)\tCtrl+Enter");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_COPY_PATH, L"复制完整路径(&C)\tCtrl+Shift+C");
    AppendMenuW(m, MF_STRING, IDM_COPY_NAME, L"复制名称(&N)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_PROPERTIES, L"属性(&R)\tAlt+Enter");
    SetMenuDefaultItem(m, IDM_OPEN, FALSE);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(m);
}

void MainWindow::TrayMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_SHOW, L"显示闪寻(&S)");
    AppendMenuW(m, MF_STRING, IDM_REINDEX, L"重建索引(&R)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"退出(&X)");
    SetMenuDefaultItem(m, IDM_SHOW, FALSE);
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(hwnd_);   // 否则点外部菜单不消失
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(m);
}

void MainWindow::SavePlacement() {
    auto& s = GetSettings();
    WINDOWPLACEMENT wp{sizeof(wp)};
    GetWindowPlacement(hwnd_, &wp);
    UINT dpi = DpiOf(hwnd_);
    s.x = wp.rcNormalPosition.left; s.y = wp.rcNormalPosition.top;
    s.w = MulDiv(wp.rcNormalPosition.right - wp.rcNormalPosition.left, 96, static_cast<int>(dpi));
    s.h = MulDiv(wp.rcNormalPosition.bottom - wp.rcNormalPosition.top, 96, static_cast<int>(dpi));
    s.maximized = wp.showCmd == SW_SHOWMAXIMIZED;
    auto w = list_.ColumnWidths();
    s.colName = w[0]; s.colPath = w[1]; s.colSize = w[2]; s.colDate = w[3];
}

// ---------- 键盘 ----------
bool MainWindow::PreTranslate(MSG& msg) {
    if (msg.message != WM_KEYDOWN && msg.message != WM_SYSKEYDOWN) return false;
    if (msg.hwnd != hwnd_ && GetParent(msg.hwnd) != hwnd_ && GetParent(GetParent(msg.hwnd)) != hwnd_) return false;
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
    WPARAM k = msg.wParam;
    HWND lv = list_.Hwnd();

    if (k == VK_ESCAPE) {
        if (GetWindowTextLengthW(edit_) > 0) { SetWindowTextW(edit_, L""); SetFocus(edit_); }
        else if (GetSettings().escHides) HideToTray();
        return true;
    }
    if ((ctrl && k == 'F') || k == VK_F3 || (alt && k == 'D')) { SetFocus(edit_); SendMessageW(edit_, EM_SETSEL, 0, -1); return true; }
    if (k == VK_F5) { StartIndex(true); return true; }
    if (ctrl && k == 'Q') { DestroyWindow(hwnd_); return true; }
    if (ctrl && k == 'W') { HideToTray(); return true; }

    if (msg.hwnd == edit_) {
        if (ctrl && k == 'A') { SendMessageW(edit_, EM_SETSEL, 0, -1); return true; }
        if (k == VK_DOWN || k == VK_NEXT) {
            if (ListView_GetItemCount(lv) > 0) {
                SetFocus(lv);
                ListView_SetItemState(lv, -1, 0, LVIS_SELECTED);
                ListView_SetItemState(lv, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EnsureVisible(lv, 0, FALSE);
            }
            return true;
        }
        if (k == VK_RETURN) { OpenItems(ctrl); return true; }
    } else if (msg.hwnd == lv) {
        if (k == VK_RETURN && alt) { ShowProperties(); return true; }
        if (k == VK_RETURN) { OpenItems(ctrl); return true; }
        if (ctrl && shift && k == 'C') { CopyPaths(false); return true; }
        if (ctrl && k == 'A') { ListView_SetItemState(lv, -1, LVIS_SELECTED, LVIS_SELECTED); return true; }
        if (k == VK_UP && ListView_GetNextItem(lv, -1, LVNI_FOCUSED) == 0 && !shift) { SetFocus(edit_); return true; }
        if (k == VK_APPS || (shift && k == VK_F10)) {
            int f = list_.FocusedIndex(); RECT r{};
            if (f >= 0) ListView_GetItemRect(lv, f, &r, LVIR_LABEL);
            POINT pt{r.left, r.bottom}; ClientToScreen(lv, &pt);
            ContextMenu(pt);
            return true;
        }
    }
    return false;
}

// ---------- 消息 ----------
LRESULT MainWindow::Handle(UINT m, WPARAM w, LPARAM l) {
    if (m == taskbarCreated_ && taskbarCreated_) { tray_.Add(hwnd_, iconSmall_); return 0; }
    switch (m) {
    case WM_CREATE: OnCreate(); return 0;
    case WM_SIZE: Layout(); return 0;
    case WM_PAINT: Paint(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(l);
        UINT dpi = DpiOf(hwnd_);
        mm->ptMinTrackSize = {Scale(360, dpi), Scale(240, dpi)};
        return 0;
    }
    case WM_DPICHANGED: {
        auto* r = reinterpret_cast<RECT*>(l);
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            UpdateFonts();
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (l && wcscmp(reinterpret_cast<const wchar_t*>(l), L"ImmersiveColorSet") == 0 && GetSettings().theme == ThemeMode::System)
            ApplyTheme();
        return 0;
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(w);
        SetTextColor(dc, GetTheme().P().text);
        SetBkColor(dc, GetTheme().P().surface);
        return reinterpret_cast<LRESULT>(GetTheme().SurfaceBrush());
    }
    case WM_ACTIVATE:
        if (LOWORD(w) != WA_INACTIVE) SetFocus(edit_);
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == IDC_SEARCH) {
            if (HIWORD(w) == EN_CHANGE) RunSearch();
            if (HIWORD(w) == EN_SETFOCUS || HIWORD(w) == EN_KILLFOCUS) InvalidateRect(hwnd_, &searchRc_, FALSE);
            return 0;
        }
        switch (id) {
        case IDM_EXIT: DestroyWindow(hwnd_); break;
        case IDM_REINDEX: StartIndex(true); break;
        case IDM_THEME_SYSTEM: case IDM_THEME_LIGHT: case IDM_THEME_DARK:
            GetSettings().theme = static_cast<ThemeMode>(id - IDM_THEME_SYSTEM); ApplyTheme(); break;
        case IDM_DOUBLE_CTRL:
            GetSettings().doubleCtrl = !GetSettings().doubleCtrl;
            if (GetSettings().doubleCtrl) hotkeys_.EnableDoubleCtrl(hwnd_); else hotkeys_.DisableDoubleCtrl();
            ApplyTheme(); break;
        case IDM_SHOW: ShowAndFocus(); break;
        case IDM_OPEN: OpenItems(false); break;
        case IDM_OPEN_PATH: OpenItems(true); break;
        case IDM_COPY_PATH: CopyPaths(false); break;
        case IDM_COPY_NAME: CopyPaths(true); break;
        case IDM_PROPERTIES: ShowProperties(); break;
        case IDM_ABOUT: {
            std::wstring t = std::wstring(L"闪寻 FlashFind ") + FLASHFIND_VERSION +
                L"\n\n极速文件名搜索（原生 Win32，无第三方依赖）。\n\n"
                L"全局热键：" + GetSettings().toggleKey + L" 或双击 Ctrl 显示/隐藏。\n配置文件：" + GetSettings().IniPath();
            MessageBoxW(hwnd_, t.c_str(), L"关于闪寻", MB_OK | MB_ICONINFORMATION);
            break;
        }
        }
        return 0;
    }
    case WM_NOTIFY: {
        auto* nm = reinterpret_cast<NMHDR*>(l);
        LRESULT out = 0;
        if (nm->hwndFrom == list_.Hwnd()) {
            if (nm->code == NM_DBLCLK) { OpenItems(false); return 0; }
            if (nm->code == NM_RCLICK) { POINT pt; GetCursorPos(&pt); ContextMenu(pt); return 0; }
            if (nm->code == LVN_ITEMCHANGED || nm->code == LVN_ODSTATECHANGED) UpdateStatus();
        }
        if (list_.OnNotify(nm, &out)) return out;
        return 0;
    }
    case WM_HOTKEY:
        if (w == HotkeyManager::kToggleId) Toggle();
        return 0;
    case WM_APP_TOGGLE: Toggle(); return 0;
    case WM_APP_SHOW: ShowAndFocus(); return 0;
    case WM_APP_TRAY:
        switch (LOWORD(l)) {
        case WM_LBUTTONUP: case NIN_SELECT: case NIN_KEYSELECT: Toggle(); break;
        case WM_CONTEXTMENU: case WM_RBUTTONUP: TrayMenu(); break;
        }
        return 0;
    case WM_APP_RESULTS: {
        auto r = searcher_->Latest();
        if (r && r->generation == static_cast<uint64_t>(w)) {
            lastSearchMs_ = r->elapsedMs;
            list_.SetResults(r);
            UpdateStatus();
        }
        return 0;
    }
    case WM_APP_INDEX_PROGRESS: UpdateStatus(); return 0;
    case WM_APP_INDEX_DONE: RunSearch(); UpdateStatus(); return 0;
    case WM_CLOSE:
        if (GetSettings().closeToTray) { HideToTray(); return 0; }
        DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY:
        SavePlacement();
        tray_.Remove();
        hotkeys_.UnregisterToggle();
        hotkeys_.DisableDoubleCtrl();
        indexer_.Stop();
        searcher_.reset();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd_, m, w, l);
}

} // namespace ff
