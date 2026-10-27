#include "Options.h"
#include "App.h"
#include "Dpi.h"
#include "Hotkey.h"
#include "Settings.h"
#include "Theme.h"
#include "ShellInstall.h"
#include <commctrl.h>
#include <windowsx.h>
#include <shobjidl.h>
#include <algorithm>
#include <vector>

namespace ff {

std::wstring FormatHotkey(UINT mods, UINT vk) {
    std::wstring s;
    auto add = [&](const wchar_t* t) { if (!s.empty()) s += L"+"; s += t; };
    if (mods & MOD_CONTROL) add(L"Ctrl");
    if (mods & MOD_ALT) add(L"Alt");
    if (mods & MOD_SHIFT) add(L"Shift");
    if (mods & MOD_WIN) add(L"Win");
    wchar_t k[16] = {};
    if (vk == VK_SPACE) wcscpy_s(k, L"Space");
    else if (vk == VK_TAB) wcscpy_s(k, L"Tab");
    else if (vk == VK_OEM_3) wcscpy_s(k, L"`");
    else if (vk >= VK_F1 && vk <= VK_F24) swprintf_s(k, L"F%u", vk - VK_F1 + 1);
    else if ((vk >= L'0' && vk <= L'9') || (vk >= L'A' && vk <= L'Z')) { k[0] = static_cast<wchar_t>(vk); }
    else return L"";
    add(k);
    return s;
}

namespace {

enum : int {
    IDC_TAB = 200, IDC_OK, IDC_CANCEL, IDC_APPLY,
    // 常规
    IDC_ESC_HIDES, IDC_CLOSE_TRAY, IDC_SELECT_ALL, IDC_FAST_ASCII, IDC_AUTOSTART, IDC_COMPRESS, IDC_THEME, IDC_DELAY,
    // 键盘
    IDC_HOTKEY, IDC_DOUBLE_CTRL, IDC_HK_STATUS,
    // NTFS
    IDC_VOLS, IDC_V_INCLUDE, IDC_V_MONITOR, IDC_V_USN, IDC_V_ONLY,
    // 排除
    IDC_EX_LIST, IDC_EX_EDIT, IDC_EX_ADD, IDC_EX_BROWSE, IDC_EX_REMOVE,
    IDC_URL_PROTO, IDC_FOLDER_CM,
};

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

bool AutostartEnabled() {
    wchar_t buf[MAX_PATH * 2]; DWORD cb = sizeof(buf);
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"FlashFind", RRF_RT_REG_SZ, nullptr, buf, &cb) == ERROR_SUCCESS;
}
void SetAutostart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring v = L"\"" + std::wstring(exe) + L"\" -startup";
        RegSetValueExW(k, L"FlashFind", 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()), static_cast<DWORD>((v.size() + 1) * 2));
    } else RegDeleteValueW(k, L"FlashFind");
    RegCloseKey(k);
}

UINT HkfToMod(BYTE f) { return ((f & HOTKEYF_CONTROL) ? MOD_CONTROL : 0) | ((f & HOTKEYF_ALT) ? MOD_ALT : 0) | ((f & HOTKEYF_SHIFT) ? MOD_SHIFT : 0); }
BYTE ModToHkf(UINT m) { return static_cast<BYTE>(((m & MOD_CONTROL) ? HOTKEYF_CONTROL : 0) | ((m & MOD_ALT) ? HOTKEYF_ALT : 0) | ((m & MOD_SHIFT) ? HOTKEYF_SHIFT : 0)); }

class Dlg {
public:
    OptionsResult Run(HWND owner, int page);
private:
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT, WPARAM, LPARAM);
    void Build();
    HWND Ctl(int page, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id);
    void ShowPage(int p);
    void LoadVol(); void StoreVol();
    bool Apply();
    void HotkeyStatus();

    HWND hwnd_ = nullptr, owner_ = nullptr, tab_ = nullptr;
    HFONT font_ = nullptr;
    UINT dpi_ = 96;
    std::vector<std::pair<int, HWND>> ctls_;   // (页, 控件)；-1 = 所有页
    Settings work_;
    int curVol_ = -1;
    bool done_ = false;
    OptionsResult res_;
};

HWND Dlg::Ctl(int page, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | style, Scale(x, dpi_), Scale(y, dpi_), Scale(w, dpi_), Scale(h, dpi_),
                             hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), App::Instance(), nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
    ctls_.emplace_back(page, c);
    return c;
}

