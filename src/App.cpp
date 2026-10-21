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
static void WriteOut(const std::wstring& name, const std::wstring& out) {
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::wstring path = std::wstring(tmp) + name;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD wr; WriteFile(f, "\xFF\xFE", 2, &wr, nullptr);
    WriteFile(f, out.data(), static_cast<DWORD>(out.size() * 2), &wr, nullptr);
    CloseHandle(f);
}

static bool HasName(const IndexData& d, const wchar_t* n) {
    for (uint32_t i : d.byName) if (d.Name(i) == n) return true;
    return false;
}

static void Touch(const std::wstring& p, const char* data) {
    HANDLE f = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD wr; WriteFile(f, data, static_cast<DWORD>(strlen(data)), &wr, nullptr); CloseHandle(f);
}

// -usntest G: ：中完整性下验证 USN 增量路径（种子库用带 FileId 的枚举建立，仅自检允许）
static int UsnTest(const std::wstring& vol) {
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::wstring db = std::wstring(tmp) + L"ff_usntest.db";
    DeleteFileW(db.c_str());
    std::wstring out; wchar_t buf[1024]; int fails = 0;
    auto waitIdle = [](Indexer& ix) { while (ix.Busy()) Sleep(10); };
    auto check = [&](bool ok, const wchar_t* what) { out += (ok ? L"  [OK]   " : L"  [FAIL] "); out += what; out += L"\r\n"; if (!ok) ++fails; };
    std::wstring dir = vol + L"\\ff_usn_test";
    Indexer::Options o; o.roots = {vol}; o.dbPath = db; o.monitor = false; o.seedTraverse = {vol}; o.excludes = {};
    {
        Indexer ix; o.forceRebuild = true; ix.Start(nullptr, o); waitIdle(ix);
        auto d = ix.Snapshot(); auto st = ix.Stats();
        swprintf_s(buf, L"[seed] entries=%zu jid=%016I64X next=%I64d save=%d\r\n", d ? d->entries.size() : 0,
                   d && !d->volumes.empty() ? d->volumes[0].journalId : 0, d && !d->volumes.empty() ? d->volumes[0].nextUsn : 0, st.fullSave);
        out += buf;
        check(d && !d->volumes.empty() && d->volumes[0].journalId != 0, L"种子库带 Journal ID");
    }
    o.seedTraverse.clear(); o.forceRebuild = false;
    // 文件系统变化：新建目录/文件、重命名、删除、修改
    CreateDirectoryW(dir.c_str(), nullptr);
    Touch(dir + L"\\usn_a.txt", "a");
    Touch(dir + L"\\usn_b.txt", "bb");
    Touch(dir + L"\\usn_keep.txt", "k");
    MoveFileW((dir + L"\\usn_b.txt").c_str(), (dir + L"\\usn_renamed_中文.txt").c_str());
    DeleteFileW((dir + L"\\usn_a.txt").c_str());
    Touch(dir + L"\\usn_keep.txt", "kkkkkkkkkk");
    {
        Indexer ix; auto t = GetTickCount64(); ix.Start(nullptr, o); waitIdle(ix);
        auto d = ix.Snapshot(); auto st = ix.Stats();
        swprintf_s(buf, L"[usn1] total=%.3fs load=%.3fs update=%.3fs changes=%zu unresolved=%zu fullSave=%d headerOnly=%d rebuild=%.2f\r\n",
                   (GetTickCount64() - t) / 1000.0, st.loadSec, st.updateSec, st.usnChanges, st.usnUnresolved, st.fullSave, st.headerOnlyUpdate, st.rebuildSec);
        out += buf;
        if (!st.note.empty()) out += L"  [note] " + st.note + L"\r\n";
        check(st.loadedFromDb && st.rebuildSec < 0, L"启动只加载数据库，没有重扫");
        check(d && HasName(*d, L"ff_usn_test"), L"新目录 ff_usn_test 出现");
        check(d && HasName(*d, L"usn_renamed_中文.txt"), L"重命名后的 usn_renamed_中文.txt 出现");
        check(d && !HasName(*d, L"usn_b.txt"), L"旧名 usn_b.txt 消失");
        check(d && !HasName(*d, L"usn_a.txt"), L"已删除 usn_a.txt 消失");
        bool sizeOk = false;
        if (d) for (uint32_t i : d->byName) if (d->Name(i) == L"usn_keep.txt") { sizeOk = d->entries[i].size == 10; out += L"  path=" + d->FullPath(i) + L"\r\n"; }
        check(sizeOk, L"usn_keep.txt 大小更新为 10");
        check(st.fullSave, L"记录有变化 -> 整库保存");
    }
    {   // 没有任何变化：既不合并也不写文件
        Indexer ix; ix.Start(nullptr, o); waitIdle(ix);
        auto st = ix.Stats();
        swprintf_s(buf, L"[usn2] load=%.3fs update=%.3fs changes=%zu fullSave=%d headerOnly=%d\r\n", st.loadSec, st.updateSec, st.usnChanges, st.fullSave, st.headerOnlyUpdate);
        out += buf;
        check(!st.fullSave && !st.headerOnlyUpdate && st.rebuildSec < 0, L"无变化 -> 不保存、不重扫");
    }
    // 只有不影响索引的日志记录（新建后立即删除）：游标前进，只原地更新卷表
    Touch(dir + L"\\usn_tmp.txt", "t"); DeleteFileW((dir + L"\\usn_tmp.txt").c_str());
    {
        WIN32_FILE_ATTRIBUTE_DATA before{}; GetFileAttributesExW(db.c_str(), GetFileExInfoStandard, &before);
        Indexer ix; ix.Start(nullptr, o); waitIdle(ix);
        auto st = ix.Stats();
        WIN32_FILE_ATTRIBUTE_DATA after{}; GetFileAttributesExW(db.c_str(), GetFileExInfoStandard, &after);
        swprintf_s(buf, L"[usn3] changes=%zu fullSave=%d headerOnly=%d save=%.4fs\r\n", st.usnChanges, st.fullSave, st.headerOnlyUpdate, st.saveSec);
        out += buf;
        check(!st.fullSave && st.headerOnlyUpdate, L"游标前进但无记录变化 -> 只更新头部");
        check(memcmp(&before.ftCreationTime, &after.ftCreationTime, sizeof(FILETIME)) == 0, L"数据库文件未被替换（创建时间不变）");
    }
    {   // 头部原地更新后能再次加载，且游标已前进
        Indexer ix; ix.Start(nullptr, o); waitIdle(ix);
        auto st = ix.Stats(); auto d = ix.Snapshot();
        check(st.loadedFromDb && !st.fullSave && !st.headerOnlyUpdate && d && HasName(*d, L"usn_renamed_中文.txt"), L"更新头部后的数据库可加载，且无重复变化");
    }
    {   // 监视：运行中的变化被应用
        Indexer ix; auto om = o; om.monitor = true; ix.Start(nullptr, om);
        while (ix.Busy()) Sleep(10);
        Touch(dir + L"\\usn_live.txt", "live");
        bool seen = false;
        for (int k = 0; k < 100 && !seen; ++k) { Sleep(100); auto d = ix.Snapshot(); seen = d && HasName(*d, L"usn_live.txt"); }
        check(seen, L"监视线程 10 秒内应用了 usn_live.txt");
        ix.Stop();
    }
    // 清理
    for (auto n : {L"usn_keep.txt", L"usn_renamed_中文.txt", L"usn_live.txt"}) DeleteFileW((dir + L"\\" + n).c_str());
    RemoveDirectoryW(dir.c_str());
    {   // 删除也能被应用
        Indexer ix; ix.Start(nullptr, o); waitIdle(ix);
        auto d = ix.Snapshot();
        check(d && !HasName(*d, L"ff_usn_test") && !HasName(*d, L"usn_keep.txt"), L"清理后的删除被应用");
    }
    swprintf_s(buf, L"[result] %s (%d failed)\r\n", fails ? L"FAIL" : L"PASS", fails);
    out += buf;
    WriteOut(L"ff_usntest.txt", out);
    return fails ? 1 : 0;
}

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
    if (wcsstr(GetCommandLineW(), L"-usntest")) {
        InitFold();
        int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        std::wstring v = argc >= 3 ? argv[2] : L"G:";
        LocalFree(argv);
        return UsnTest(v);
    }
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
