#include "Fold.h"
#include "MainWindow.h"
#include "NtfsSource.h"
#include "Options.h"
#include "FileOps.h"
#include "History.h"
#include "ShellInstall.h"
#include <shellapi.h>
#include <algorithm>
#include "Dpi.h"
#include "Settings.h"
#include "Theme.h"
#include "Util.h"
#include <commctrl.h>
#include <uxtheme.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

namespace ff {

enum : int {
    IDC_SEARCH = 100, IDC_LIST, IDC_FILTER,
    IDM_EXIT = 1000, IDM_REINDEX, IDM_REFRESH, IDM_THEME_SYSTEM, IDM_THEME_LIGHT, IDM_THEME_DARK,
    IDM_ABOUT, IDM_SHOW, IDM_OPEN, IDM_OPEN_PATH, IDM_COPY_PATH, IDM_COPY_NAME, IDM_PROPERTIES,
    IDM_DOUBLE_CTRL,
    IDM_INSTALL_SVC,
    IDM_UNINSTALL_SVC,
    IDC_SVC_BTN,
    IDM_OPTIONS,
    IDM_REGEX, IDM_CASE, IDM_WHOLEWORD, IDM_MATCHPATH,
    IDM_RENAME, IDM_RENAME_COMMIT, IDM_DELETE, IDM_DELETE_PERM, IDM_CUT, IDM_COPY_FILES, IDM_PASTE,
    IDM_COPY_TO, IDM_MOVE_TO, IDM_CLEAR_HISTORY, IDM_HISTORY_ENABLE,
    IDM_COL_PATH, IDM_COL_EXT, IDM_COL_SIZE, IDM_COL_DATE,
    IDM_INSTALL_URL, IDM_UNINSTALL_URL, IDM_INSTALL_FOLDER_CM, IDM_UNINSTALL_FOLDER_CM,
};

// 过滤器（与 Everything 默认过滤器相同的扩展名表）
struct FilterDef { const wchar_t* name; const wchar_t* expr; };
static const FilterDef kFilters[] = {
    {L"全部", L""},
    {L"音频", L"ext:aac;ac3;aif;aifc;aiff;au;cda;dts;fla;flac;it;m1a;m2a;m3u;m4a;mid;midi;mka;mod;mp2;mp3;mpa;ogg;ra;rmi;spc;snd;umx;voc;wav;wma;xm"},
    {L"压缩文件", L"ext:7z;ace;arj;bz2;cab;gz;gzip;jar;r00;r01;r02;r03;r04;r05;r06;r07;r08;r09;r10;r11;r12;r13;r14;r15;r16;r17;r18;r19;r20;r21;r22;r23;r24;r25;r26;r27;r28;r29;rar;tar;tgz;xz;z;zip;zst"},
    {L"文档", L"ext:c;chm;cpp;csv;cxx;doc;docm;docx;dot;dotm;dotx;h;hpp;htm;html;hxx;ini;java;lua;md;mht;mhtml;odt;pdf;potx;potm;ppam;ppsm;ppsx;pps;ppt;pptm;pptx;rtf;sldm;sldx;thmx;txt;vsd;wpd;wps;wri;xlam;xls;xlsb;xlsm;xlsx;xltm;xltx;xml"},
    {L"可执行文件", L"ext:bat;cmd;exe;msi;msp;scr"},
    {L"文件夹", L"folder:"},
    {L"图片", L"ext:ani;bmp;gif;heic;ico;jpe;jpeg;jpg;pcx;png;psd;svg;tga;tif;tiff;webp;wmf"},
    {L"视频", L"ext:3g2;3gp;3gp2;3gpp;amr;amv;asf;avi;bdmv;bik;d2v;divx;drc;dsa;dsm;dss;dsv;evo;f4v;flc;fli;flic;flv;hdmov;ifo;ivf;m1v;m2p;m2t;m2ts;m2v;m4b;m4p;m4v;mkv;mp2v;mp4;mp4v;mpe;mpeg;mpg;mpls;mpv2;mpv4;mov;mts;ogm;ogv;pss;pva;qt;ram;ratdvd;rm;rmm;rmvb;roq;rpm;smil;smk;swf;tp;tpr;ts;vob;vp6;webm;wm;wmp;wmv"},
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
    filter_ = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                              0, 0, 10, 300, hwnd_, reinterpret_cast<HMENU>(IDC_FILTER), App::Instance(), nullptr);
    for (auto& f : kFilters) SendMessageW(filter_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(f.name));
    SendMessageW(filter_, CB_SETCURSEL, std::clamp(GetSettings().filter, 0, static_cast<int>(std::size(kFilters)) - 1), 0);
    svcBtn_ = CreateWindowExW(0, WC_BUTTONW, L"安装 FlashFind 服务", WS_CHILD | BS_PUSHBUTTON,
                              0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(IDC_SVC_BTN), App::Instance(), nullptr);
    SendMessageW(svcBtn_, BCM_SETSHIELD, 0, TRUE);
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

// UAC 盾牌：SHGetStockIconInfo(SIID_SHIELD) 小图标 -> 32bpp 预乘 alpha 位图（菜单用）
static HBITMAP ShieldBitmap(UINT dpi) {
    SHSTOCKICONINFO sii{sizeof(sii)};
    if (FAILED(SHGetStockIconInfo(SIID_SHIELD, SHGSI_ICONLOCATION, &sii))) return nullptr;
    int cx = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
    HICON ic = nullptr;
    if (FAILED(LoadIconWithScaleDown(nullptr, IDI_SHIELD, cx, cx, &ic)) || !ic) {
        if (FAILED(SHGetStockIconInfo(SIID_SHIELD, SHGSI_ICON | SHGSI_SMALLICON, &sii))) return nullptr;
        ic = sii.hIcon;
    }
    BITMAPINFO bi{}; bi.bmiHeader = {sizeof(BITMAPINFOHEADER), cx, -cx, 1, 32, BI_RGB};
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp) {
        HGDIOBJ o = SelectObject(dc, bmp);
        DrawIconEx(dc, 0, 0, ic, cx, cx, 0, nullptr, DI_NORMAL);
        SelectObject(dc, o);
    }
    DeleteDC(dc); DestroyIcon(ic);
    return bmp;
}