void Dlg::Build() {
    const int L = 24, T = 48;
    tab_ = Ctl(-1, WC_TABCONTROLW, L"", WS_VISIBLE | WS_CLIPSIBLINGS, 12, 10, 516, 380, IDC_TAB);
    const wchar_t* pages[] = {L"常规", L"键盘", L"NTFS 卷", L"排除"};
    for (int i = 0; i < 4; ++i) { TCITEMW ti{TCIF_TEXT}; ti.pszText = const_cast<wchar_t*>(pages[i]); TabCtrl_InsertItem(tab_, i, &ti); }
    // 常规
    auto chk = [&](int p, const wchar_t* t, int y, int id, bool v) { HWND c = Ctl(p, WC_BUTTONW, t, BS_AUTOCHECKBOX | WS_TABSTOP, L, y, 460, 24, id); Button_SetCheck(c, v); };
    chk(0, L"按 Esc 隐藏窗口", T, IDC_ESC_HIDES, work_.escHides);
    chk(0, L"关闭按钮最小化到托盘", T + 30, IDC_CLOSE_TRAY, work_.closeToTray);
    chk(0, L"显示窗口时全选搜索框", T + 60, IDC_SELECT_ALL, work_.selectAllOnShow);
    chk(0, L"ASCII 快速搜索（fast_ascii_search）", T + 90, IDC_FAST_ASCII, work_.fastAsciiSearch);
    chk(0, L"开机时自动启动闪寻（当前用户）", T + 120, IDC_AUTOSTART, AutostartEnabled());
    chk(0, L"压缩数据库（更小，加载稍慢）", T + 150, IDC_COMPRESS, work_.compressDb);
    Ctl(0, WC_BUTTONW, L"安装 es: 协议与文件夹「用闪寻搜索」", BS_PUSHBUTTON | WS_TABSTOP, L, T + 180, 300, 26, IDC_URL_PROTO);
    Ctl(0, WC_BUTTONW, L"卸载 es: 协议与文件夹菜单", BS_PUSHBUTTON | WS_TABSTOP, L + 310, T + 180, 160, 26, IDC_FOLDER_CM);
    Ctl(0, WC_STATICW, L"主题：", 0, L, T + 220, 120, 22, -1);
    HWND th = Ctl(0, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, L + 130, T + 216, 160, 200, IDC_THEME);
    for (auto t : {L"跟随系统", L"浅色", L"深色"}) SendMessageW(th, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t));
    SendMessageW(th, CB_SETCURSEL, static_cast<int>(work_.theme), 0);
    Ctl(0, WC_STATICW, L"USN 更新间隔（毫秒）：", 0, L, T + 258, 160, 22, -1);
    HWND de = Ctl(0, WC_EDITW, std::to_wstring(work_.monitorUpdateDelay).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, L + 170, T + 254, 90, 24, IDC_DELAY);
    (void)de;
    // 键盘
    Ctl(1, WC_STATICW, L"显示/隐藏窗口的全局热键（在框内按下组合键）：", 0, L, T, 460, 22, -1);
    HWND hk = Ctl(1, HOTKEY_CLASSW, L"", WS_BORDER | WS_TABSTOP, L, T + 28, 260, 26, IDC_HOTKEY);
    HotkeyDef def = ParseHotkey(work_.toggleKey);
    SendMessageW(hk, HKM_SETRULES, HKCOMB_NONE | HKCOMB_S, MAKELPARAM(HOTKEYF_CONTROL | HOTKEYF_ALT, 0));
    SendMessageW(hk, HKM_SETHOTKEY, MAKEWORD(static_cast<BYTE>(def.vk), ModToHkf(def.mods)), 0);
    Ctl(1, WC_STATICW, L"", 0, L, T + 62, 460, 40, IDC_HK_STATUS);
    chk(1, L"双击 Ctrl 显示/隐藏窗口", T + 110, IDC_DOUBLE_CTRL, work_.doubleCtrl);
    Ctl(1, WC_STATICW, L"窗口内快捷键：Enter 打开，Ctrl+Enter 打开路径，Ctrl+Shift+C 复制完整路径，Alt+Enter 属性；列表按住空格预览图片/文本；搜索框 Ctrl+V 粘贴文字，列表 Ctrl+V 粘贴文件；Ctrl+A 全选；F5 刷新（Ctrl+F5 重建索引）；Esc 取消选中→清搜索→隐藏。", 0, L, T + 150, 470, 60, -1);
    // NTFS 卷
    Ctl(2, WC_STATICW, L"卷：", 0, L, T, 60, 22, -1);
    HWND vl = Ctl(2, WC_LISTBOXW, L"", WS_BORDER | WS_VSCROLL | LBS_NOTIFY | WS_TABSTOP, L, T + 24, 150, 260, IDC_VOLS);
    for (auto& v : work_.ntfsVolumes) SendMessageW(vl, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(v.path.c_str()));
    Ctl(2, WC_BUTTONW, L"包含到数据库", BS_AUTOCHECKBOX | WS_TABSTOP, L + 170, T + 24, 290, 24, IDC_V_INCLUDE);
    Ctl(2, WC_BUTTONW, L"监视变更（USN 日志）", BS_AUTOCHECKBOX | WS_TABSTOP, L + 170, T + 54, 290, 24, IDC_V_MONITOR);
    Ctl(2, WC_BUTTONW, L"需要时创建 USN 日志", BS_AUTOCHECKBOX | WS_TABSTOP, L + 170, T + 84, 290, 24, IDC_V_USN);
    Ctl(2, WC_STATICW, L"仅包含这些文件夹（分号分隔，空 = 整卷）：", 0, L + 170, T + 120, 300, 22, -1);
    Ctl(2, WC_EDITW, L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, L + 170, T + 146, 300, 24, IDC_V_ONLY);
    // 排除
    Ctl(3, WC_STATICW, L"排除的文件夹（及其子项）：", 0, L, T, 300, 22, -1);
    HWND el = Ctl(3, WC_LISTBOXW, L"", WS_BORDER | WS_VSCROLL | LBS_NOTIFY | WS_TABSTOP, L, T + 24, 470, 200, IDC_EX_LIST);
    for (auto& e : work_.excludeFolders) SendMessageW(el, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(e.c_str()));
    Ctl(3, WC_EDITW, L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, L, T + 234, 260, 24, IDC_EX_EDIT);
    Ctl(3, WC_BUTTONW, L"添加", BS_PUSHBUTTON | WS_TABSTOP, L + 268, T + 233, 64, 26, IDC_EX_ADD);
    Ctl(3, WC_BUTTONW, L"浏览…", BS_PUSHBUTTON | WS_TABSTOP, L + 338, T + 233, 64, 26, IDC_EX_BROWSE);
    Ctl(3, WC_BUTTONW, L"删除", BS_PUSHBUTTON | WS_TABSTOP, L + 408, T + 233, 64, 26, IDC_EX_REMOVE);
    // 底部按钮
    Ctl(-1, WC_BUTTONW, L"确定", WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP, 288, 400, 76, 28, IDC_OK);
    Ctl(-1, WC_BUTTONW, L"取消", WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP, 370, 400, 76, 28, IDC_CANCEL);
    Ctl(-1, WC_BUTTONW, L"应用", WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP, 452, 400, 76, 28, IDC_APPLY);
    SetWindowPos(tab_, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    if (!work_.ntfsVolumes.empty()) { SendMessageW(vl, LB_SETCURSEL, 0, 0); curVol_ = 0; LoadVol(); }
    HotkeyStatus();
}

