#include "FileOps.h"
#include "App.h"
#include "Dpi.h"
#include "Theme.h"
#include <shellapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <shlguid.h>
#include <commctrl.h>
#include <algorithm>

#ifndef FOFX_RECYCLEONDELETE
#define FOFX_RECYCLEONDELETE 0x00080000
#endif

namespace ff {

namespace {
template <class T> struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

std::wstring HrText(HRESULT hr) {
    wchar_t* t = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, static_cast<DWORD>(hr), 0, reinterpret_cast<LPWSTR>(&t), 0, nullptr);
    wchar_t b[32]; swprintf_s(b, L"0x%08X", static_cast<unsigned>(hr));
    std::wstring s = t ? std::wstring(t) : std::wstring();
    if (t) LocalFree(t);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
    return s.empty() ? std::wstring(b) : s + L"（" + b + L"）";
}

// 绝对路径 -> IShellItemArray（可跨文件夹）
HRESULT MakeItemArray(const std::vector<std::wstring>& paths, IShellItemArray** out) {
    std::vector<PIDLIST_ABSOLUTE> ids;
    HRESULT hr = S_OK;
    for (auto& p : paths) {
        PIDLIST_ABSOLUTE id = nullptr;
        hr = SHParseDisplayName(p.c_str(), nullptr, &id, 0, nullptr);
        if (FAILED(hr) || !id) break;
        ids.push_back(id);
    }
    if (SUCCEEDED(hr) && !ids.empty())
        hr = SHCreateShellItemArrayFromIDLists(static_cast<UINT>(ids.size()), const_cast<PCIDLIST_ABSOLUTE_ARRAY>(reinterpret_cast<PIDLIST_ABSOLUTE*>(ids.data())), out);
    for (auto id : ids) CoTaskMemFree(id);
    return ids.empty() && SUCCEEDED(hr) ? E_INVALIDARG : hr;
}
} // namespace

bool ValidFileName(const std::wstring& n) {
    if (n.empty() || n == L"." || n == L".." || n.size() > 255) return false;
    if (n.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) return false;
    for (wchar_t c : n) if (c < 32) return false;
    return n.back() != L' ';
}

size_t RenameItems(HWND owner, const std::vector<RenameReq>& reqs, std::wstring& err) {
    Com<IFileOperation> op;
    HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op));
    size_t queued = 0;
    if (SUCCEEDED(hr)) {
        op->SetOwnerWindow(owner);
        op->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR | FOFX_SHOWELEVATIONPROMPT);
        for (auto& r : reqs) {
            Com<IShellItem> it;
            HRESULT h2 = SHCreateItemFromParsingName(r.path.c_str(), nullptr, IID_PPV_ARGS(&it));
            if (SUCCEEDED(h2)) h2 = op->RenameItem(it.p, r.newName.c_str(), nullptr);
            if (SUCCEEDED(h2)) ++queued;
            else if (err.empty()) err = r.path + L"：" + HrText(h2);
        }
        if (!queued) return 0;
        hr = op->PerformOperations();
        BOOL aborted = FALSE; op->GetAnyOperationsAborted(&aborted);
        if (FAILED(hr) && err.empty()) err = HrText(hr);
        if (aborted && err.empty()) err = L"操作被取消";
        return SUCCEEDED(hr) && !aborted ? queued : 0;
    }
    // 退路：MoveFileEx（无撤销）
    size_t ok = 0;
    for (auto& r : reqs) {
        size_t s = r.path.find_last_of(L'\\');
        std::wstring dst = r.path.substr(0, s + 1) + r.newName;
        if (MoveFileExW(r.path.c_str(), dst.c_str(), 0)) ++ok;
        else if (err.empty()) err = r.path + L"：" + HrText(HRESULT_FROM_WIN32(GetLastError()));
    }
    return ok;
}

bool DeleteItems(HWND owner, const std::vector<std::wstring>& paths, bool permanent, std::wstring& err) {
    Com<IFileOperation> op;
    HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op));
    if (FAILED(hr)) { err = HrText(hr); return false; }
    Com<IShellItemArray> arr;
    hr = MakeItemArray(paths, &arr);
    if (FAILED(hr)) { err = HrText(hr); return false; }
    op->SetOwnerWindow(owner);
    DWORD flags = FOFX_SHOWELEVATIONPROMPT;
    if (permanent) flags |= FOF_NOCONFIRMATION;          // 已由调用方确认
    else flags |= FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE;   // 回收站（是否再确认按系统回收站设置）
    op->SetOperationFlags(flags);
    hr = op->DeleteItems(arr.p);
    if (SUCCEEDED(hr)) hr = op->PerformOperations();
    BOOL aborted = FALSE; op->GetAnyOperationsAborted(&aborted);
    if (FAILED(hr)) { err = HrText(hr); return false; }
    if (aborted) { err = L"操作被取消"; return false; }
    return true;
}