// 安装/卸载服务：以 runas 重新启动自身（UAC），等待，显示结果；成功后重新连接管道继续索引（无需重启）
void MainWindow::ServiceAction(bool install) {
    DWORD r = RunElevated(hwnd_, install ? L"-install-service" : L"-uninstall-service");
    std::wstring msg;
    if (r == 0) {
        msg = install ? L"FlashFind 服务已安装并启动。闪寻将通过服务读取 NTFS 主文件表与 USN 日志，正在继续索引。"
                      : L"FlashFind 服务已卸载。";
        MessageBoxW(hwnd_, msg.c_str(), kAppName, MB_ICONINFORMATION);
        noteShown_ = false;
        StartIndex(false);   // 未提升的界面连接服务管道，继续索引
        return;
    }
    if (r == ERROR_CANCELLED) msg = L"已取消：用户拒绝了 UAC 提权请求（ERROR_CANCELLED）。";
    else {
        wchar_t* t = nullptr;
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, r, 0, reinterpret_cast<LPWSTR>(&t), 0, nullptr);
        wchar_t b[96]; swprintf_s(b, L"失败，错误码 %lu", r);
        msg = std::wstring(install ? L"安装" : L"卸载") + L" FlashFind 服务" + b + (t ? L"：" + std::wstring(t) : L"");
        if (t) LocalFree(t);
        msg += L"\n详情见 %ProgramData%\\FlashFind\\FlashFind-service.log";
    }
    MessageBoxW(hwnd_, msg.c_str(), kAppName, MB_ICONWARNING);
}