void Dlg::ShowPage(int p) {
    for (auto& [pg, c] : ctls_) if (pg >= 0) ShowWindow(c, pg == p ? SW_SHOW : SW_HIDE);
}

void Dlg::LoadVol() {
    bool ok = curVol_ >= 0 && curVol_ < static_cast<int>(work_.ntfsVolumes.size());
    for (int id : {IDC_V_INCLUDE, IDC_V_MONITOR, IDC_V_USN, IDC_V_ONLY}) EnableWindow(GetDlgItem(hwnd_, id), ok);
    if (!ok) return;
    auto& v = work_.ntfsVolumes[static_cast<size_t>(curVol_)];
    Button_SetCheck(GetDlgItem(hwnd_, IDC_V_INCLUDE), v.include);
    Button_SetCheck(GetDlgItem(hwnd_, IDC_V_MONITOR), v.monitor);
    Button_SetCheck(GetDlgItem(hwnd_, IDC_V_USN), v.enableUsnJournal);
    std::wstring only; for (auto& s : v.includeOnly) { if (!only.empty()) only += L";"; only += s; }
    SetDlgItemTextW(hwnd_, IDC_V_ONLY, only.c_str());
}

void Dlg::StoreVol() {
    if (curVol_ < 0 || curVol_ >= static_cast<int>(work_.ntfsVolumes.size())) return;
    auto& v = work_.ntfsVolumes[static_cast<size_t>(curVol_)];
    v.include = Button_GetCheck(GetDlgItem(hwnd_, IDC_V_INCLUDE)) == BST_CHECKED;
    v.monitor = Button_GetCheck(GetDlgItem(hwnd_, IDC_V_MONITOR)) == BST_CHECKED;
    v.enableUsnJournal = Button_GetCheck(GetDlgItem(hwnd_, IDC_V_USN)) == BST_CHECKED;
    wchar_t buf[4096]; GetDlgItemTextW(hwnd_, IDC_V_ONLY, buf, 4096);
    v.includeOnly.clear();
    std::wstring s(buf);
    for (size_t b = 0; b <= s.size();) {
        size_t e = s.find(L';', b); if (e == std::wstring::npos) e = s.size();
        std::wstring x = s.substr(b, e - b);
        while (!x.empty() && (x.back() == L' ' || x.back() == L'\\')) x.pop_back();
        while (!x.empty() && x.front() == L' ') x.erase(0, 1);
        if (!x.empty()) v.includeOnly.push_back(x);
        b = e + 1;
    }
}

