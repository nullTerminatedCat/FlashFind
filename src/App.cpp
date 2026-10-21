#include "App.h"
#include "MainWindow.h"
#include "Settings.h"
#include "Theme.h"
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <psapi.h>
#include <algorithm>
#include "Fold.h"
#include "Index.h"
#include "Search.h"

namespace ff {

static HINSTANCE g_inst = nullptr;
HINSTANCE App::Instance() { return g_inst; }

// 自检：FlashFind.exe -selftest <根目录> <搜索词> [-compress]
// 依次测：冷重建(含排序+保存)、热加载、数据库大小、每对象内存、搜索耗时。结果写到 %TEMP%\ff_selftest.txt
static int SelfTest() {
    int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc < 4) return 2;
    bool compress = wcsstr(GetCommandLineW(), L"-compress") != nullptr;
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::wstring db = std::wstring(tmp) + L"ff_selftest.db";
    DeleteFileW(db.c_str());
    std::wstring out; wchar_t buf[1024];
    auto waitIdle = [](Indexer& ix) { while (ix.Busy()) Sleep(10); };

    Indexer::Options o; o.roots = {argv[2]}; o.dbPath = db; o.compress = compress;
    // 不加排除目录，便于与 M1 基线（同样无排除）对比
    {   // 冷重建
        Indexer ix; o.forceRebuild = true;
        auto t = GetTickCount64();
        ix.Start(nullptr, o); waitIdle(ix);
        auto st = ix.Stats(); auto d = ix.Snapshot();
        swprintf_s(buf, L"[cold] total=%.2fs build=%.2fs merge+sort=%.2fs save=%.2fs db=%llu bytes (%s)\r\n",
                   (GetTickCount64() - t) / 1000.0, st.rebuildSec, st.sortSec, st.saveSec, st.dbBytes, compress ? L"compressed" : L"raw");
        out += buf;
        if (d) {
            swprintf_s(buf, L"[mem] entries=%zu folders=%zu files=%zu name_chars=%zu index_bytes=%zu bytes/object=%.1f\r\n",
                       d->entries.size(), d->folders, d->files, d->names.size(), d->MemoryBytes(), double(d->MemoryBytes()) / d->entries.size());
            out += buf;
        }
        if (!st.note.empty()) out += L"[note] " + st.note + L"\r\n";
    }
    {   // 热加载
        Indexer ix; o.forceRebuild = false; o.updateAfterLoad = false;
        ix.Start(nullptr, o); waitIdle(ix);
        auto st = ix.Stats(); auto d = ix.Snapshot();
        swprintf_s(buf, L"[warm] load=%.3fs ready=%d\r\n", st.loadSec, d ? 1 : 0);
        out += buf;
        if (d) {
            auto terms = ParseTerms(argv[3]);
            std::vector<uint32_t> res; double best = 1e9, sum = 0;
            for (int k = 0; k < 7; ++k) {
                LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
                SearchCore(*d, terms, res, [] { return false; });
                QueryPerformanceCounter(&b);
                double ms = (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
                best = std::min(best, ms); if (k) sum += ms;
            }
            swprintf_s(buf, L"[search] query=\"%s\" results=%zu best=%.2fms avg=%.2fms threads=%u\r\n",
                       argv[3], res.size(), best, sum / 6, std::thread::hardware_concurrency());
            out += buf;
            for (size_t k = 0; k < res.size() && k < 5; ++k) out += L"  " + d->FullPath(res[k]) + L"\r\n";
        }
    }
    PROCESS_MEMORY_COUNTERS_EX pmc{sizeof(pmc)};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    swprintf_s(buf, L"[proc] peak_working_set=%zu MB\r\n", pmc.PeakWorkingSetSize >> 20);
    out += buf;
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

    InitFold();
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