void MainWindow::BuildMenu() {
    HMENU bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDM_REFRESH, L"刷新(&F)\tF5");
    AppendMenuW(file, MF_STRING, IDM_REINDEX, L"重建索引(&R)\tCtrl+F5");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_EXIT, L"退出(&X)\tCtrl+Q");
    HMENU view = CreatePopupMenu();
    HMENU theme = CreatePopupMenu();
    AppendMenuW(theme, MF_STRING, IDM_THEME_SYSTEM, L"跟随系统");
    AppendMenuW(theme, MF_STRING, IDM_THEME_LIGHT, L"浅色");
    AppendMenuW(theme, MF_STRING, IDM_THEME_DARK, L"深色");
    HMENU cols = CreatePopupMenu();
    AppendMenuW(cols, MF_STRING, IDM_COL_PATH, L"路径(&P)");
    AppendMenuW(cols, MF_STRING, IDM_COL_EXT, L"扩展名(&E)");
    AppendMenuW(cols, MF_STRING, IDM_COL_SIZE, L"大小(&S)");
    AppendMenuW(cols, MF_STRING, IDM_COL_DATE, L"修改时间(&M)");
    AppendMenuW(view, MF_POPUP, reinterpret_cast<UINT_PTR>(cols), L"列(&C)");
    AppendMenuW(view, MF_POPUP, reinterpret_cast<UINT_PTR>(theme), L"主题(&T)");
    HMENU search = CreatePopupMenu();
    AppendMenuW(search, MF_STRING, IDM_CASE, L"区分大小写(&C)\tCtrl+I");
    AppendMenuW(search, MF_STRING, IDM_WHOLEWORD, L"全字匹配(&W)\tCtrl+B");
    AppendMenuW(search, MF_STRING, IDM_MATCHPATH, L"匹配路径(&P)\tCtrl+U");
    AppendMenuW(search, MF_STRING, IDM_REGEX, L"正则表达式(&R)\tCtrl+R");
    AppendMenuW(search, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(search, MF_STRING, IDM_HISTORY_ENABLE, L"启用搜索历史");
    AppendMenuW(search, MF_STRING, IDM_CLEAR_HISTORY, L"清空搜索历史");
    HMENU tools = CreatePopupMenu();
    AppendMenuW(tools, MF_STRING, IDM_DOUBLE_CTRL, L"双击 Ctrl 显示/隐藏");
    AppendMenuW(tools, MF_STRING, IDM_OPTIONS, L"选项(&O)…\tCtrl+P");
    AppendMenuW(tools, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(tools, MF_STRING, IDM_INSTALL_URL, L"安装 es: 协议");
    AppendMenuW(tools, MF_STRING, IDM_UNINSTALL_URL, L"卸载 es: 协议");
    AppendMenuW(tools, MF_STRING, IDM_INSTALL_FOLDER_CM, L"安装文件夹右键菜单");
    AppendMenuW(tools, MF_STRING, IDM_UNINSTALL_FOLDER_CM, L"卸载文件夹右键菜单");
    AppendMenuW(tools, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(tools, MF_STRING, IDM_INSTALL_SVC, L"安装 FlashFind 服务(&I)…");
    AppendMenuW(tools, MF_STRING, IDM_UNINSTALL_SVC, L"卸载 FlashFind 服务(&U)…");
    if (!shieldBmp_) shieldBmp_ = ShieldBitmap(DpiOf(hwnd_));
    if (shieldBmp_) {
        MENUITEMINFOW mi{sizeof(mi)}; mi.fMask = MIIM_BITMAP; mi.hbmpItem = shieldBmp_;
        SetMenuItemInfoW(tools, IDM_INSTALL_SVC, FALSE, &mi);
        SetMenuItemInfoW(tools, IDM_UNINSTALL_SVC, FALSE, &mi);
    }
    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, IDM_ABOUT, L"关于闪寻(&A)…");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"文件(&F)");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"视图(&V)");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(search), L"搜索(&S)");
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
    if (filter_) SetWindowTheme(filter_, t.Dark() ? L"DarkMode_CFD" : L"CFD", nullptr);
    HMENU bar = GetMenu(hwnd_);
    int sel = IDM_THEME_SYSTEM + static_cast<int>(GetSettings().theme);
    CheckMenuRadioItem(bar, IDM_THEME_SYSTEM, IDM_THEME_DARK, sel, MF_BYCOMMAND);
    CheckMenuItem(bar, IDM_COL_PATH, MF_BYCOMMAND | (list_.ColumnVisible(ColPath) ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(bar, IDM_COL_EXT, MF_BYCOMMAND | (list_.ColumnVisible(ColExt) ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(bar, IDM_COL_SIZE, MF_BYCOMMAND | (list_.ColumnVisible(ColSize) ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(bar, IDM_COL_DATE, MF_BYCOMMAND | (list_.ColumnVisible(ColDate) ? MF_CHECKED : MF_UNCHECKED));
    auto& st = GetSettings();
    CheckMenuItem(bar, IDM_CASE, MF_BYCOMMAND | (st.matchCase ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(bar, IDM_WHOLEWORD, MF_BYCOMMAND | (st.matchWholeWord ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(bar, IDM_MATCHPATH, MF_BYCOMMAND | (st.matchPath ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(bar, IDM_REGEX, MF_BYCOMMAND | (st.matchRegex ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(bar, IDM_HISTORY_ENABLE, MF_BYCOMMAND | (st.historyEnabled ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(bar, IDM_INSTALL_URL, MF_BYCOMMAND | (UrlProtocolInstalled() ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(bar, IDM_INSTALL_FOLDER_CM, MF_BYCOMMAND | (FolderContextMenuInstalled() ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(bar, IDM_DOUBLE_CTRL, MF_BYCOMMAND | (GetSettings().doubleCtrl ? MF_CHECKED : MF_UNCHECKED));
    DrawMenuBar(hwnd_);
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);   // 重画非客户区（菜单栏下方的线）
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
    if (filter_) {   // 过滤器下拉框在搜索框右侧
        SendMessageW(filter_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
        int fw = Scale(120, dpi);
        searchRc_.right -= fw + m;
        RECT cr{}; GetWindowRect(filter_, &cr);
        int ch = cr.bottom - cr.top;
        MoveWindow(filter_, searchRc_.right + m, searchRc_.top + (sh - ch) / 2, fw, Scale(320, dpi), TRUE);
    }
    HDC dc = GetDC(hwnd_);
    HGDIOBJ o = SelectObject(dc, font_);
    TEXTMETRICW tm; GetTextMetricsW(dc, &tm);
    SelectObject(dc, o); ReleaseDC(hwnd_, dc);
    int pad = Scale(10, dpi);
    int eh = tm.tmHeight;
    MoveWindow(edit_, searchRc_.left + pad, searchRc_.top + (sh - eh) / 2, (searchRc_.right - searchRc_.left) - 2 * pad, eh, TRUE);
    statusRc_ = {rc.left, rc.bottom - stH, rc.right, rc.bottom};
    if (svcBtn_) {
        SendMessageW(svcBtn_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
        SIZE bs{}; SendMessageW(svcBtn_, BCM_GETIDEALSIZE, 0, reinterpret_cast<LPARAM>(&bs));
        int bw = std::max<int>(bs.cx, Scale(150, dpi)), bh = stH - Scale(4, dpi);
        svcBtnW_ = bw + Scale(8, dpi);
        MoveWindow(svcBtn_, statusRc_.right - Scale(4, dpi) - bw, statusRc_.top + Scale(2, dpi), bw, bh, TRUE);
    }
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
    if (svcBtn_ && IsWindowVisible(svcBtn_)) tr.right -= svcBtnW_;
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
    o.roots.clear();
    for (auto& r : s.indexRoots.empty() ? Indexer::DefaultRoots() : s.indexRoots) {
        bool skip = false;
        for (auto& v : s.ntfsVolumes) if (_wcsicmp(v.path.c_str(), r.c_str()) == 0 && !v.include) skip = true;
        if (!skip) o.roots.push_back(r);
    }
    o.volumes = s.ntfsVolumes;
    o.monitorDelayMs = s.monitorUpdateDelay;
    o.excludes = s.excludeFolders;
    o.dbPath = Indexer::DefaultDbPath();
    o.compress = s.compressDb;
    o.forceRebuild = force;
    g_fastAscii = s.fastAsciiSearch;
    noteShown_ = false;
    // TODO(服务): 需要特权的 MFT 读取将来交给服务进程；目前当前进程已提升时直接读取
    indexer_.Start(hwnd_, std::move(o));
    UpdateStatus();
}


void MainWindow::Refresh() {
    // F5 软刷新（非全量重建）：
    // 1) 对当前结果集从磁盘读 size/mtime 覆盖显示（立刻可见）
    // 2) 用现有快照重搜（监控线程可能已写入 USN 变更）
    // 3) StartIndex(false) 再推一把增量；完成后 RESULTS 里再刷一遍磁盘属性
    // 全量重建：Ctrl+F5 / 菜单「重建索引」→ StartIndex(true)
    softRefreshPending_ = true;
    list_.RefreshAttrsFromDisk();
    InvalidateRect(list_.Hwnd(), nullptr, FALSE);
    UpdateStatus();
    RunSearch();
    StartIndex(false);
}

void MainWindow::RunSearch() {
    if (hwnd_) KillTimer(hwnd_, kSearchTimerId);
    UpdateTitle();
    auto snap = indexer_.Snapshot();
    if (!snap) return;   // 索引未就绪：丢弃本次刷新，就绪后 WM_APP_INDEX_DONE 会重新搜索
    int len = GetWindowTextLengthW(edit_);
    std::wstring q(static_cast<size_t>(len), L'\0');
    if (len) GetWindowTextW(edit_, q.data(), len + 1);
    int f = filter_ ? static_cast<int>(SendMessageW(filter_, CB_GETCURSEL, 0, 0)) : 0;
    std::wstring fx = f > 0 && f < static_cast<int>(std::size(kFilters)) ? kFilters[f].expr : L"";
    auto& st = GetSettings();
    MatchOpts o; o.matchCase = st.matchCase; o.matchWholeWord = st.matchWholeWord; o.matchPath = st.matchPath; o.matchRegex = st.matchRegex;
    searcher_->Request(q, std::move(snap), list_.Sort(), o, fx);
}

void MainWindow::ScheduleSearch() {
    // 标题即时刷新；搜索请求 50ms 防抖，连打字时少触发编译/扫描（Searcher 虽有 gen_ 取消，仍省调度与 CPU）
    UpdateTitle();
    if (!hwnd_) return;
    SetTimer(hwnd_, kSearchTimerId, kSearchDebounceMs, nullptr);
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
            // 全选大量结果时逐条累加体积会卡住 UI；超过阈值只显示个数
            constexpr int kMaxSizeSum = 2000;
            if (sel <= kMaxSizeSum) {
                unsigned long long total = 0;
                for (auto i : list_.Selection()) total += r->data->entries[i].size;
                s += L"  ·  已选 " + WithThousands(static_cast<size_t>(sel)) + L" 个，" + FormatSize(total);
            } else {
                s += L"  ·  已选 " + WithThousands(static_cast<size_t>(sel)) + L" 个";
            }
        }
        wchar_t ms[48]; swprintf_s(ms, L"  ·  %.0f ms", lastSearchMs_);
        s += ms;
        auto& st = GetSettings();
        if (st.matchCase) s += L"  ·  区分大小写";
        if (st.matchWholeWord) s += L"  ·  全字";
        if (st.matchPath) s += L"  ·  路径";
        if (st.matchRegex) s += L"  ·  正则";
        if (!r->error.empty()) s = L"⚠ " + r->error;
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
            if (!noteShown_) {
                noteShown_ = true;
                std::wstring b = note;
                if (indexer_.Stats().needService) b += L"\n点击此通知安装 FlashFind 服务（需要管理员确认）。";
                tray_.ShowBalloon(L"闪寻：部分 NTFS 卷未能更新", b.c_str());
            }
        }
    }
    statusLeft_ = s;
    {
        bool need = indexer_.Phase() == IndexPhase::Idle && indexer_.Stats().needService;
        if (svcBtn_ && (IsWindowVisible(svcBtn_) != FALSE) != need) ShowWindow(svcBtn_, need ? SW_SHOWNA : SW_HIDE);
    }
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

bool MainWindow::ShowImagePreview() {
    auto paths = SelectedPaths();
    if (paths.empty()) return false;
    std::wstring path = paths[0];
    int fi = list_.FocusedIndex();
    auto r = list_.Results();
    if (fi >= 0 && r && r->data && static_cast<size_t>(fi) < r->items.size()) {
        uint32_t idx = r->items[static_cast<size_t>(fi)];
        auto sel = list_.Selection();
        if (std::find(sel.begin(), sel.end(), idx) != sel.end())
            path = r->data->FullPath(idx);
    }
    // 锚点：文件名列右侧一点点（NameTextRect 右缘），垂直贴行顶
    POINT anchor{};
    HWND lv = list_.Hwnd();
    int row = list_.FocusedIndex();
    if (row < 0) row = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
    if (row >= 0) {
        RECT nr = list_.NameTextRect(row);
        POINT pt{nr.right, nr.top};
        ClientToScreen(lv, &pt);
        anchor = pt;
    } else {
        GetCursorPos(&anchor);
    }
    return imagePreview_.Show(hwnd_, path, anchor);
}

void MainWindow::HideImagePreview() {
    imagePreview_.Hide();
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
    if (list_.SelectedCount() == 0) return;
    // 系统右键菜单（IContextMenu）：同一文件夹内的选择；顶部插入闪寻自己的命令
    constexpr UINT kFirst = 0x8000, kLast = 0xEFFF;
    auto sel = list_.Selection();
    const auto* res = list_.Results();
    IContextMenu* cm = nullptr;
    if (res && res->data && sel.size() <= 4096 && GetSettings().singleParentContextMenu) {
        const auto& d = *res->data;
        uint32_t parent = d.entries[sel[0]].parent;
        bool same = std::all_of(sel.begin(), sel.end(), [&](uint32_t i) { return d.entries[i].parent == parent; });
        if (same) {
            std::vector<PIDLIST_ABSOLUTE> abs;
            for (uint32_t i : sel) {
                PIDLIST_ABSOLUTE p = nullptr;
                if (SUCCEEDED(SHParseDisplayName(d.FullPath(i).c_str(), nullptr, &p, 0, nullptr)) && p) abs.push_back(p);
            }
            if (abs.size() == sel.size()) {
                IShellFolder* folder = nullptr; PCUITEMID_CHILD child = nullptr;
                if (SUCCEEDED(SHBindToParent(abs[0], IID_PPV_ARGS(&folder), &child))) {
                    std::vector<PCUITEMID_CHILD> kids;
                    for (auto p : abs) kids.push_back(ILFindLastID(p));
                    folder->GetUIObjectOf(hwnd_, static_cast<UINT>(kids.size()), kids.data(), IID_IContextMenu, nullptr, reinterpret_cast<void**>(&cm));
                    folder->Release();
                }
            }
            for (auto p : abs) CoTaskMemFree(p);
        }
    }
    HMENU m = CreatePopupMenu();
    if (!cm) {
        AppendMenuW(m, MF_STRING, IDM_OPEN, L"打开(&O)\tEnter");
    }
    AppendMenuW(m, MF_STRING, IDM_OPEN_PATH, L"打开路径(&P)\tCtrl+Enter");
    AppendMenuW(m, MF_STRING, IDM_COPY_PATH, L"复制完整路径(&C)\tCtrl+Shift+C");
    AppendMenuW(m, MF_STRING, IDM_COPY_NAME, L"复制名称(&N)");
    if (GetSettings().allowRename) AppendMenuW(m, MF_STRING, IDM_RENAME, sel.size() > 1 ? L"重命名多个(&M)…\tF2" : L"重命名(&M)\tF2");
    AppendMenuW(m, MF_STRING, IDM_CUT, L"剪切(&T)\tCtrl+X");
    AppendMenuW(m, MF_STRING, IDM_COPY_FILES, L"复制(&Y)\tCtrl+C");
    AppendMenuW(m, MF_STRING, IDM_COPY_TO, L"复制到文件夹(&F)…");
    AppendMenuW(m, MF_STRING, IDM_MOVE_TO, L"移动到文件夹(&M)…");
    if (!cm) AppendMenuW(m, MF_STRING, IDM_DELETE, L"删除(&D)\tDel");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    if (cm) {
        UINT flags = CMF_NORMAL | ((GetKeyState(VK_SHIFT) < 0) ? CMF_EXTENDEDVERBS : 0);
        if (FAILED(cm->QueryContextMenu(m, GetMenuItemCount(m), kFirst, kLast, flags))) { cm->Release(); cm = nullptr; }
    }
    if (!cm) {
        AppendMenuW(m, MF_STRING, IDM_PROPERTIES, L"属性(&R)\tAlt+Enter");
        SetMenuDefaultItem(m, IDM_OPEN, FALSE);
    }
    if (cm) { cm->QueryInterface(IID_PPV_ARGS(&cm3_)); if (!cm3_) cm->QueryInterface(IID_PPV_ARGS(&cm2_)); }
    UINT cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, 0, hwnd_, nullptr);
    if (cm3_) { cm3_->Release(); cm3_ = nullptr; }
    if (cm2_) { cm2_->Release(); cm2_ = nullptr; }
    if (cmd >= kFirst && cmd <= kLast && cm) {
        char verb[64] = {};
        bool ren = SUCCEEDED(cm->GetCommandString(cmd - kFirst, GCS_VERBA, nullptr, verb, sizeof(verb))) && _stricmp(verb, "rename") == 0;
        if (ren) PostMessageW(hwnd_, WM_COMMAND, IDM_RENAME, 0);   // 系统“重命名”改用我们的就地编辑
        else {
            CMINVOKECOMMANDINFOEX ci{sizeof(ci)};
            ci.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE | (GetKeyState(VK_CONTROL) < 0 ? CMIC_MASK_CONTROL_DOWN : 0) | (GetKeyState(VK_SHIFT) < 0 ? CMIC_MASK_SHIFT_DOWN : 0);
            ci.hwnd = hwnd_;
            ci.lpVerb = MAKEINTRESOURCEA(cmd - kFirst);
            ci.lpVerbW = MAKEINTRESOURCEW(cmd - kFirst);
            ci.nShow = SW_SHOWNORMAL;
            ci.ptInvoke = pt;
            cm->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&ci));
        }
    } else if (cmd) {
        PostMessageW(hwnd_, WM_COMMAND, cmd, 0);
    }
    if (cm) cm->Release();
    DestroyMenu(m);
}

void MainWindow::TrayMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_SHOW, L"显示闪寻(&S)");
    AppendMenuW(m, MF_STRING, IDM_REFRESH, L"刷新(&F)");
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
    if (w.size() >= 5) { s.colName = w[0]; s.colPath = w[1]; s.colExt = w[2]; s.colSize = w[3]; s.colDate = w[4]; }
}

// ---------- 文件操作 ----------
std::vector<std::wstring> MainWindow::SelectedPaths() const {
    std::vector<std::wstring> v;
    auto r = list_.Results();
    if (!r || !r->data) return v;
    for (auto i : list_.Selection()) v.push_back(r->data->FullPath(i));
    return v;
}

void MainWindow::Rename() {
    if (!GetSettings().allowRename || renameEdit_) return;
    auto r = list_.Results();
    if (!r || !r->data) return;
    auto sel = list_.Selection();
    if (sel.empty()) return;
    if (sel.size() == 1) { BeginInlineRename(); return; }
    std::vector<std::wstring> paths, names;
    for (auto i : sel) { paths.push_back(r->data->FullPath(i)); names.emplace_back(r->data->Name(i)); }
    std::vector<std::wstring> newNames = names;
    if (!ShowRenameDialog(hwnd_, paths, newNames)) return;
    std::vector<RenameReq> reqs;
    for (size_t k = 0; k < paths.size(); ++k) if (newNames[k] != names[k]) reqs.push_back({paths[k], newNames[k]});
    if (reqs.empty()) return;
    std::wstring err;
    size_t n = RenameItems(hwnd_, reqs, err);
    if (!err.empty() && err != L"操作被取消") MessageBoxW(hwnd_, (L"部分项目未能重命名：\n" + err).c_str(), kAppName, MB_ICONWARNING);
    (void)n;
}

void MainWindow::BeginInlineRename() {
    HWND lv = list_.Hwnd();
    int row = list_.FocusedIndex();
    if (row < 0 || ListView_GetItemState(lv, row, LVIS_SELECTED) == 0) row = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
    auto r = list_.Results();
    if (row < 0 || !r || !r->data) return;
    ListView_EnsureVisible(lv, row, FALSE);
    uint32_t id = r->items[static_cast<size_t>(row)];
    renamePath_ = r->data->FullPath(id);
    renameOrig_ = std::wstring(r->data->Name(id));
    RECT rc = list_.NameTextRect(row);
    renameEdit_ = CreateWindowExW(0, WC_EDITW, renameOrig_.c_str(), WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                  rc.left, rc.top + 1, std::max<int>(rc.right - rc.left, Scale(120, DpiOf(hwnd_))), rc.bottom - rc.top - 2,
                                  lv, nullptr, App::Instance(), nullptr);
    SendMessageW(renameEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
    SendMessageW(renameEdit_, EM_LIMITTEXT, 255, 0);
    SetWindowSubclass(renameEdit_, RenameEditProc, 1, reinterpret_cast<DWORD_PTR>(this));
    // 与资源管理器一致：文件只选中主名（不含扩展名）
    size_t dot = renameOrig_.rfind(L'.');
    bool dir = r->data->entries[id].IsDir();
    SendMessageW(renameEdit_, EM_SETSEL, 0, (!dir && dot != std::wstring::npos && dot > 0) ? static_cast<LPARAM>(dot) : -1);
    SetFocus(renameEdit_);
}

LRESULT CALLBACK MainWindow::RenameEditProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) {
    auto* self = reinterpret_cast<MainWindow*>(ref);
    switch (m) {
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS;
    case WM_KEYDOWN:
        if (w == VK_RETURN) { self->EndInlineRename(true); return 0; }
        if (w == VK_ESCAPE) { self->EndInlineRename(false); return 0; }
        break;
    case WM_CHAR: if (w == L'\r' || w == 27) return 0; break;
    case WM_KILLFOCUS: if (!self->renameEnding_) PostMessageW(self->hwnd_, WM_COMMAND, IDM_RENAME_COMMIT, 0); break;   // 点到别处 = 提交
    case WM_NCDESTROY: RemoveWindowSubclass(h, RenameEditProc, 1); break;
    }
    return DefSubclassProc(h, m, w, l);
}

void MainWindow::EndInlineRename(bool commit) {
    if (!renameEdit_ || renameEnding_) return;
    renameEnding_ = true;
    int n = GetWindowTextLengthW(renameEdit_);
    std::wstring nn(static_cast<size_t>(n), L'\0'); if (n) GetWindowTextW(renameEdit_, nn.data(), n + 1);
    HWND e = renameEdit_;
    bool hadFocus = GetFocus() == e;
    renameEdit_ = nullptr;
    DestroyWindow(e);
    if (hadFocus) SetFocus(list_.Hwnd());
    renameEnding_ = false;
    if (!commit || nn == renameOrig_) return;
    if (!ValidFileName(nn)) { MessageBoxW(hwnd_, L"文件名不能为空，也不能包含下列字符：\n\\ / : * ? \" < > |", kAppName, MB_ICONWARNING); return; }
    std::wstring err;
    if (!RenameItems(hwnd_, {{renamePath_, nn}}, err) && !err.empty() && err != L"操作被取消")
        MessageBoxW(hwnd_, (L"重命名失败：\n" + err).c_str(), kAppName, MB_ICONWARNING);
}

void MainWindow::Delete(bool permanent) {
    auto paths = SelectedPaths();
    if (paths.empty()) return;
    if (permanent) {
        std::wstring q = paths.size() == 1 ? L"确实要永久删除“" + paths[0] + L"”吗？\n\n此操作无法撤销。"
                                           : L"确实要永久删除这 " + std::to_wstring(paths.size()) + L" 项吗？\n\n此操作无法撤销。";
        if (MessageBoxW(hwnd_, q.c_str(), L"永久删除", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    }
    std::wstring err;
    if (!DeleteItems(hwnd_, paths, permanent, err) && !err.empty() && err != L"操作被取消")
        MessageBoxW(hwnd_, (L"删除失败：\n" + err).c_str(), kAppName, MB_ICONWARNING);
}


void MainWindow::ClipboardFiles(bool cut) {
    auto paths = SelectedPaths();
    std::wstring err;
    if (!ClipboardSetFiles(hwnd_, paths, cut, err) && !err.empty())
        MessageBoxW(hwnd_, err.c_str(), kAppName, MB_ICONWARNING);
}

void MainWindow::PasteFiles() {
    // 粘贴到第一个选中项的父目录，否则到第一个结果的父目录
    std::wstring dest;
    auto paths = SelectedPaths();
    if (!paths.empty()) {
        auto slash = paths[0].find_last_of(L'\\');
        dest = slash == std::wstring::npos ? paths[0] : paths[0].substr(0, slash);
    } else if (auto r = list_.Results(); r && r->data && !r->items.empty()) {
        dest = r->data->ParentPath(r->items[0]);
    }
    if (dest.empty()) { MessageBoxW(hwnd_, L"请先选中一个目标文件夹（或结果中的文件）。", kAppName, MB_ICONINFORMATION); return; }
    std::wstring err;
    if (!ClipboardPaste(hwnd_, dest, err) && !err.empty() && err != L"操作被取消")
        MessageBoxW(hwnd_, (L"粘贴失败：\n" + err).c_str(), kAppName, MB_ICONWARNING);
}

void MainWindow::CopyOrMoveTo(bool move) {
    auto paths = SelectedPaths();
    std::wstring err;
    if (!CopyOrMoveToFolder(hwnd_, paths, move, err) && !err.empty() && err != L"操作被取消")
        MessageBoxW(hwnd_, err.c_str(), kAppName, MB_ICONWARNING);
}

void MainWindow::InstallUrlProtocol(bool install) {
    std::wstring err;
    if (!::ff::InstallUrlProtocol(install, &err))
        MessageBoxW(hwnd_, err.empty() ? L"操作失败" : err.c_str(), kAppName, MB_ICONWARNING);
    else
        MessageBoxW(hwnd_, install ? L"已安装 es: 协议（当前用户）。" : L"已卸载 es: 协议。", kAppName, MB_ICONINFORMATION);
}

void MainWindow::InstallFolderContextMenu(bool install) {
    std::wstring err;
    if (!::ff::InstallFolderContextMenu(install, &err))
        MessageBoxW(hwnd_, err.empty() ? L"操作失败" : err.c_str(), kAppName, MB_ICONWARNING);
    else
        MessageBoxW(hwnd_, install ? L"已安装文件夹右键「用闪寻搜索」。" : L"已卸载文件夹右键菜单。", kAppName, MB_ICONINFORMATION);
}

void MainWindow::HistoryUpDown(int delta) {
    if (!GetSettings().historyEnabled) return;
    if (historyPos_ < 0) {
        historyCache_ = LoadSearchHistory();
        int len = GetWindowTextLengthW(edit_);
        historyDraft_.assign(static_cast<size_t>(len), L'\0');
        if (len) GetWindowTextW(edit_, historyDraft_.data(), len + 1);
        if (historyCache_.empty()) return;
        historyPos_ = (delta < 0) ? 0 : -1;
    }
    int n = static_cast<int>(historyCache_.size());
    int np = historyPos_ + delta;
    if (np < -1) np = -1;
    if (np >= n) np = n - 1;
    historyPos_ = np;
    const std::wstring& t = (historyPos_ < 0) ? historyDraft_ : historyCache_[static_cast<size_t>(historyPos_)];
    SetWindowTextW(edit_, t.c_str());
    SendMessageW(edit_, EM_SETSEL, t.size(), t.size());
    RunSearch();
}

void MainWindow::ApplyStartup(const std::wstring& search, int filter, SortSpec sort, const std::wstring& select, bool reindex) {
    startupSearch_ = search; startupFilter_ = filter; startupSort_ = sort; startupSelect_ = select; startupReindex_ = reindex;
}

// ---------- 深色菜单栏 ----------
// Windows 没有公开的深色菜单栏：拦截未公开的 UAH 消息自绘菜单栏（WM_UAHDRAWMENU / WM_UAHDRAWMENUITEM），
// 并在 WM_NCPAINT / WM_NCACTIVATE 之后补画菜单栏下方那条 1px 浅色线。弹出菜单用 uxtheme 序号 135/136（SetPreferredAppMode / FlushMenuThemes）。
namespace {
constexpr UINT WM_UAHDRAWMENU = 0x0091, WM_UAHDRAWMENUITEM = 0x0092;
union UAHMENUITEMMETRICS { struct { DWORD cx, cy; } rgsizeBar[2]; struct { DWORD cx, cy; } rgsizePopup[4]; };
struct UAHMENUPOPUPMETRICS { DWORD rgcx[4]; DWORD fUpdateMaxWidths : 2; };
struct UAHMENU { HMENU hmenu; HDC hdc; DWORD dwFlags; };
struct UAHMENUITEM { int iPosition; UAHMENUITEMMETRICS umim; UAHMENUPOPUPMETRICS umpm; };
struct UAHDRAWMENUITEM { DRAWITEMSTRUCT dis; UAHMENU um; UAHMENUITEM umi; };
}

bool MainWindow::DarkMenuBar(UINT m, WPARAM w, LPARAM l, LRESULT* out) {
    auto& t = GetTheme();
    if (!t.Dark()) return false;
    const auto& p = t.P();
    switch (m) {
    case WM_UAHDRAWMENU: {
        auto* um = reinterpret_cast<UAHMENU*>(l);
        MENUBARINFO mbi{sizeof(mbi)};
        if (!GetMenuBarInfo(hwnd_, OBJID_MENU, 0, &mbi)) return false;
        RECT wr; GetWindowRect(hwnd_, &wr);
        RECT rc = mbi.rcBar; OffsetRect(&rc, -wr.left, -wr.top);
        rc.top -= 1;
        HBRUSH b = CreateSolidBrush(p.bg); FillRect(um->hdc, &rc, b); DeleteObject(b);
        *out = TRUE; return true;
    }
    case WM_UAHDRAWMENUITEM: {
        auto* di = reinterpret_cast<UAHDRAWMENUITEM*>(l);
        wchar_t text[256] = {};
        MENUITEMINFOW mii{sizeof(mii)}; mii.fMask = MIIM_STRING; mii.dwTypeData = text; mii.cch = 255;
        GetMenuItemInfoW(di->um.hmenu, static_cast<UINT>(di->umi.iPosition), TRUE, &mii);
        UINT st = di->dis.itemState;
        COLORREF bg = p.bg, fg = p.text;
        if (st & (ODS_HOTLIGHT | ODS_SELECTED)) bg = p.hover;
        if (st & (ODS_INACTIVE | ODS_GRAYED | ODS_DISABLED)) fg = p.text2;
        HBRUSH b = CreateSolidBrush(bg); FillRect(di->um.hdc, &di->dis.rcItem, b); DeleteObject(b);
        SetBkMode(di->um.hdc, TRANSPARENT); SetTextColor(di->um.hdc, fg);
        DrawTextW(di->um.hdc, text, -1, &di->dis.rcItem, DT_CENTER | DT_SINGLELINE | DT_VCENTER | ((st & ODS_NOACCEL) ? DT_HIDEPREFIX : 0));
        *out = TRUE; return true;
    }
    case WM_NCPAINT: case WM_NCACTIVATE: {
        *out = DefWindowProcW(hwnd_, m, w, l);
        MENUBARINFO mbi{sizeof(mbi)};
        if (!GetMenuBarInfo(hwnd_, OBJID_MENU, 0, &mbi)) return true;
        RECT cr; GetClientRect(hwnd_, &cr); MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&cr), 2);
        RECT wr; GetWindowRect(hwnd_, &wr); OffsetRect(&cr, -wr.left, -wr.top);
        RECT line = cr; line.bottom = line.top; line.top -= 1;
        HDC dc = GetWindowDC(hwnd_);
        HBRUSH b = CreateSolidBrush(p.bg); FillRect(dc, &line, b); DeleteObject(b);
        ReleaseDC(hwnd_, dc);
        return true;
    }
    }
    return false;
}

// ---------- 键盘 ----------
bool MainWindow::PreTranslate(MSG& msg) {
    // 松开空格：关闭图片预览（即使焦点已变也处理，避免 Alt-Tab 丢 KEYUP 前先藏）
    if (msg.message == WM_KEYUP || msg.message == WM_SYSKEYUP) {
        if (msg.wParam == VK_SPACE && spacePreviewHeld_) { spacePreviewHeld_ = false; HideImagePreview(); return true; }
        return false;
    }
    if (msg.message != WM_KEYDOWN && msg.message != WM_SYSKEYDOWN) return false;
    if (renameEdit_ && msg.hwnd == renameEdit_) return false;   // 就地重命名编辑框自己处理按键
    if (msg.hwnd != hwnd_ && GetParent(msg.hwnd) != hwnd_ && GetParent(GetParent(msg.hwnd)) != hwnd_) return false;
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
    WPARAM k = msg.wParam;
    HWND lv = list_.Hwnd();

    if (k == VK_ESCAPE) {
        if (spacePreviewHeld_ || imagePreview_.Visible()) { spacePreviewHeld_ = false; HideImagePreview(); return true; }
        if (list_.SelectedCount() > 0) { list_.ClearSelection(); return true; }
        if (GetWindowTextLengthW(edit_) > 0) { SetWindowTextW(edit_, L""); SetFocus(edit_); }
        else if (GetSettings().escHides) HideToTray();
        return true;
    }
    if ((ctrl && k == 'F') || k == VK_F3 || (alt && k == 'D')) { SetFocus(edit_); SendMessageW(edit_, EM_SETSEL, 0, -1); return true; }
    if (k == VK_F5) {
        if (ctrl) StartIndex(true);
        else Refresh();
        return true;
    }
    if (ctrl && k == 'A' && msg.hwnd == list_.Hwnd()) { list_.SelectAll(); return true; }
    if (ctrl && k == 'Q') { DestroyWindow(hwnd_); return true; }
    if (ctrl && k == 'P') { PostMessageW(hwnd_, WM_COMMAND, IDM_OPTIONS, 0); return true; }
    if (ctrl && k == 'W') { HideToTray(); return true; }
    if (ctrl && !shift && k == 'R') { PostMessageW(hwnd_, WM_COMMAND, IDM_REGEX, 0); return true; }
    if (ctrl && !shift && k == 'I') { PostMessageW(hwnd_, WM_COMMAND, IDM_CASE, 0); return true; }
    if (ctrl && !shift && k == 'B') { PostMessageW(hwnd_, WM_COMMAND, IDM_WHOLEWORD, 0); return true; }
    if (ctrl && !shift && k == 'U') { PostMessageW(hwnd_, WM_COMMAND, IDM_MATCHPATH, 0); return true; }
    if (ctrl && !shift && k == 'X' && msg.hwnd == list_.Hwnd()) { PostMessageW(hwnd_, WM_COMMAND, IDM_CUT, 0); return true; }
    if (ctrl && !shift && k == 'C' && msg.hwnd == list_.Hwnd()) { PostMessageW(hwnd_, WM_COMMAND, IDM_COPY_FILES, 0); return true; }
    if (ctrl && !shift && k == 'V' && msg.hwnd == list_.Hwnd()) { PostMessageW(hwnd_, WM_COMMAND, IDM_PASTE, 0); return true; }

    if (msg.hwnd == edit_) {
        if (ctrl && k == 'A') { SendMessageW(edit_, EM_SETSEL, 0, -1); return true; }
        if (k == VK_UP) { HistoryUpDown(-1); return true; }
        if (k == VK_DOWN && GetKeyState(VK_MENU) >= 0) {
            // Alt+Down 也可翻历史；普通 Down 仍进列表（无历史浏览时）
            int len = GetWindowTextLengthW(edit_);
            DWORD start = 0, end = 0; SendMessageW(edit_, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
            if (historyPos_ >= 0 || (len == 0)) { HistoryUpDown(+1); return true; }
        }
        if (k == VK_DOWN || k == VK_NEXT) {
            if (ListView_GetItemCount(lv) > 0) {
                SetFocus(lv);
                ListView_SetItemState(lv, -1, 0, LVIS_SELECTED);
                ListView_SetItemState(lv, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EnsureVisible(lv, 0, FALSE);
            }
            return true;
        }
        if (k == VK_RETURN) {
            int len = GetWindowTextLengthW(edit_);
            std::wstring q(static_cast<size_t>(len), L'\0'); if (len) GetWindowTextW(edit_, q.data(), len + 1);
            AddSearchHistory(q); historyPos_ = -1;
            OpenItems(ctrl); return true;
        }
    } else if (msg.hwnd == lv) {
        if (k == VK_RETURN && alt) { ShowProperties(); return true; }
        if (k == VK_SPACE && !ctrl && !alt && !shift) {
            // 仅在列表焦点：按下显示、松开关闭；Show 失败不置 held，避免吞掉后续按键
            if (!spacePreviewHeld_) {
                if (ShowImagePreview()) spacePreviewHeld_ = true;
            }
            return true;  // 无论成败都吞掉空格，避免 ListView 切换选中
        }
        if (k == VK_RETURN) { OpenItems(ctrl); return true; }
        if (ctrl && shift && k == 'C') { CopyPaths(false); return true; }
        if (k == VK_F2 && !ctrl && !alt) { Rename(); return true; }
        if (k == VK_DELETE && !ctrl && !alt) { Delete(shift); return true; }
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
    // 系统右键菜单的子菜单（发送到、打开方式等）需要这些消息
    if ((cm3_ || cm2_) && (m == WM_INITMENUPOPUP || m == WM_DRAWITEM || m == WM_MEASUREITEM || m == WM_MENUCHAR)) {
        LRESULT r = 0;
        if (cm3_ && SUCCEEDED(cm3_->HandleMenuMsg2(m, w, l, &r))) return r;
        if (cm2_ && SUCCEEDED(cm2_->HandleMenuMsg(m, w, l))) return m == WM_INITMENUPOPUP ? 0 : TRUE;
    }
    if (m == taskbarCreated_ && taskbarCreated_) { tray_.Add(hwnd_, iconSmall_); return 0; }
    { LRESULT r = 0; if (DarkMenuBar(m, w, l, &r)) return r; }
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
        if (LOWORD(w) == WA_INACTIVE) {
            // lParam = 即将激活的窗口；若是我们的预览浮层则不要关掉
            HWND other = reinterpret_cast<HWND>(l);
            if (spacePreviewHeld_ && other != imagePreview_.Hwnd()) {
                spacePreviewHeld_ = false; HideImagePreview();
            }
        } else if (!spacePreviewHeld_ && !imagePreview_.Visible()) {
            SetFocus(edit_);
        }
        return 0;
    case WM_TIMER:
        if (w == kSearchTimerId) { KillTimer(hwnd_, kSearchTimerId); RunSearch(); }
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == IDC_FILTER) {
            if (HIWORD(w) == CBN_SELCHANGE) { GetSettings().filter = static_cast<int>(SendMessageW(filter_, CB_GETCURSEL, 0, 0)); RunSearch(); }
            return 0;
        }
        if (id == IDC_SEARCH) {
            if (HIWORD(w) == EN_CHANGE) ScheduleSearch();
            if (HIWORD(w) == EN_SETFOCUS || HIWORD(w) == EN_KILLFOCUS) InvalidateRect(hwnd_, &searchRc_, FALSE);
            return 0;
        }
        switch (id) {
        case IDM_EXIT: DestroyWindow(hwnd_); break;
        case IDM_REFRESH: Refresh(); break;
        case IDM_REINDEX: StartIndex(true); break;
        case IDM_THEME_SYSTEM: case IDM_THEME_LIGHT: case IDM_THEME_DARK:
            GetSettings().theme = static_cast<ThemeMode>(id - IDM_THEME_SYSTEM); ApplyTheme(); break;
        case IDM_OPTIONS: {
            auto r = ShowOptions(hwnd_);
            if (!r.applied) break;
            auto& s = GetSettings();
            g_fastAscii = s.fastAsciiSearch;
            if (r.hotkeyChanged) {
                hotkeys_.UnregisterToggle();
                hotkeyFailed_ = !hotkeys_.RegisterToggle(hwnd_, s.toggleKey);
                if (s.doubleCtrl) hotkeys_.EnableDoubleCtrl(hwnd_); else hotkeys_.DisableDoubleCtrl();
                if (hotkeyFailed_) MessageBoxW(hwnd_, (L"全局热键 " + s.toggleKey + L" 已被其他程序占用。").c_str(), kAppName, MB_ICONWARNING);
            }
            ApplyTheme();
            if (r.indexChanged) StartIndex(false); else RunSearch();
            UpdateStatus();
            break;
        }
        case IDM_INSTALL_SVC: case IDC_SVC_BTN: ServiceAction(true); break;
        case IDM_UNINSTALL_SVC: ServiceAction(false); break;
        case IDM_RENAME: Rename(); break;
        case IDM_RENAME_COMMIT: EndInlineRename(true); break;
        case IDM_DELETE: Delete(false); break;
        case IDM_DELETE_PERM: Delete(true); break;
        case IDM_COL_PATH: case IDM_COL_EXT: case IDM_COL_SIZE: case IDM_COL_DATE: {
            int c = (id == IDM_COL_PATH) ? ColPath : (id == IDM_COL_EXT) ? ColExt : (id == IDM_COL_SIZE) ? ColSize : ColDate;
            list_.SetColumnVisible(c, !list_.ColumnVisible(c)); ApplyTheme(); break;
        }
        case IDM_CASE: GetSettings().matchCase = !GetSettings().matchCase; ApplyTheme(); RunSearch(); break;
        case IDM_WHOLEWORD: GetSettings().matchWholeWord = !GetSettings().matchWholeWord; ApplyTheme(); RunSearch(); break;
        case IDM_MATCHPATH: GetSettings().matchPath = !GetSettings().matchPath; ApplyTheme(); RunSearch(); break;
        case IDM_REGEX: GetSettings().matchRegex = !GetSettings().matchRegex; ApplyTheme(); RunSearch(); break;
        case IDM_HISTORY_ENABLE: GetSettings().historyEnabled = !GetSettings().historyEnabled; ApplyTheme(); break;
        case IDM_CLEAR_HISTORY: ClearSearchHistory(); break;
        case IDM_CUT: ClipboardFiles(true); break;
        case IDM_COPY_FILES: ClipboardFiles(false); break;
        case IDM_PASTE: PasteFiles(); break;
        case IDM_COPY_TO: CopyOrMoveTo(false); break;
        case IDM_MOVE_TO: CopyOrMoveTo(true); break;
        case IDM_INSTALL_URL: InstallUrlProtocol(true); ApplyTheme(); break;
        case IDM_UNINSTALL_URL: InstallUrlProtocol(false); ApplyTheme(); break;
        case IDM_INSTALL_FOLDER_CM: InstallFolderContextMenu(true); ApplyTheme(); break;
        case IDM_UNINSTALL_FOLDER_CM: InstallFolderContextMenu(false); ApplyTheme(); break;
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
            if (nm->code == LVN_BEGINDRAG || nm->code == LVN_BEGINRDRAG) {   // 拖出：CF_HDROP + Shell IDList
                auto paths = SelectedPaths();
                if (!paths.empty()) DragOut(hwnd_, paths);
                return 0;
            }
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
    case WM_APP_SORT: RunSearch(); return 0;
    case WM_APP_TRAY:
        switch (LOWORD(l)) {
        case WM_LBUTTONUP: case NIN_SELECT: case NIN_KEYSELECT: Toggle(); break;
        case WM_CONTEXTMENU: case WM_RBUTTONUP: TrayMenu(); break;
        case NIN_BALLOONUSERCLICK: if (indexer_.Stats().needService) { ShowAndFocus(); ServiceAction(true); } break;
        }
        return 0;
    case WM_APP_RESULTS: {
        auto r = searcher_->Latest();
        if (r && r->generation == static_cast<uint64_t>(w)) {
            lastSearchMs_ = r->elapsedMs;
            list_.SetResults(r);
            if (softRefreshPending_) {
                softRefreshPending_ = false;
                list_.RefreshAttrsFromDisk();
            }
            UpdateStatus();
        }
        return 0;
    }
    case WM_APP_INDEX_PROGRESS: UpdateStatus(); return 0;
    case WM_APP_INDEX_DONE:
        if (!startupApplied_) {
            startupApplied_ = true;
            if (startupFilter_ >= 0 && filter_) SendMessageW(filter_, CB_SETCURSEL, startupFilter_, 0);
            if (startupSort_.col != SortCol::Name || startupSort_.desc) list_.SetSort(startupSort_);
            if (!startupSearch_.empty()) SetWindowTextW(edit_, startupSearch_.c_str());
            if (startupReindex_) { StartIndex(true); return 0; }
        }
        RunSearch(); UpdateStatus();
        if (!startupSelect_.empty()) {
            // 选中匹配完整路径的第一项
            auto r = list_.Results();
            if (r && r->data) {
                for (size_t row = 0; row < r->items.size(); ++row) {
                    if (_wcsicmp(r->data->FullPath(r->items[row]).c_str(), startupSelect_.c_str()) == 0) {
                        ListView_SetItemState(list_.Hwnd(), -1, 0, LVIS_SELECTED);
                        ListView_SetItemState(list_.Hwnd(), static_cast<int>(row), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                        ListView_EnsureVisible(list_.Hwnd(), static_cast<int>(row), FALSE);
                        break;
                    }
                }
            }
            startupSelect_.clear();
        }
        return 0;
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