void Dlg::HotkeyStatus() {
    WORD hk = static_cast<WORD>(SendDlgItemMessageW(hwnd_, IDC_HOTKEY, HKM_GETHOTKEY, 0, 0));
    std::wstring spec = FormatHotkey(HkfToMod(HIBYTE(hk)), LOBYTE(hk));
    std::wstring msg;
    if (spec.empty() || !(HkfToMod(HIBYTE(hk)) & (MOD_CONTROL | MOD_ALT | MOD_WIN))) msg = L"⚠ 请使用 Ctrl/Alt 组合键（支持字母、数字、F1-F24、Space、Tab、`）";
    else if (spec == GetSettings().toggleKey) msg = L"当前热键：" + spec;
    else {
        // 冲突检测：用临时 id 试注册
        HotkeyDef d = ParseHotkey(spec);
        if (RegisterHotKey(hwnd_, 0x7F01, d.mods | MOD_NOREPEAT, d.vk)) { UnregisterHotKey(hwnd_, 0x7F01); msg = L"✓ " + spec + L" 可用"; }
        else msg = L"⚠ " + spec + L" 已被其他程序占用";
    }
    SetDlgItemTextW(hwnd_, IDC_HK_STATUS, msg.c_str());
}

bool Dlg::Apply() {
    StoreVol();
    auto& s = GetSettings();
    auto on = [&](int id) { return Button_GetCheck(GetDlgItem(hwnd_, id)) == BST_CHECKED; };
    work_.escHides = on(IDC_ESC_HIDES); work_.closeToTray = on(IDC_CLOSE_TRAY); work_.selectAllOnShow = on(IDC_SELECT_ALL);
    work_.fastAsciiSearch = on(IDC_FAST_ASCII); work_.compressDb = on(IDC_COMPRESS);
    work_.theme = static_cast<ThemeMode>(std::clamp(static_cast<int>(SendDlgItemMessageW(hwnd_, IDC_THEME, CB_GETCURSEL, 0, 0)), 0, 2));
    work_.monitorUpdateDelay = std::clamp(static_cast<int>(GetDlgItemInt(hwnd_, IDC_DELAY, nullptr, FALSE)), 100, 60000);
    work_.doubleCtrl = on(IDC_DOUBLE_CTRL);
    WORD hk = static_cast<WORD>(SendDlgItemMessageW(hwnd_, IDC_HOTKEY, HKM_GETHOTKEY, 0, 0));
    std::wstring spec = FormatHotkey(HkfToMod(HIBYTE(hk)), LOBYTE(hk));
    if (!spec.empty() && (HkfToMod(HIBYTE(hk)) & (MOD_CONTROL | MOD_ALT))) work_.toggleKey = spec;
    work_.excludeFolders.clear();
    HWND el = GetDlgItem(hwnd_, IDC_EX_LIST);
    for (int i = 0, n = static_cast<int>(SendMessageW(el, LB_GETCOUNT, 0, 0)); i < n; ++i) {
        wchar_t b[MAX_PATH * 2]; if (SendMessageW(el, LB_GETTEXTLEN, i, 0) < MAX_PATH * 2) { SendMessageW(el, LB_GETTEXT, i, reinterpret_cast<LPARAM>(b)); work_.excludeFolders.emplace_back(b); }
    }
    if (on(IDC_AUTOSTART) != AutostartEnabled()) SetAutostart(on(IDC_AUTOSTART));
    auto volEq = [](const VolumeConfig& a, const VolumeConfig& b) { return a.path == b.path && a.include == b.include && a.monitor == b.monitor && a.enableUsnJournal == b.enableUsnJournal && a.includeOnly == b.includeOnly; };
    bool volsSame = s.ntfsVolumes.size() == work_.ntfsVolumes.size() && std::equal(s.ntfsVolumes.begin(), s.ntfsVolumes.end(), work_.ntfsVolumes.begin(), volEq);
    res_.indexChanged |= !volsSame || s.excludeFolders != work_.excludeFolders || s.monitorUpdateDelay != work_.monitorUpdateDelay || s.compressDb != work_.compressDb || s.fastAsciiSearch != work_.fastAsciiSearch;
    res_.hotkeyChanged |= s.toggleKey != work_.toggleKey || s.doubleCtrl != work_.doubleCtrl;
    res_.themeChanged |= s.theme != work_.theme;
    // 只覆盖本对话框管理的字段（窗口位置、列宽等保持不变）
    s.escHides = work_.escHides; s.closeToTray = work_.closeToTray; s.selectAllOnShow = work_.selectAllOnShow;
    s.fastAsciiSearch = work_.fastAsciiSearch; s.compressDb = work_.compressDb; s.theme = work_.theme;
    s.monitorUpdateDelay = work_.monitorUpdateDelay; s.doubleCtrl = work_.doubleCtrl; s.toggleKey = work_.toggleKey;
    s.excludeFolders = work_.excludeFolders; s.ntfsVolumes = work_.ntfsVolumes;
    s.Save();
    res_.applied = true;
    return true;
}