bool DragOut(HWND owner, const std::vector<std::wstring>& paths) {
    Com<IShellItemArray> arr;
    if (FAILED(MakeItemArray(paths, &arr))) return false;
    Com<IDataObject> dobj;
    if (FAILED(arr->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&dobj)))) return false;
    DWORD eff = 0;
    // 默认放置源 + 系统拖动图像；Shift 移动 / Ctrl 复制 / Alt 快捷方式 由目标决定
    return SUCCEEDED(SHDoDragDrop(owner, dobj.p, nullptr, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &eff));
}

// ---------- 多项重命名对话框 ----------
namespace {
enum : int { IDC_OLD = 300, IDC_NEW, IDC_FIND, IDC_REPL, IDC_DOREPL, IDC_ROK, IDC_RCANCEL, IDC_RINFO };

class RenameDlg {
public:
    bool Run(HWND owner, const std::vector<std::wstring>& paths, std::vector<std::wstring>& names) {
        names_ = &names; paths_ = &paths;
        static bool reg = false;
        if (!reg) {
            WNDCLASSEXW wc{sizeof(wc)}; wc.lpfnWndProc = Proc; wc.hInstance = App::Instance(); wc.lpszClassName = L"FlashFindRename";
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
            RegisterClassExW(&wc); reg = true;
        }
        dpi_ = DpiOf(owner);
        NONCLIENTMETRICSW ncm{sizeof(ncm)}; SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi_);
        font_ = CreateFontIndirectW(&ncm.lfMessageFont);
        RECT orc{}; GetWindowRect(owner, &orc);
        RECT rc{0, 0, S(720), S(460)};
        AdjustWindowRectExForDpi(&rc, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME, dpi_);
        int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
        CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, L"FlashFindRename", L"重命名多个项目", WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                        orc.left + ((orc.right - orc.left) - ww) / 2, orc.top + ((orc.bottom - orc.top) - wh) / 2, ww, wh, owner, nullptr, App::Instance(), this);
        if (!hwnd_) { DeleteObject(font_); return false; }
        Build();
        GetTheme().ApplyToWindow(hwnd_);
        EnableWindow(owner, FALSE);
        ShowWindow(hwnd_, SW_SHOW);
        SetFocus(newEd_);
        MSG msg{};
        while (!done_ && GetMessageW(&msg, nullptr, 0, 0)) {
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) { done_ = true; continue; }
            if (IsDialogMessageW(hwnd_, &msg)) continue;
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        if (msg.message == WM_QUIT) PostQuitMessage(static_cast<int>(msg.wParam));
        EnableWindow(owner, TRUE);
        DestroyWindow(hwnd_);
        SetActiveWindow(owner);
        DeleteObject(font_);
        return ok_;
    }
