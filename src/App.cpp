#include "App.h"
#include "MainWindow.h"
#include "Settings.h"
#include "Theme.h"
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include "Index.h"
#include "Search.h"

namespace ff {

static HINSTANCE g_inst = nullptr;
HINSTANCE App::Instance() { return g_inst; }

// 自检：FlashFind.exe -selftest <根目录> <搜索词>，结果写到 %TEMP%\\ff_selftest.txt
static int SelfTest() {
    int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc < 4) return 2;
    std::wstring out;
    DWORD t0 = GetTickCount();
    Indexer ix;
    ix.Start(nullptr, {argv[2]}, {});
    while (ix.Busy()) Sleep(20);
    auto d = ix.Snapshot();
    DWORD t1 = GetTickCount();
    Searcher s(nullptr);
    s.Request(argv[3], d);
    std::shared_ptr<SearchResult> r;
    while (!(r = s.Latest())) Sleep(5);
    wchar_t buf[512];
    swprintf_s(buf, L"entries=%zu index_ms=%lu results=%zu search_ms=%.2f names_chars=%zu\r\n",
               d->entries.size(), t1 - t0, r->items.size(), r->elapsedMs, d->names.size());
    out += buf;
    for (size_t k = 0; k < r->items.size() && k < 10; ++k) out += d->FullPath(r->items[k]) + L"\r\n";
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::wstring path = std::wstring(tmp) + L"ff_selftest.txt";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD wr; WriteFile(f, "\xFF\xFE", 2, &wr, nullptr);
    WriteFile(f, out.data(), static_cast<DWORD>(out.size() * 2), &wr, nullptr);
    CloseHandle(f);
    LocalFree(argv);
    return 0;
}

int App::Run(HINSTANCE inst, int show) {
    g_inst = inst;
    if (wcsstr(GetCommandLineW(), L"-selftest")) return SelfTest();
    // 单实例：已有实例则让它显示后退出
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\FlashFind.Instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND h = FindWindowW(kMainClass, nullptr)) {
            DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(h, WM_APP_SHOW, 0, 0);
        }
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    GetSettings().Load();
    GetTheme().Resolve(GetSettings().theme);

    bool startHidden = wcsstr(GetCommandLineW(), L"-startup") != nullptr;
    MainWindow wnd;
    if (!wnd.Create(startHidden ? SW_HIDE : show)) return 1;

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (wnd.PreTranslate(msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    GetSettings().Save();
    CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return static_cast<int>(msg.wParam);
}

} // namespace ff