LRESULT CALLBACK Dlg::Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) { auto* cs = reinterpret_cast<CREATESTRUCTW*>(l); SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams)); reinterpret_cast<Dlg*>(cs->lpCreateParams)->hwnd_ = h; }
    auto* self = reinterpret_cast<Dlg*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->Handle(m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT Dlg::Handle(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_NOTIFY: {
        auto* nm = reinterpret_cast<NMHDR*>(l);
        if (nm->hwndFrom == tab_ && nm->code == TCN_SELCHANGE) ShowPage(TabCtrl_GetCurSel(tab_));
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(w), code = HIWORD(w);
        switch (id) {
        case IDC_OK: case IDOK: Apply(); done_ = true; return 0;
        case IDC_CANCEL: case IDCANCEL: done_ = true; return 0;
        case IDC_APPLY: Apply(); return 0;
        case IDC_URL_PROTO: {
            std::wstring err;
            if (::ff::InstallUrlProtocol(true, &err) && ::ff::InstallFolderContextMenu(true, &err))
                MessageBoxW(hwnd_, L"已安装 es: 协议与文件夹右键「用闪寻搜索」。", L"闪寻", MB_OK | MB_ICONINFORMATION);
            else MessageBoxW(hwnd_, err.empty() ? L"安装失败" : err.c_str(), L"闪寻", MB_OK | MB_ICONWARNING);
            return 0;
        }
        case IDC_FOLDER_CM: {
            std::wstring err;
            if (::ff::InstallUrlProtocol(false, &err) && ::ff::InstallFolderContextMenu(false, &err))
                MessageBoxW(hwnd_, L"已卸载 es: 协议与文件夹菜单。", L"闪寻", MB_OK | MB_ICONINFORMATION);
            else MessageBoxW(hwnd_, err.empty() ? L"卸载失败" : err.c_str(), L"闪寻", MB_OK | MB_ICONWARNING);
            return 0;
        }
        case IDC_HOTKEY: if (code == EN_CHANGE) HotkeyStatus(); return 0;
        case IDC_VOLS:
            if (code == LBN_SELCHANGE) { StoreVol(); curVol_ = static_cast<int>(SendDlgItemMessageW(hwnd_, IDC_VOLS, LB_GETCURSEL, 0, 0)); LoadVol(); }
            return 0;
        case IDC_EX_ADD: {
            wchar_t b[MAX_PATH * 2]; GetDlgItemTextW(hwnd_, IDC_EX_EDIT, b, MAX_PATH * 2);
            std::wstring x(b); while (!x.empty() && (x.back() == L'\\' || x.back() == L' ')) x.pop_back();
            if (!x.empty() && SendDlgItemMessageW(hwnd_, IDC_EX_LIST, LB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(x.c_str())) == LB_ERR)
                SendDlgItemMessageW(hwnd_, IDC_EX_LIST, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(x.c_str()));
            SetDlgItemTextW(hwnd_, IDC_EX_EDIT, L"");
            return 0;
        }
        case IDC_EX_BROWSE: {
            IFileOpenDialog* fd = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fd)))) {
                DWORD o = 0; fd->GetOptions(&o); fd->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
                if (SUCCEEDED(fd->Show(hwnd_))) {
                    IShellItem* it = nullptr;
                    if (SUCCEEDED(fd->GetResult(&it))) {
                        PWSTR p = nullptr;
                        if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { SetDlgItemTextW(hwnd_, IDC_EX_EDIT, p); CoTaskMemFree(p); SendMessageW(hwnd_, WM_COMMAND, IDC_EX_ADD, 0); }
                        it->Release();
                    }
                }
                fd->Release();
            }
            return 0;
        }
        case IDC_EX_REMOVE: {
            int i = static_cast<int>(SendDlgItemMessageW(hwnd_, IDC_EX_LIST, LB_GETCURSEL, 0, 0));
            if (i >= 0) SendDlgItemMessageW(hwnd_, IDC_EX_LIST, LB_DELETESTRING, i, 0);
            return 0;
        }
        }
        break;
    }
    case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN: {
        HDC dc = reinterpret_cast<HDC>(w);
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }
    case WM_CLOSE: done_ = true; return 0;
    }
    return DefWindowProcW(hwnd_, m, w, l);
}