private:
    int S(int v) const { return Scale(v, dpi_); }
    HWND Ctl(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0) {
        HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), App::Instance(), nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
        return c;
    }
    static std::wstring Join(const std::vector<std::wstring>& v) { std::wstring s; for (auto& x : v) { if (!s.empty()) s += L"\r\n"; s += x; } return s; }
    void Build() {
        wchar_t info[160]; swprintf_s(info, L"共 %zu 项（可跨文件夹）。右侧每行一个新名称，行数须与左侧一致；未改动的行跳过。", names_->size());
        Ctl(WC_STATICW, info, 0, 12, 10, 696, 20, IDC_RINFO);
        Ctl(WC_STATICW, L"原名称", 0, 12, 34, 340, 18, 0);
        Ctl(WC_STATICW, L"新名称", 0, 368, 34, 340, 18, 0);
        DWORD ml = WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_TABSTOP;
        oldEd_ = Ctl(WC_EDITW, Join(*names_).c_str(), ml | ES_READONLY, 12, 54, 340, 320, IDC_OLD, WS_EX_CLIENTEDGE);
        newEd_ = Ctl(WC_EDITW, Join(*names_).c_str(), ml, 368, 54, 340, 320, IDC_NEW, WS_EX_CLIENTEDGE);
        Ctl(WC_STATICW, L"查找：", 0, 12, 389, 44, 20, 0);
        Ctl(WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP, 56, 386, 200, 24, IDC_FIND, WS_EX_CLIENTEDGE);
        Ctl(WC_STATICW, L"替换为：", 0, 266, 389, 56, 20, 0);
        Ctl(WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP, 322, 386, 200, 24, IDC_REPL, WS_EX_CLIENTEDGE);
        Ctl(WC_BUTTONW, L"全部替换", BS_PUSHBUTTON | WS_TABSTOP, 530, 385, 90, 26, IDC_DOREPL);
        Ctl(WC_BUTTONW, L"重命名", BS_DEFPUSHBUTTON | WS_TABSTOP, 520, 422, 90, 28, IDC_ROK);
        Ctl(WC_BUTTONW, L"取消", BS_PUSHBUTTON | WS_TABSTOP, 618, 422, 90, 28, IDC_RCANCEL);
    }
    static std::wstring Text(HWND h) { int n = GetWindowTextLengthW(h); std::wstring s(static_cast<size_t>(n), L'\0'); if (n) GetWindowTextW(h, s.data(), n + 1); return s; }
    static std::vector<std::wstring> Lines(const std::wstring& t) {
        std::vector<std::wstring> v; std::wstring cur;
        for (wchar_t c : t) { if (c == L'\r') continue; if (c == L'\n') { v.push_back(cur); cur.clear(); } else cur += c; }
        v.push_back(cur);
        while (v.size() > 1 && v.back().empty()) v.pop_back();
        return v;
    }
    void ReplaceAll() {
        std::wstring f = Text(GetDlgItem(hwnd_, IDC_FIND)), r = Text(GetDlgItem(hwnd_, IDC_REPL));
        if (f.empty()) return;
        auto lines = Lines(Text(newEd_));
        for (auto& l : lines) {   // 不区分大小写
            std::wstring o; size_t i = 0;
            while (i < l.size()) {
                if (i + f.size() <= l.size() && CompareStringOrdinal(l.c_str() + i, static_cast<int>(f.size()), f.c_str(), static_cast<int>(f.size()), TRUE) == CSTR_EQUAL) { o += r; i += f.size(); }
                else o += l[i++];
            }
            l = o;
        }
        SetWindowTextW(newEd_, Join(lines).c_str());
    }
    void Ok() {
        auto lines = Lines(Text(newEd_));
        if (lines.size() != names_->size()) {
            wchar_t b[128]; swprintf_s(b, L"新名称有 %zu 行，应为 %zu 行。", lines.size(), names_->size());
            MessageBoxW(hwnd_, b, L"重命名", MB_ICONWARNING); return;
        }
        for (size_t k = 0; k < lines.size(); ++k) if (!ValidFileName(lines[k])) {
            std::wstring m = L"第 " + std::to_wstring(k + 1) + L" 行不是合法的文件名：\n" + lines[k] + L"\n\n不能为空，不能包含 \\ / : * ? \" < > |";
            MessageBoxW(hwnd_, m.c_str(), L"重命名", MB_ICONWARNING); return;
        }
        *names_ = std::move(lines); ok_ = true; done_ = true;
    }
    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        RenameDlg* self = nullptr;
        if (m == WM_NCCREATE) { self = static_cast<RenameDlg*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams); self->hwnd_ = h; SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); }
        else self = reinterpret_cast<RenameDlg*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (!self) return DefWindowProcW(h, m, w, l);
        switch (m) {
        case WM_COMMAND:
            switch (LOWORD(w)) {
            case IDC_ROK: case IDOK: self->Ok(); return 0;
            case IDC_RCANCEL: case IDCANCEL: self->done_ = true; return 0;
            case IDC_DOREPL: self->ReplaceAll(); return 0;
            case IDC_NEW:   // 两侧滚动同步
                if (HIWORD(w) == EN_VSCROLL) {
                    int a = static_cast<int>(SendMessageW(self->newEd_, EM_GETFIRSTVISIBLELINE, 0, 0)), b = static_cast<int>(SendMessageW(self->oldEd_, EM_GETFIRSTVISIBLELINE, 0, 0));
                    SendMessageW(self->oldEd_, EM_LINESCROLL, 0, a - b);
                }
                return 0;
            }
            break;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(w);
            SetBkColor(dc, GetSysColor(COLOR_WINDOW));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
        case WM_CLOSE: self->done_ = true; return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }
    HWND hwnd_ = nullptr, oldEd_ = nullptr, newEd_ = nullptr;
    HFONT font_ = nullptr; UINT dpi_ = 96;
    bool done_ = false, ok_ = false;
    std::vector<std::wstring>* names_ = nullptr; const std::vector<std::wstring>* paths_ = nullptr;
};
} // namespace

bool ShowRenameDialog(HWND owner, const std::vector<std::wstring>& paths, std::vector<std::wstring>& names) {
    RenameDlg d; return d.Run(owner, paths, names);
}

} // namespace ff