OptionsResult Dlg::Run(HWND owner, int page) {
    owner_ = owner;
    work_ = GetSettings();
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc{sizeof(wc)}; wc.lpfnWndProc = Proc; wc.hInstance = App::Instance(); wc.lpszClassName = L"FlashFindOptions";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        RegisterClassExW(&wc); reg = true;
    }
    dpi_ = DpiOf(owner);
    NONCLIENTMETRICSW ncm{sizeof(ncm)}; SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi_);
    font_ = CreateFontIndirectW(&ncm.lfMessageFont);
    RECT orc{}; GetWindowRect(owner, &orc);
    RECT rc{0, 0, Scale(540, dpi_), Scale(440, dpi_)};
    AdjustWindowRectExForDpi(&rc, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME, dpi_);
    int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, L"FlashFindOptions", L"闪寻 选项", WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                    orc.left + ((orc.right - orc.left) - ww) / 2, orc.top + ((orc.bottom - orc.top) - wh) / 2, ww, wh, owner, nullptr, App::Instance(), this);
    if (!hwnd_) { DeleteObject(font_); return res_; }
    Build();
    TabCtrl_SetCurSel(tab_, std::clamp(page, 0, 3));
    ShowPage(std::clamp(page, 0, 3));
    GetTheme().ApplyToWindow(hwnd_);
    EnableWindow(owner, FALSE);
    ShowWindow(hwnd_, SW_SHOW);
    MSG msg{};
    while (!done_ && GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE && GetDlgCtrlID(msg.hwnd) != IDC_HOTKEY) { done_ = true; continue; }
        if (IsDialogMessageW(hwnd_, &msg)) continue;
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    if (msg.message == WM_QUIT) PostQuitMessage(static_cast<int>(msg.wParam));
    EnableWindow(owner, TRUE);
    DestroyWindow(hwnd_);
    SetActiveWindow(owner);
    DeleteObject(font_);
    return res_;
}

} // namespace

OptionsResult ShowOptions(HWND owner, int page) { Dlg d; return d.Run(owner, page); }

} // namespace ff
