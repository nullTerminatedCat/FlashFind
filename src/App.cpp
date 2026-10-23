#include "App.h"
#include "MainWindow.h"
#include "Settings.h"
#include "Theme.h"
#include "ResultList.h"
#include <commctrl.h>
#include <objbase.h>
#include <ole2.h>
#include <csignal>
#include <exception>
#include <cstdio>
#include <shellapi.h>
#include <psapi.h>
#include <algorithm>
#include "Fold.h"
#include "Index.h"
#include "Search.h"
#include "Db.h"
#include "MftIndexer.h"
#include "Usn.h"
#include <winioctl.h>
#include <sddl.h>
#include "NtfsSource.h"
#include "Options.h"
#include "Hotkey.h"
#include "Log.h"
#include "../tests/OptimizationTests.h"

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
    // 同时输出到 stdout（ctest / CI 日志可见）
    HANDLE so = GetStdHandle(STD_OUTPUT_HANDLE);
    if (so && so != INVALID_HANDLE_VALUE) {
        int n = WideCharToMultiByte(CP_UTF8, 0, out.data(), static_cast<int>(out.size()), nullptr, 0, nullptr, nullptr);
        std::string u(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, out.data(), static_cast<int>(out.size()), u.data(), n, nullptr, nullptr);
        WriteFile(so, u.data(), static_cast<DWORD>(u.size()), &wr, nullptr);
    }
}

static size_t CountExact(const IndexData& d, std::wstring_view nm) {
    auto it = std::lower_bound(d.byName.begin(), d.byName.end(), nm, [&](uint32_t x, std::wstring_view n) { return CompareFold(d.Name(x), n) < 0; });
    size_t c = 0;
    for (; it != d.byName.end() && CompareFold(d.Name(*it), nm) == 0; ++it) if (d.Name(*it) == nm) ++c;
    return c;
}
static bool InByName(const IndexData& d, uint32_t id) {
    auto it = std::lower_bound(d.byName.begin(), d.byName.end(), id, [&](uint32_t a, uint32_t b) { int c = CompareFold(d.Name(a), d.Name(b)); return c ? c < 0 : a < b; });
    return it != d.byName.end() && *it == id;
}
static size_t SearchCount(const IndexData& d, const wchar_t* q) {
    std::vector<uint32_t> res; SearchCore(d, ParseTerms(q), res, [] { return false; }); return res.size();
}
static double NowMs() { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return c.QuadPart * 1000.0 / f.QuadPart; }

// 合成索引（没有现成数据库时）：n 个对象，约 1/8 为文件夹
static std::shared_ptr<IndexData> Synthesize(size_t n) {
    VolumeData v; v.info.root = L"S:"; v.info.ntfs = true;
    v.Add(L"S:", 2, kNoParent, kFlagDir, 0, 0);
    std::vector<uint32_t> dirs{0};
    uint64_t x = 88172645463325252ULL;
    wchar_t nm[64];
    for (size_t i = 1; i < n; ++i) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        bool dir = (x & 7) == 0;
        // 1/16 的对象挂在前 64 个文件夹下：形成数百~数千子项的大目录（与真实磁盘相似，便于选取测试目标）
        uint32_t parent = ((x >> 40) & 15) == 0 ? dirs[(x >> 8) % std::min<size_t>(dirs.size(), 64)] : dirs[(x >> 8) % dirs.size()];
        int len = swprintf_s(nm, L"%s_%llx%s", dir ? L"folder" : L"file", (unsigned long long)(x >> 20) & 0xFFFFFF, dir ? L"" : L".txt");
        uint32_t idx = v.Add(nm, len, parent, dir ? kFlagDir : 0, dir ? 0 : (x & 0xFFFFF), 0);
        if (dir) dirs.push_back(idx);
    }
    std::vector<VolumeData> vols; vols.push_back(std::move(v));
    return Merge(vols, 0);
}

// -incrtest [db]：在已加载（或合成）的大索引上测试 USN 增量插入/删除的正确性与耗时
static int IncrTest(std::wstring dbPath) {
    std::wstring out; wchar_t buf[2048]; int fails = 0;
    auto check = [&](bool ok, const wchar_t* what) { out += (ok ? L"  [OK]   " : L"  [FAIL] "); out += what; out += L"\r\n"; if (!ok) ++fails; };
    if (dbPath.empty()) dbPath = Indexer::DefaultDbPath();
    std::wstring err;
    double t0 = NowMs();
    std::shared_ptr<IndexData> cur = LoadDbCopy(dbPath, err);
    double loadMs = NowMs() - t0;
    if (!cur) { out += L"[load] 数据库不可用（" + err + L"），改用合成索引\r\n"; t0 = NowMs(); cur = Synthesize(2260000); loadMs = NowMs() - t0; }
    const size_t n = cur->entries.size();
    swprintf_s(buf, L"[load] %s entries=%zu folders=%zu files=%zu version=%u time=%.0f ms\r\n", dbPath.c_str(), n, cur->folders, cur->files, cur->dbVersion, loadMs);
    out += buf;
    // 合成 FRN（遍历建立的旧库没有 FRN）
    if (cur->frn.size() != n) { cur->frn.resize(n); }
    for (size_t i = 0; i < n; ++i) cur->frn.Mut(i) = (1ULL << 48) | (i + 16);
    for (auto& v : cur->volumes) v.mft = true;
    {
        double a = NowMs(); std::vector<uint32_t> res; auto terms = ParseTerms(L"report");
        double best = 1e9; for (int k = 0; k < 5; ++k) { a = NowMs(); SearchCore(*cur, terms, res, [] { return false; }); best = std::min(best, NowMs() - a); }
        swprintf_s(buf, L"[search] report results=%zu best=%.2f ms（分块条目/名称池）\r\n", res.size(), best); out += buf;
    }
    std::shared_ptr<const IndexData> base = cur;
    const IndexData& B = *base;
    // 选取目标（卷 0）
    std::vector<uint32_t> direct(n, 0);
    for (size_t i = 0; i < n; ++i) { const Entry& e = B.entries[i]; if (!e.IsDeleted() && e.parent != kNoParent) ++direct[e.parent]; }
    uint32_t D0 = kNoParent, S = kNoParent, X = kNoParent;
    std::vector<uint32_t> files;
    for (uint32_t id : B.byName) {
        const Entry& e = B.entries[id];
        if (e.Vol() != 0 || e.parent == kNoParent) continue;
        if (e.IsDir()) {
            if (D0 == kNoParent && direct[id] >= 3 && direct[id] < 200) D0 = id;
            else if (S == kNoParent && direct[id] >= 300 && direct[id] < 30000 && id != D0) S = id;
            else if (X == kNoParent && direct[id] >= 2 && direct[id] < 100 && id != D0 && id != S) X = id;
        } else if (files.size() < 400000) files.push_back(id);
    }
    auto isUnder = [&](uint32_t i, uint32_t anc) { for (uint32_t p = B.entries[i].parent; p != kNoParent; p = B.entries[p].parent) if (p == anc) return true; return false; };
    // 改名/删除目标：不在 S、X 之下，取均匀分布的文件
    std::vector<uint32_t> pick;
    for (size_t k = 0; k < files.size() && pick.size() < 550; k += std::max<size_t>(1, files.size() / 6000))
        if (!isUnder(files[k], S) && !isUnder(files[k], X) && files[k] != D0) pick.push_back(files[k]);
    if (D0 == kNoParent || S == kNoParent || X == kNoParent || pick.size() < 550) { out += L"[FAIL] 找不到测试目标\r\n"; WriteOut(L"ff_incrtest.txt", out); return 1; }
    size_t subDirs = 0, subFiles = 0;
    for (size_t i = 0; i < n; ++i) if (!B.entries[i].IsDeleted() && isUnder(static_cast<uint32_t>(i), S)) (B.entries[i].IsDir() ? subDirs : subFiles)++;
    std::wstring pathX = B.FullPath(X), pathD0 = B.FullPath(D0), pathS = B.FullPath(S);
    swprintf_s(buf, L"[targets] D0=%s\r\n  S=%s（子树 %zu 文件夹 %zu 文件）\r\n  X(排除)=%s\r\n", pathD0.c_str(), pathS.c_str(), subDirs, subFiles, pathX.c_str());
    out += buf;

    // 构造 USN_RECORD_V2 记录流
    std::vector<UsnEvent> evs; int64_t usn = 1000; const int64_t now = 133500000000000000LL;
    auto ev = [&](uint64_t frn, uint64_t parent, uint32_t reason, uint32_t attrs, const std::wstring& name) {
        UsnEvent e; e.frn = frn; e.parent = parent; e.reason = reason; e.attrs = attrs; e.usn = usn; usn += 96; e.time = now; e.name = name; evs.push_back(e);
    };
    uint64_t nextFrn = (2ULL << 48) | (n + 1000);
    for (int k = 0; k < 1000; ++k) { swprintf_s(buf, L"ff_incr_%04d.txt", k); ev(nextFrn++, B.frn[D0], USN_REASON_FILE_CREATE | USN_REASON_CLOSE, FILE_ATTRIBUTE_ARCHIVE, buf); }
    uint64_t ndir = nextFrn++;
    ev(ndir, B.frn[D0], USN_REASON_FILE_CREATE | USN_REASON_CLOSE, FILE_ATTRIBUTE_DIRECTORY, L"ff_incr_dir");
    for (int k = 0; k < 100; ++k) { swprintf_s(buf, L"ff_incr_sub_%03d.log", k); ev(nextFrn++, ndir, USN_REASON_FILE_CREATE | USN_REASON_CLOSE, FILE_ATTRIBUTE_ARCHIVE, buf); }
    std::vector<uint32_t> renamed(pick.begin(), pick.begin() + 200), deleted(pick.begin() + 200, pick.begin() + 500), modified(pick.begin() + 500, pick.begin() + 550);
    for (size_t k = 0; k < renamed.size(); ++k) {
        uint32_t id = renamed[k];
        ev(B.frn[id], B.frn[B.entries[id].parent], USN_REASON_RENAME_OLD_NAME, FILE_ATTRIBUTE_ARCHIVE, std::wstring(B.Name(id)));
        swprintf_s(buf, L"ff_renamed_%03zu_中文.dat", k);
        ev(B.frn[id], B.frn[B.entries[id].parent], USN_REASON_RENAME_NEW_NAME | USN_REASON_CLOSE, FILE_ATTRIBUTE_ARCHIVE, buf);
    }
    for (uint32_t id : deleted) ev(B.frn[id], B.frn[B.entries[id].parent], USN_REASON_FILE_DELETE | USN_REASON_CLOSE, FILE_ATTRIBUTE_ARCHIVE, std::wstring(B.Name(id)));
    for (uint32_t id : modified) ev(B.frn[id], B.frn[B.entries[id].parent], USN_REASON_DATA_EXTEND | USN_REASON_CLOSE, FILE_ATTRIBUTE_ARCHIVE, std::wstring(B.Name(id)));
    ev(B.frn[S], B.frn[B.entries[S].parent], USN_REASON_FILE_DELETE | USN_REASON_CLOSE, FILE_ATTRIBUTE_DIRECTORY, std::wstring(B.Name(S)));   // 只有文件夹本身的删除记录
    ev(nextFrn++, B.frn[X], USN_REASON_FILE_CREATE | USN_REASON_CLOSE, FILE_ATTRIBUTE_ARCHIVE, L"ff_excluded_new.txt");                      // 父目录在排除列表中
    ev((9ULL << 48) | 77, B.frn[D0], USN_REASON_FILE_DELETE, FILE_ATTRIBUTE_DIRECTORY, L"ghost_dir");                                    // 不存在的文件夹
    ev(nextFrn++, B.frn[D0], USN_REASON_FILE_CREATE, FILE_ATTRIBUTE_ARCHIVE, L"ff_tmp_created_deleted.tmp");
    ev(nextFrn - 1, B.frn[D0], USN_REASON_FILE_DELETE | USN_REASON_CLOSE, FILE_ATTRIBUTE_ARCHIVE, L"ff_tmp_created_deleted.tmp");
    std::vector<uint8_t> raw;
    for (auto& e : evs) EncodeUsnRecord(raw, e);
    std::vector<UsnEvent> parsed; int budget = 0;
    double tp = NowMs();
    bool pok = ParseUsnRecords(raw.data(), raw.size(), parsed, err, budget);
    double parseMs = NowMs() - tp;
    swprintf_s(buf, L"[parse] ok=%d parsed=%zu events=%zu bytes=%zu err=%s\r\n", pok, parsed.size(), evs.size(), raw.size(), err.c_str()); out += buf;
    check(pok && parsed.size() == evs.size() && parsed.size() > 1000 && parsed[1000].name == L"ff_incr_dir" && parsed[1000].frn == evs[1000].frn, L"USN_RECORD_V2 解析（名称取自记录）");
    {   // 损坏记录
        std::vector<uint8_t> bad(raw.begin(), raw.begin() + raw.size() / evs.size() * 0 + reinterpret_cast<USN_RECORD_V2*>(raw.data())->RecordLength);
        auto* r = reinterpret_cast<USN_RECORD_V2*>(bad.data()); r->FileNameOffset = 8;
        std::vector<UsnEvent> tmp; std::wstring e2; int b2 = 0;
        check(!ParseUsnRecords(bad.data(), bad.size(), tmp, e2, b2) && e2.find(L"bad file name offset") == 0, L"bad file name offset 被拒绝");
        r->FileNameOffset = 60; r->RecordLength = 4096;
        e2.clear(); tmp.clear();
        check(!ParseUsnRecords(bad.data(), bad.size(), tmp, e2, b2) && e2.find(L"bad record length") == 0, L"bad record length 被拒绝");
    }

    LiveUpdater upd(base);
    upd.SetFilter(0, {pathX}, {});
    double ta = NowMs();
    // 修改时间来自元数据查询；USN TimeStamp 是事件时间，不能冒充文件最后写入时间。
    upd.Apply(0, parsed, [&](uint32_t, const std::vector<uint64_t>& frns, std::vector<LookupResult>& out) {
        out.assign(frns.size(), LookupResult{0, 1, 0, static_cast<uint64_t>(now)});
    });
    auto snap = upd.Commit();
    double totalMs = NowMs() - ta;
    auto s1 = upd.LastStats();
    swprintf_s(buf, L"[batch1] %zu 条记录：解析 %.2f ms；FRN 索引(首次) %.1f ms；应用 %.1f ms；合并 %.1f ms；合计 %.1f ms\r\n"
                    L"  新建 %zu 删除 %zu 改名 %zu 修改 %zu 丢弃 %zu 未找到 %zu；子树移除 %zu 文件夹 %zu 文件；byName 插入 %zu 移除 %zu；复制块 %zu（%.1f MB）\r\n",
               parsed.size(), parseMs, s1.frnIndexMs, s1.applyMs - s1.frnIndexMs, s1.commitMs, totalMs,
               s1.creates, s1.deletes, s1.renames, s1.modifies, s1.dropped, s1.notFound, s1.removedDirs, s1.removedFiles, s1.inserted, s1.removed,
               s1.chunkClones, s1.chunkClones * 65536.0 * 32 / 1048576.0);
    out += buf;
    const IndexData& W = *snap;
    // 结构校验
    size_t alive = 0, fo = 0, fi = 0; bool sorted = true, noDel = true;
    for (size_t i = 0; i < W.entries.size(); ++i) if (!W.entries[i].IsDeleted()) { ++alive; (W.entries[i].IsDir() ? fo : fi)++; }
    for (size_t k = 0; k < W.byName.size(); ++k) {
        if (W.entries[W.byName[k]].IsDeleted()) noDel = false;
        if (k) { int c = CompareFold(W.Name(W.byName[k - 1]), W.Name(W.byName[k])); if (c > 0 || (c == 0 && W.byName[k - 1] >= W.byName[k])) sorted = false; }
    }
    check(sorted, L"byName 仍然有序（逐项比较）");
    check(noDel && W.byName.size() == alive, L"byName 恰好包含全部存活对象");
    check(W.folders == fo && W.files == fi, L"文件夹/文件计数正确");
    check(SearchCount(W, L"ff_incr_") == 1101, L"新建 1000 + 文件夹 1 + 其中 100 个文件可搜索");
    bool renOk = true; for (size_t k = 0; k < renamed.size(); ++k) { swprintf_s(buf, L"ff_renamed_%03zu_中文.dat", k); if (W.Name(renamed[k]) != buf || !InByName(W, renamed[k])) renOk = false; }
    check(renOk && SearchCount(W, L"ff_renamed_") == 200, L"改名 200 个：新名可搜索、位置正确");
    bool delOk = true; for (uint32_t id : deleted) if (!W.entries[id].IsDeleted() || InByName(W, id)) delOk = false;
    check(delOk, L"删除 300 个：已从索引移除");
    bool subOk = s1.removedDirs == subDirs && s1.removedFiles == subFiles;
    for (size_t i = 0; i < W.entries.size() && subOk; ++i) if (!W.entries[i].IsDeleted() && isUnder(static_cast<uint32_t>(i), S)) subOk = false;
    check(subOk && W.entries[S].IsDeleted(), L"文件夹删除：内存中移除整棵子树");
    swprintf_s(buf, L"[subtree] 反向索引删除 %zu 文件夹 %zu 文件：%.2f ms（无全表扫描）\r\n", s1.removedDirs, s1.removedFiles, s1.subtreeMs); out += buf;
    check(SearchCount(W, L"ff_excluded_new") == 0 && s1.dropped >= 1, L"排除目录下的 CREATE 被丢弃");
    check(SearchCount(W, L"ff_tmp_created_deleted") == 0, L"同批新建后删除：不出现");
    bool modOk = true; for (uint32_t id : modified) if (W.entries[id].mtime != static_cast<uint64_t>(now)) modOk = false;
    check(modOk, L"MODIFY 更新修改时间");
    check(SearchCount(B, L"ff_incr_") == 0 && B.byName.size() == base->byName.size() && CountExact(B, B.Name(S)) >= 1 && !B.entries[S].IsDeleted(), L"旧快照未被修改（写时复制）");

    {   // 第二批：典型监视批次（FRN 索引已建立）
        evs.clear();
        for (int k = 0; k < 100; ++k) { swprintf_s(buf, L"ff_batch2_%03d.txt", k); ev(nextFrn++, W.frn[D0], USN_REASON_FILE_CREATE | USN_REASON_CLOSE, FILE_ATTRIBUTE_ARCHIVE, buf); }
        for (int k = 0; k < 100; ++k) { uint32_t id = static_cast<uint32_t>(n + k); ev(W.frn[id], W.frn[D0], USN_REASON_FILE_DELETE | USN_REASON_CLOSE, FILE_ATTRIBUTE_ARCHIVE, std::wstring(W.Name(id))); }
        upd.ResetStats();
        double tb = NowMs();
        upd.Apply(0, evs, nullptr);
        auto snap2 = upd.Commit();
        double ms2 = NowMs() - tb;
        auto s2 = upd.LastStats();
        swprintf_s(buf, L"[batch2] 100 新建 + 100 删除：应用 %.2f ms，合并 %.2f ms，合计 %.2f ms；复制块 %zu\r\n", s2.applyMs, s2.commitMs, ms2, s2.chunkClones);
        out += buf;
        check(SearchCount(*snap2, L"ff_batch2_") == 100 && SearchCount(*snap2, L"ff_incr_") == 1001, L"第二批结果正确");
        // 单条（1 个新建）
        evs.clear(); ev(nextFrn++, W.frn[D0], USN_REASON_FILE_CREATE | USN_REASON_CLOSE, FILE_ATTRIBUTE_ARCHIVE, L"ff_single.txt");
        upd.ResetStats(); tb = NowMs(); upd.Apply(0, evs, nullptr); auto snap3 = upd.Commit(); ms2 = NowMs() - tb;
        swprintf_s(buf, L"[batch3] 1 个新建：合计 %.2f ms\r\n", ms2); out += buf;
        check(CountExact(*snap3, L"ff_single.txt") == 1, L"单条新建正确");
        snap = snap3;
        {   // 移入的文件夹（从索引范围外改名进来）：立即扫描并插入子项
            const uint64_t mv = (3ULL << 48) | 4242;
            upd.SetScanner([&](uint32_t, const std::wstring& path, VolumeData& vd) {
                auto add = [&](const std::wstring& nm, uint32_t parent, bool dir, uint64_t frn) {
                    Entry e; e.nameOff = static_cast<uint32_t>(vd.names.size()); e.nameLen = static_cast<uint16_t>(nm.size());
                    e.flags = dir ? kFlagDir : 0; e.parent = parent; e.size = 123; e.mtime = now;
                    vd.names.insert(vd.names.end(), nm.begin(), nm.end()); vd.entries.push_back(e); vd.frn.push_back(frn);
                };
                add(path.substr(path.rfind(L'\\') + 1), kNoParent, true, mv);
                for (int k = 0; k < 500; ++k) { swprintf_s(buf, L"ff_movedin_%03d.bin", k); add(buf, 0, false, mv + 1 + k); }
                for (int k = 0; k < 20; ++k) { swprintf_s(buf, L"ff_movedin_sub%02d", k); add(buf, 0, true, mv + 1000 + k); }
                for (int k = 0; k < 20; ++k) { swprintf_s(buf, L"ff_movedin_deep_%02d.txt", k); add(buf, 501 + k, false, mv + 2000 + k); }
                return true;
            });
            evs.clear();
            ev(mv, snap->frn[D0], USN_REASON_RENAME_NEW_NAME | USN_REASON_CLOSE, FILE_ATTRIBUTE_DIRECTORY, L"ff_movedin_root");
            upd.ResetStats(); tb = NowMs(); upd.Apply(0, evs, nullptr); auto s5 = upd.Commit(); ms2 = NowMs() - tb;
            auto st5 = upd.LastStats();
            swprintf_s(buf, L"[movedin] 移入文件夹扫描：%zu 次扫描，readd files %zu folders %zu，扫描插入 %.2f ms，合计 %.2f ms\r\n", st5.scans, st5.readdFiles, st5.readdFolders, st5.scanMs, ms2); out += buf;
            check(st5.readdFiles == 520 && st5.readdFolders == 20 && SearchCount(*s5, L"ff_movedin_") == 541, L"移入文件夹：子项立即加入索引");
            // 再把它删掉：通过反向索引删除整棵子树
            evs.clear();
            ev(mv, snap->frn[D0], USN_REASON_FILE_DELETE | USN_REASON_CLOSE, FILE_ATTRIBUTE_DIRECTORY, L"ff_movedin_root");
            upd.ResetStats(); upd.Apply(0, evs, nullptr); auto s6 = upd.Commit();
            check(SearchCount(*s6, L"ff_movedin_") == 0 && upd.LastStats().removedFiles == 520, L"移入后删除：反向索引移除子树");
            snap = s6;
        }
        {   // 幽灵压缩（不重读 MFT、不重新排序）
            size_t ghosts = snap->entries.size() - snap->folders - snap->files, cleared = 0;
            std::vector<std::wstring> before; for (uint32_t id : snap->byName) if (before.size() < 200000) before.emplace_back(snap->Name(id));
            size_t cntBefore = SearchCount(*snap, L"report"), b2 = SearchCount(*snap, L"ff_renamed_");
            double tg = NowMs(); auto g = ClearGhosts(*snap, &cleared); double gms = NowMs() - tg;
            bool same = g->byName.size() == snap->byName.size() && g->entries.size() == snap->entries.size() - ghosts;
            for (size_t k = 0; k < before.size() && same; ++k) if (g->Name(g->byName[k]) != before[k]) same = false;
            bool parentsOk = true; for (size_t i = 0; i < g->entries.size(); ++i) { auto pp = g->entries[i].parent; if (pp != kNoParent && (pp >= g->entries.size() || !g->entries[pp].IsDir())) { parentsOk = false; break; } }
            swprintf_s(buf, L"[deleteme] 清除 %zu 个幽灵条目（共 %zu）：%.0f ms\r\n", cleared, snap->entries.size(), gms); out += buf;
            check(cleared == ghosts && same && parentsOk && SearchCount(*g, L"report") == cntBefore && SearchCount(*g, L"ff_renamed_") == b2, L"幽灵压缩：顺序保持、父链正确、搜索结果不变");
            LiveUpdater u3(g); evs.clear();
            uint32_t d0n = kNoParent; for (uint32_t id : g->byName) if (g->FullPath(id) == pathD0) { d0n = id; break; }
            if (d0n != kNoParent) {
                ev(nextFrn++, g->frn[d0n], USN_REASON_FILE_CREATE | USN_REASON_CLOSE, FILE_ATTRIBUTE_ARCHIVE, L"ff_after_ghost.txt");
                u3.Apply(0, evs, nullptr); auto s7 = u3.Commit();
                check(CountExact(*s7, L"ff_after_ghost.txt") == 1, L"压缩后 Rebase：FRN 索引可用");
            } else check(false, L"压缩后找不到 D0");
            snap = g;
        }
        {   // 批量日志：一批一次写文件
            double tl = NowMs();
            { LogBatch lb; for (int k = 0; k < 1000; ++k) Log(L"logbatch test line %d", k); }
            swprintf_s(buf, L"[logbatch] 1000 行一次写入：%.2f ms\r\n", NowMs() - tl); out += buf;
        }
    }
    {   // 仅包含列表
        LiveUpdater u2(snap);
        u2.SetFilter(0, {}, {pathD0});
        evs.clear();
        ev(nextFrn++, snap->frn[0], USN_REASON_FILE_CREATE, FILE_ATTRIBUTE_ARCHIVE, L"ff_inc_root.txt");
        uint32_t d0c = D0; for (uint32_t id : snap->byName) if (snap->FullPath(id) == pathD0) { d0c = id; break; }   // 压缩后下标已变
        ev(nextFrn++, snap->frn[d0c], USN_REASON_FILE_CREATE, FILE_ATTRIBUTE_ARCHIVE, L"ff_inc_inside.txt");
        u2.Apply(0, evs, nullptr);
        auto s4 = u2.Commit();
        check(s4 && SearchCount(*s4, L"ff_inc_inside") == 1 && SearchCount(*s4, L"ff_inc_root") == 0, L"ntfs_volume_include_onlys：范围外的 CREATE 被丢弃");
    }
    {   // 对比：旧做法（提取各卷 + 合并 + 整体排序）
        double tm = NowMs();
        std::vector<VolumeData> vols;
        for (uint32_t vi = 0; vi < snap->volumes.size(); ++vi) vols.push_back(ExtractVolume(*snap, vi));
        auto full = Merge(vols, 0);
        swprintf_s(buf, L"[compare] 旧做法 提取+合并+整体排序 %.0f ms（%zu 个对象）\r\n", NowMs() - tm, full->entries.size());
        out += buf;
    }
    {   // v4 保存/加载往返
        wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
        std::wstring db = std::wstring(tmp) + L"ff_incrtest.db";
        uint64_t bytes = 0; double ts = NowMs();
        bool ok = SaveDb(db, *snap, false, &bytes); double saveMs = NowMs() - ts;
        ts = NowMs(); auto back = LoadDb(db, err); double lMs = NowMs() - ts;
        swprintf_s(buf, L"[db v4] 保存 %.0f ms（%llu 字节），加载 %.0f ms\r\n", saveMs, bytes, lMs); out += buf;
        check(ok && back && back->byName == snap->byName && back->entries.size() == snap->entries.size() && SearchCount(*back, L"ff_batch2_") == 100, L"v4 数据库往返一致");
        DeleteFileW(db.c_str());
        if (back) {
            double a, best = 1e9; std::vector<uint32_t> res; auto terms = ParseTerms(L"report");
            for (int k = 0; k < 5; ++k) { a = NowMs(); SearchCore(*back, terms, res, [] { return false; }); best = std::min(best, NowMs() - a); }
            swprintf_s(buf, L"[search] 加载后 report results=%zu best=%.2f ms\r\n", res.size(), best); out += buf;
        }
    }
    PROCESS_MEMORY_COUNTERS_EX pmc{sizeof(pmc)};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    swprintf_s(buf, L"[proc] peak_working_set=%zu MB\r\n[result] %s (%d failed)\r\n", pmc.PeakWorkingSetSize >> 20, fails ? L"FAIL" : L"PASS", fails);
    out += buf;
    WriteOut(L"ff_incrtest.txt", out);
    return fails ? 1 : 0;
}

// -initest：每卷设置的 ini 并行数组往返 + 过滤规则
static int IniTest() {
    std::wstring out; int fails = 0;
    auto check = [&](bool ok, const wchar_t* what) { out += (ok ? L"  [OK]   " : L"  [FAIL] "); out += what; out += L"\r\n"; if (!ok) ++fails; };
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::wstring ini = std::wstring(tmp) + L"ff_initest.ini";
    DeleteFileW(ini.c_str());
    Settings a;
    VolumeConfig c; c.path = L"C:"; c.guid = L"\\\\?\\Volume{11111111-2222-3333-4444-555555555555}\\";
    VolumeConfig d; d.path = L"D:"; d.include = false; d.monitor = false; d.loadRecentChanges = true; d.enableUsnJournal = false;
    d.includeOnly = {L"D:\\a,b", L"D:\\工作\\\"q\""};
    VolumeConfig g; g.path = L"G:"; g.enableUsnJournal = true; g.loadRecentChanges = false;   // Journal 开、最近更改关
    a.ntfsVolumes = {c, d, g};
    a.SaveTo(ini);
    Settings b; b.LoadFrom(ini, false);
    bool same = b.ntfsVolumes.size() == 3;
    for (size_t i = 0; same && i < 3; ++i) {
        auto& x = a.ntfsVolumes[i]; auto& y = b.ntfsVolumes[i];
        same = x.path == y.path && x.guid == y.guid && x.include == y.include && x.monitor == y.monitor &&
               x.loadRecentChanges == y.loadRecentChanges && x.enableUsnJournal == y.enableUsnJournal && x.includeOnly == y.includeOnly;
    }
    check(same, L"7 个每卷并行数组写入后读回一致（含逗号/引号/中文）");
    check(b.ntfsVolumes.size() == 3 && b.ntfsVolumes[2].enableUsnJournal && !b.ntfsVolumes[2].loadRecentChanges, L"Journal 开启而 load_recent_changes 关闭可以共存");
    // ini 原文
    HANDLE f = CreateFileW(ini.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        std::string raw(65536, 0); DWORD rd = 0; ReadFile(f, raw.data(), 65535, &rd, nullptr); raw.resize(rd); CloseHandle(f);
        std::wstring w;
        if (rd >= 2 && (uint8_t)raw[0] == 0xFF) w.assign(reinterpret_cast<const wchar_t*>(raw.data() + 2), (rd - 2) / 2);
        else { int n = MultiByteToWideChar(CP_ACP, 0, raw.data(), rd, nullptr, 0); w.resize(n); MultiByteToWideChar(CP_ACP, 0, raw.data(), rd, w.data(), n); }
        for (size_t p = 0; (p = w.find(L"ntfs_volume", p)) != std::wstring::npos;) { size_t e = w.find(L'\n', p); out += L"    " + w.substr(p, e - p) + L"\n"; p = e; }
    }
    // 只有排除列表变化才触发重建
    std::vector<std::wstring> ex = {L"C:\\Windows\\WinSxS"};
    auto vols = a.ntfsVolumes;
    uint64_t h0 = ConfigHash(ex, vols);
    auto vols2 = vols; VolumeConfig e; e.path = L"E:"; vols2.push_back(e);
    check(ConfigHash(ex, vols2) == h0, L"新增卷不改变排除哈希（按卷处理，不全量重建）");
    auto ex2 = ex; ex2.push_back(L"C:\\Temp");
    check(ConfigHash(ex2, vols) != h0, L"排除列表变化 -> rebuild: excludes changed");
    auto vols3 = vols; vols3[0].includeOnly = {L"C:\\work"};
    check(ConfigHash(ex, vols3) != h0, L"仅包含列表变化 -> 重建");
    // 路径过滤
    std::vector<std::wstring> inc = {L"C:\\work\\src"};
    check(PathFiltered(L"C:\\Windows\\WinSxS\\x.dll", false, ex, {}) && !PathFiltered(L"C:\\Windows\\WinSxS", true, ex, {}), L"排除目录：内容过滤，目录本身保留");
    check(!PathFiltered(L"C:\\Windows\\WinSxSabc\\a", false, ex, {}), L"前缀相同但不是子目录不被排除");
    check(PathFiltered(L"C:\\other.txt", false, {}, inc) && !PathFiltered(L"C:\\work", true, {}, inc) && !PathFiltered(L"C:\\WORK\\SRC\\a.c", false, {}, inc), L"仅包含：祖先文件夹保留，范围外丢弃（不区分大小写）");
    DeleteFileW(ini.c_str());
    out += fails ? L"[result] FAIL\r\n" : L"[result] PASS\r\n";
    {   // 热键字符串往返（选项对话框）
        bool ok = true;
        for (const wchar_t* k : {L"Ctrl+Alt+Space", L"Ctrl+Shift+F", L"Alt+F12", L"Ctrl+`", L"Ctrl+Alt+Shift+Win+Tab", L"Ctrl+9"}) {
            HotkeyDef hd = ParseHotkey(k); if (FormatHotkey(hd.mods, hd.vk) != k) ok = false;
        }
        check(ok, L"热键 Parse/Format 往返");
    }
    WriteOut(L"ff_initest.txt", out);
    return fails ? 1 : 0;
}

// -gtest <卷>：中完整性真实卷：启动不重扫、NTFS 不遍历、提示需要服务/管理员
static int GTest(const std::wstring& vol) {
    std::wstring out; wchar_t buf[1024]; int fails = 0;
    auto check = [&](bool ok, const wchar_t* what) { out += (ok ? L"  [OK]   " : L"  [FAIL] "); out += what; out += L"\r\n"; if (!ok) ++fails; };
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::wstring db = std::wstring(tmp) + L"ff_gtest.db";
    DeleteFileW(db.c_str());
    Indexer::Options o; o.roots = {vol}; o.dbPath = db; o.monitor = true;
    auto waitIdle = [](Indexer& ix) { while (ix.Busy()) Sleep(10); };
    {
        Indexer ix; ix.Start(nullptr, o); waitIdle(ix);
        auto st = ix.Stats(); auto d = ix.Snapshot();
        swprintf_s(buf, L"[fresh] entries=%zu rebuild=%.3fs note=%s\r\n", d ? d->entries.size() : 0, st.rebuildSec, st.note.c_str()); out += buf;
        check(d && d->entries.size() == 1 && !st.note.empty(), MftIndexer::IsElevated() ? L"（已提升）" : L"未提升：NTFS 卷不遍历，只有卷根并提示服务/管理员");
        ix.Stop();
    }
    {
        Indexer ix; ix.Start(nullptr, o); waitIdle(ix);
        auto st = ix.Stats();
        swprintf_s(buf, L"[restart] loaded=%d load=%.4fs rebuild=%.2f fullSave=%d\r\n", st.loadedFromDb, st.loadSec, st.rebuildSec, st.fullSave); out += buf;
        check(st.loadedFromDb && st.rebuildSec < 0, L"重启只加载数据库，没有重扫");
        ix.Stop();
    }
    DeleteFileW(db.c_str());
    out += fails ? L"[result] FAIL\r\n" : L"[result] PASS\r\n";
    WriteOut(L"ff_gtest.txt", out);
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
                       d->entries.size(), d->folders, d->files, d->names.Used(), d->MemoryBytes(), double(d->MemoryBytes()) / d->entries.size());
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

// -wildtest [db]：通配符单元测试 + 大索引上的耗时
static int WildTest(std::wstring dbPath) {
    std::wstring out; wchar_t buf[1024]; int fails = 0;
    auto T = [&](const wchar_t* name, const wchar_t* q, bool want) {
        auto terms = ParseTerms(q);
        bool got = !terms.empty() && WildcardMatch(name, terms[0]);
        swprintf_s(buf, L"  %s %-14s ~ %-22s => %d\r\n", got == want ? L"[OK]  " : L"[FAIL]", q, name, got); out += buf;
        if (got != want) ++fails;
    };
    out += L"[unit]\r\n";
    T(L"1.exe", L"1*.exe", true);  T(L"123.EXE", L"1*.exe", true);  T(L"a1.exe", L"1*.exe", false);  T(L"1.exe.bak", L"1*.exe", false);
    T(L"notepad.exe", L"*.exe", true); T(L"exe", L"*.exe", false); T(L".exe", L"*.exe", true); T(L"x.exe.txt", L"*.exe", false);
    T(L"abc", L"a?c", true); T(L"aXc", L"A?C", true); T(L"ac", L"a?c", false); T(L"abbc", L"a?c", false);
    T(L"anything", L"**", true); T(L"", L"*", true); T(L"report", L"rep*", true); T(L"xreport", L"rep*", false);
    T(L"report.docx", L"*port*", true); T(L"report", L"report", true); T(L"my report.doc", L"report", true);
    T(L"aaaaaaaaaaaaaaaaaaaaaaaaaaaaab", L"*a*a*a*a*a*b", true); T(L"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", L"*a*a*a*a*a*b", false);
    T(L"ab.c.d", L"a*.?", true); T(L"中文文件.TXT", L"中*.txt", true); T(L"Kelvin\u212A.txt", L"kelvin?.txt", true);
    T(L"C:\\Windows\\notepad.exe", L"c:\\windows\\*.exe", true); T(L"C:\\Windows\\System32\\a.exe", L"c:\\windows\\?.exe", false);
    InitFold(); g_fastAscii = true;
    std::wstring err; if (dbPath.empty()) dbPath = Indexer::DefaultDbPath();
    auto d = LoadDbCopy(dbPath, err);
    if (!d) { out += L"[db] 无数据库，改用合成索引\r\n"; d = Synthesize(2260000); }
    swprintf_s(buf, L"[db] %zu 个对象，%u 线程\r\n", d->entries.size(), std::max(1u, std::min(std::thread::hardware_concurrency(), 16u))); out += buf;
    for (const wchar_t* q : {L"exe", L".exe", L"1*.exe", L"*.exe", L"*.EXE", L"a?c", L"**", L"*a*e*.?x?", L"report *.docx", L"win*\\*.dll"}) {
        auto terms = ParseTerms(q); std::vector<uint32_t> res; double best = 1e9, sum = 0;
        for (int k = 0; k < 7; ++k) { double a = NowMs(); SearchCore(*d, terms, res, [] { return false; }); double t = NowMs() - a; best = std::min(best, t); sum += t; }
        swprintf_s(buf, L"[search] %-14s results=%-8zu best=%6.2f ms avg=%6.2f ms\r\n", q, res.size(), best, sum / 7); out += buf;
        if (std::wstring(q) == L"1*.exe") {   // 交叉验证：与逐项 GlobMatch 一致
            size_t brute = 0; for (uint32_t id : d->byName) if (WildcardMatch(d->Name(id), terms[0])) ++brute;
            if (brute != res.size()) { ++fails; out += L"  [FAIL] 1*.exe 结果数与逐项匹配不一致\r\n"; }
        }
    }
    swprintf_s(buf, L"[result] %s (%d failed)\r\n", fails ? L"FAIL" : L"PASS", fails); out += buf;
    WriteOut(L"ff_wildtest.txt", out);
    return fails ? 1 : 0;
}

// -sorttest [db]：列排序正确性与耗时
static int SortTest(std::wstring dbPath) {
    std::wstring out; wchar_t buf[1024]; int fails = 0;
    std::wstring err; if (dbPath.empty()) dbPath = Indexer::DefaultDbPath();
    auto d = LoadDbCopy(dbPath, err);
    if (!d) { out += L"[db] 无数据库，改用合成索引\r\n"; d = Synthesize(2260000); }
    for (size_t i = 0; i < d->entries.size(); i += 7) d->entries.Mut(i).mtime = 133000000000000000ULL + (i * 2654435761ULL) % 100000000000ULL;   // 合成库时间全 0：打散
    swprintf_s(buf, L"[db] %zu 个对象\r\n", d->entries.size()); out += buf;
    double t0 = NowMs(); auto rank = BuildFolderPathRank(*d); double rankMs = NowMs() - t0;
    swprintf_s(buf, L"[path-rank] 文件夹路径先序排名 %.1f ms\r\n", rankMs); out += buf;
    const wchar_t* cn[] = {L"名称", L"路径", L"扩展名", L"大小", L"时间"};
    for (const wchar_t* q : {L"", L"exe", L"*.dll"}) {
        std::vector<uint32_t> base; SearchCore(*d, ParseTerms(q), base, [] { return false; });
        for (int c = 0; c < 5; ++c) for (int desc = 0; desc < 2; ++desc) {
            SortSpec so{static_cast<SortCol>(c), desc != 0};
            std::vector<uint32_t> it; double best = 1e9;
            for (int k = 0; k < 3; ++k) { it = base; double a = NowMs(); SortItems(*d, it, so, &rank); best = std::min(best, NowMs() - a); }
            bool ok = it.size() == base.size();
            auto key = [&](uint32_t i) -> uint64_t { const Entry& e = d->entries[i];
                if (c == 3) return e.IsDir() ? 0 : e.size + 1;
                if (c == 4) return e.mtime;
                if (c == 2) { // Ext — 与 SortItems 相同的简易键
                    auto nm = d->Name(i); size_t dot = e.IsDir() ? std::wstring_view::npos : nm.rfind(L'.');
                    uint64_t h = 0; if (dot != std::wstring_view::npos) for (size_t k = dot + 1; k < nm.size() && k < dot + 9; ++k) h = (h << 8) | (Fold(nm[k]) & 0xFF);
                    return h;
                }
                return e.parent == kNoParent ? 0 : rank[e.parent]; };
            for (size_t k = 1; k < it.size() && ok && c > 0; ++k) { uint64_t a = key(it[k - 1]), b = key(it[k]); if (desc ? a < b : a > b) ok = false; }
            if (c == 0) ok = ok && (desc ? std::equal(it.begin(), it.end(), base.rbegin()) : it == base);
            swprintf_s(buf, L"  %s %-6s %s %s results=%-8zu sort=%7.2f ms\r\n", ok ? L"[OK]  " : L"[FAIL]", q[0] ? q : L"(全部)", cn[c], desc ? L"降" : L"升", it.size(), best); out += buf;
            if (!ok) ++fails;
        }
    }
    swprintf_s(buf, L"[result] %s (%d failed)\r\n", fails ? L"FAIL" : L"PASS", fails); out += buf;
    WriteOut(L"ff_sorttest.txt", out);
    return fails ? 1 : 0;
}

// -syntaxtest [db]：搜索语法单元测试 + 大索引耗时
static uint64_t FtLocal(int y, int m, int d) {
    SYSTEMTIME lt{}; lt.wYear = (WORD)y; lt.wMonth = (WORD)m; lt.wDay = (WORD)d; lt.wHour = 12; SYSTEMTIME ut; FILETIME ft;
    TzSpecificLocalTimeToSystemTime(nullptr, &lt, &ut); SystemTimeToFileTime(&ut, &ft);
    return (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}
static int SyntaxTest(std::wstring dbPath) {
    std::wstring out; wchar_t buf[1024]; int fails = 0;
    VolumeData v; v.info.root = L"T:"; v.info.ntfs = true;
    v.Add(L"T:", 2, kNoParent, kFlagDir, 0, 0);
    auto add = [&](const wchar_t* nm, uint32_t parent, bool dir, uint64_t size, uint64_t mt) { return v.Add(nm, static_cast<uint32_t>(wcslen(nm)), parent, dir ? kFlagDir : 0, size, mt); };
    SYSTEMTIME now; GetLocalTime(&now);
    uint64_t today = FtLocal(now.wYear, now.wMonth, now.wDay);
    uint32_t docs = add(L"Docs", 0, true, 0, today), win = add(L"Windows", 0, true, 0, today);
    add(L"report.docx", docs, false, 20 << 10, FtLocal(2024, 3, 5));
    add(L"report.pdf", docs, false, 3 << 20, FtLocal(2024, 7, 1));
    add(L"notes.txt", docs, false, 0, today);
    add(L"photo.JPG", docs, false, 200 << 20, FtLocal(2023, 12, 31));
    add(L"notepad.exe", win, false, 300 << 10, FtLocal(2022, 1, 1));
    add(L"report", win, true, 0, today);
    add(L"1setup.exe", win, false, 5 << 20, today);
    std::vector<VolumeData> vols; vols.push_back(std::move(v));
    auto d = Merge(vols, 0);
    auto names = [&](const wchar_t* q) {
        auto cq = CompileQuery(q); std::vector<uint32_t> r; SearchQuery(*d, *cq, r, [] { return false; });
        std::vector<std::wstring> n; for (uint32_t i : r) n.emplace_back(d->Name(i)); std::sort(n.begin(), n.end());
        std::wstring s; for (auto& x : n) { if (!s.empty()) s += L","; s += x; } return s;
    };
    auto T = [&](const wchar_t* q, const wchar_t* want) {
        std::wstring got = names(q); bool ok = got == want;
        swprintf_s(buf, L"  %s %-34s => %s%s%s\r\n", ok ? L"[OK]  " : L"[FAIL]", q, got.c_str(), ok ? L"" : L"  期望 ", ok ? L"" : want); out += buf;
        if (!ok) ++fails;
    };
    out += L"[unit]\r\n";
    T(L"report", L"report,report.docx,report.pdf");
    T(L"report !pdf", L"report,report.docx");
    T(L"docx|pdf", L"report.docx,report.pdf");
    T(L"report docx|pdf", L"report.docx,report.pdf");
    T(L"<docx|pdf> | exe", L"1setup.exe,notepad.exe,report.docx,report.pdf");
    T(L"!<report|note>", L"1setup.exe,Docs,T:,Windows,photo.JPG");
    T(L"ext:exe", L"1setup.exe,notepad.exe");
    T(L"ext:jpg;.PDF", L"photo.JPG,report.pdf");
    T(L"size:>1mb", L"1setup.exe,photo.JPG,report.pdf");
    T(L"size:<=20kb", L"notes.txt,report.docx");
    T(L"size:1mb..10mb", L"1setup.exe,report.pdf");
    T(L"size:empty", L"notes.txt");
    T(L"size:gigantic", L"photo.JPG");
    T(L"dm:today file:", L"1setup.exe,notes.txt");
    T(L"dm:2024", L"report.docx,report.pdf");
    T(L"dm:2024-03", L"report.docx");
    T(L"dm:<2023-01-01", L"T:,notepad.exe");
    T(L"dm:2023..2024-03-05", L"photo.JPG,report.docx");
    T(L"folder:", L"Docs,T:,Windows,report");
    T(L"folder:report", L"report");
    T(L"file:report", L"report.docx,report.pdf");
    T(L"path:windows", L"1setup.exe,Windows,notepad.exe,report");
    T(L"path:docs\\", L"notes.txt,photo.JPG,report.docx,report.pdf");
    T(L"1*.exe", L"1setup.exe");
    T(L"\"report.d\"", L"report.docx");
    T(L"*.exe | *.pdf !1*", L"notepad.exe,report.pdf");
    T(L"size:abc", L"");
    T(L"startwith:notes", L"notes.txt");
    T(L"startwith:note", L"notepad.exe,notes.txt");
    T(L"endwith:.exe", L"1setup.exe,notepad.exe");
    T(L"exact:report", L"report");
    T(L"parent:Docs", L"notes.txt,photo.JPG,report.docx,report.pdf");
    T(L"nopath:windows", L"Windows");  // 仅名称：Windows 文件夹
    T(L"depth:1", L"Docs,T:,Windows");  // 根若为 T:\\ 则深度 1
    T(L"parent:Windows exact:report", L"report");

    InitFold(); g_fastAscii = true;
    std::wstring err; if (dbPath.empty()) dbPath = Indexer::DefaultDbPath();
    auto big = LoadDbCopy(dbPath, err);
    if (!big) { out += L"[db] 无数据库，改用合成索引\r\n"; big = Synthesize(2260000); }
    swprintf_s(buf, L"[db] %zu 个对象\r\n", big->entries.size()); out += buf;
    for (const wchar_t* q : {L"exe", L"exe|dll", L"ext:exe", L"ext:exe;dll;sys", L"!exe", L"size:>100mb", L"dm:thisweek", L"file: dm:today",
                             L"report !<pdf|docx>", L"path:windows ext:dll", L"*.exe size:>1mb",
                             L"ext:ani;bmp;gif;heic;ico;jpe;jpeg;jpg;pcx;png;psd;svg;tga;tif;tiff;webp;wmf", L"<report> folder:"}) {
        auto cq = CompileQuery(q); std::vector<uint32_t> res; double best = 1e9;
        for (int k = 0; k < 5; ++k) { double a = NowMs(); SearchQuery(*big, *cq, res, [] { return false; }); best = std::min(best, NowMs() - a); }
        swprintf_s(buf, L"[search] %-.40s results=%-8zu best=%6.2f ms\r\n", q, res.size(), best); out += buf;
    }
    swprintf_s(buf, L"[result] %s (%d failed)\r\n", fails ? L"FAIL" : L"PASS", fails); out += buf;
    WriteOut(L"ff_syntaxtest.txt", out);
    return fails ? 1 : 0;
}

// -regextest [db]：PCRE2 正则（单元 + 大索引耗时）
static int RegexTest(std::wstring dbPath) {
    std::wstring out; wchar_t buf[1024]; int fails = 0;
    VolumeData v; v.info.root = L"T:"; v.info.ntfs = true;
    v.Add(L"T:", 2, kNoParent, kFlagDir, 0, 0);
    auto add = [&](const wchar_t* nm, uint32_t parent, bool dir) { return v.Add(nm, static_cast<uint32_t>(wcslen(nm)), parent, dir ? kFlagDir : 0, 1, 0); };
    uint32_t docs = add(L"Docs", 0, true), win = add(L"Windows", 0, true);
    add(L"report2024.docx", docs, false); add(L"Report_v2.pdf", docs, false); add(L"notes.txt", docs, false);
    add(L"中文报告.TXT", docs, false); add(L"notepad.exe", win, false); add(L"a|b.txt", win, false);
    std::vector<VolumeData> vols; vols.push_back(std::move(v));
    auto d = Merge(vols, 0);
    auto names = [&](const wchar_t* q, bool rx, const wchar_t* extra, std::wstring* err) {
        MatchOpts o; o.matchRegex = rx;
        auto cq = CompileQuery(q, o, extra); std::vector<uint32_t> r; SearchQuery(*d, *cq, r, [] { return false; });
        if (err) *err = QueryError(*cq);
        std::vector<std::wstring> n; for (uint32_t i : r) n.emplace_back(d->Name(i)); std::sort(n.begin(), n.end());
        std::wstring s; for (auto& x : n) { if (!s.empty()) s += L","; s += x; } return s;
    };
    auto T = [&](const wchar_t* q, bool rx, const wchar_t* extra, const wchar_t* want) {
        std::wstring got = names(q, rx, extra, nullptr); bool ok = got == want;
        swprintf_s(buf, L"  %s %s%-28s %-10s => %s%s%s\r\n", ok ? L"[OK]  " : L"[FAIL]", rx ? L"(模式)" : L"", q, extra, got.c_str(), ok ? L"" : L"  期望 ", ok ? L"" : want); out += buf;
        if (!ok) ++fails;
    };
    out += L"[unit]\r\n";
    T(L"regex:^report\\d+", false, L"", L"report2024.docx");
    T(L"regex:^rep", false, L"", L"Report_v2.pdf,report2024.docx");
    T(L"regex:\\.(exe|pdf)$", false, L"", L"Report_v2.pdf,notepad.exe");
    T(L"regex:\"^not.*(txt|exe)$\"", false, L"", L"notepad.exe,notes.txt");
    T(L"!regex:\\.txt$ file:", false, L"", L"Report_v2.pdf,notepad.exe,report2024.docx");
    T(L"regex:^中文", false, L"", L"中文报告.TXT");
    T(L"regex:docs\\\\", false, L"", L"Report_v2.pdf,notes.txt,report2024.docx,中文报告.TXT");
    T(L"^(notes|notepad)\\.", true, L"", L"notepad.exe,notes.txt");
    T(L"a\\|b", true, L"", L"a|b.txt");
    T(L"^n", true, L"ext:exe", L"notepad.exe");
    T(L"", true, L"ext:pdf", L"Report_v2.pdf");
    {
        std::wstring err; std::wstring got = names(L"regex:(abc", false, L"", &err);
        bool ok = !err.empty() && got.empty();
        swprintf_s(buf, L"  %s 编译错误报告：%s\r\n", ok ? L"[OK]  " : L"[FAIL]", err.c_str()); out += buf; if (!ok) ++fails;
    }
    InitFold(); g_fastAscii = true;
    std::wstring err; if (dbPath.empty()) dbPath = Indexer::DefaultDbPath();
    auto big = LoadDbCopy(dbPath, err);
    if (!big) { out += L"[db] 无数据库，改用合成索引\r\n"; big = Synthesize(2260000); }
    swprintf_s(buf, L"[db] %zu 个对象\r\n", big->entries.size()); out += buf;
    struct { const wchar_t* q; bool rx; } qs[] = {
        {L"\\.exe$", true}, {L"^report", true}, {L"^[a-z]+\\d{3,}\\.(jpg|png)$", true}, {L"(?i)setup.*\\.msi$", true}, {L"^.*$", true},
        {L"\\d{4}-\\d{2}-\\d{2}", true}, {L"windows\\\\system32\\\\.*\\.dll$", true}, {L"ext:exe", false}, {L"*.exe", false}};
    for (auto& x : qs) {
        MatchOpts o; o.matchRegex = x.rx; double c0 = NowMs(); auto cq = CompileQuery(x.q, o); double cms = NowMs() - c0;
        std::vector<uint32_t> res; double best = 1e9;
        for (int k = 0; k < 5; ++k) { double a = NowMs(); SearchQuery(*big, *cq, res, [] { return false; }); best = std::min(best, NowMs() - a); }
        swprintf_s(buf, L"[search] %s%-36s results=%-8zu compile=%5.2f ms best=%7.2f ms\r\n", x.rx ? L"regex " : L"      ", x.q, res.size(), cms, best); out += buf;
    }
    {   // 交叉验证：file: regex:\.exe$ 与 ext:exe 结果数一致
        std::vector<uint32_t> a, b;
        SearchQuery(*big, *CompileQuery(L"file: regex:\\.exe$"), a, [] { return false; });
        SearchQuery(*big, *CompileQuery(L"ext:exe"), b, [] { return false; });
        bool ok = a == b; swprintf_s(buf, L"  %s file: regex:\\.exe$ (%zu) == ext:exe (%zu)\r\n", ok ? L"[OK]  " : L"[FAIL]", a.size(), b.size()); out += buf; if (!ok) ++fails;
    }
    swprintf_s(buf, L"[result] %s (%d failed)\r\n", fails ? L"FAIL" : L"PASS", fails); out += buf;
    WriteOut(L"ff_regextest.txt", out);
    return fails ? 1 : 0;
}

// -scrolltest：滚动重影自检。真实窗口上滚动（行/页/滚轮/水平），抓取屏幕像素，与强制整窗重画后的像素逐点比较
static int ScrollTest() {
    std::wstring out; wchar_t buf[512]; int fails = 0;
    OleInitialize(nullptr);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES}; InitCommonControlsEx(&icc);
    GetTheme().Resolve(ThemeMode::Light);
    WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"FFScrollTest"; RegisterClassW(&wc);
    HWND top = CreateWindowExW(WS_EX_TOPMOST, L"FFScrollTest", L"scrolltest", WS_POPUP | WS_VISIBLE, 40, 40, 700, 500, nullptr, nullptr, wc.hInstance, nullptr);
    ResultList list; HWND lv = list.Create(top, 1);
    MoveWindow(lv, 0, 0, 700, 500, TRUE);
    NONCLIENTMETRICSW ncm{sizeof(ncm)}; SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    HFONT f = CreateFontIndirectW(&ncm.lfMessageFont); list.SetFont(f, f, 26);
    ListView_SetColumnWidth(lv, 0, 400); ListView_SetColumnWidth(lv, 1, 600);   // 产生水平滚动条
    auto d = Synthesize(20000);
    auto r = std::make_shared<SearchResult>(); r->data = d; r->items = d->byName; r->items.resize(5000);
    list.SetResultsForTest(r);
    SetForegroundWindow(top); SetFocus(lv);
    auto pump = [] { MSG m; for (int k = 0; k < 50; ++k) { while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); } Sleep(2); } };
    auto grab = [&](std::vector<uint32_t>& px) {
        RECT rc; GetClientRect(lv, &rc); HDC sdc = GetDC(nullptr); POINT o{0, 0}; ClientToScreen(lv, &o);
        HDC mdc = CreateCompatibleDC(sdc); BITMAPINFO bi{}; bi.bmiHeader = {sizeof(BITMAPINFOHEADER), rc.right, -rc.bottom, 1, 32, BI_RGB};
        void* bits = nullptr; HBITMAP bm = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0); HGDIOBJ ob = SelectObject(mdc, bm);
        BitBlt(mdc, 0, 0, rc.right, rc.bottom, sdc, o.x, o.y, SRCCOPY);
        px.assign(static_cast<uint32_t*>(bits), static_cast<uint32_t*>(bits) + rc.right * rc.bottom);
        SelectObject(mdc, ob); DeleteObject(bm); DeleteDC(mdc); ReleaseDC(nullptr, sdc);
    };
    pump();
    struct Step { const wchar_t* name; UINT m; WPARAM w; } steps[] = {
        {L"行下 x3", WM_VSCROLL, SB_LINEDOWN}, {L"页下", WM_VSCROLL, SB_PAGEDOWN}, {L"滚轮下", WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA))},
        {L"滚轮上", WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA)}, {L"行上", WM_VSCROLL, SB_LINEUP}, {L"水平右", WM_HSCROLL, SB_PAGERIGHT}, {L"水平左", WM_HSCROLL, SB_LINELEFT},
        {L"键 PageDown", WM_KEYDOWN, VK_NEXT}, {L"键 Down x3", WM_KEYDOWN, VK_DOWN}};
    for (auto& st : steps) {
        RECT c; GetClientRect(lv, &c); POINT mid{c.right / 2, c.bottom / 2}; ClientToScreen(lv, &mid);
        int reps = wcsstr(st.name, L"x3") ? 3 : 1;
        for (int k = 0; k < reps; ++k) SendMessageW(lv, st.m, st.w, st.m == WM_MOUSEWHEEL ? MAKELPARAM(mid.x, mid.y) : 0);
        pump(); pump(); pump(); pump(); pump(); pump(); pump(); pump(); pump(); pump();   // 选择变化有主题淡入动画：等它结束
        std::vector<uint32_t> a, b; grab(a);
        RedrawWindow(lv, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN); pump(); grab(b);
        RECT hr{}; GetWindowRect(ListView_GetHeader(lv), &hr); RECT cr; GetClientRect(lv, &cr);
        size_t from = static_cast<size_t>(hr.bottom - hr.top) * cr.right;   // 只比较列表行区域（列头有悬停动画）
        size_t diff = 0; for (size_t k = from; k < a.size() && k < b.size(); ++k) if (a[k] != b[k]) ++diff;
        bool ok = diff == 0 && !a.empty();
        swprintf_s(buf, L"  %s %-12s 顶行=%d 焦点行=%d 差异像素=%zu\r\n", ok ? L"[OK]  " : L"[FAIL]", st.name, ListView_GetTopIndex(lv), ListView_GetNextItem(lv, -1, LVNI_FOCUSED), diff); out += buf;
        if (!ok) ++fails;
    }
    {   // 整窗重画耗时
        double best = 1e9; for (int k = 0; k < 20; ++k) { double t0 = NowMs(); RedrawWindow(lv, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW); best = std::min(best, NowMs() - t0); }
        swprintf_s(buf, L"[paint] 整个列表重画 %.2f ms\r\n", best); out += buf;
    }
    DestroyWindow(top); DeleteObject(f);
    swprintf_s(buf, L"[result] %s (%d failed)\r\n", fails ? L"FAIL" : L"PASS", fails); out += buf;
    WriteOut(L"ff_scrolltest.txt", out);
    return fails ? 1 : 0;
}

// -sortclicktest：真实鼠标点击列头（SendInput）-> 排序 -> 搜索线程 -> 结果，逐列升/降序校验
namespace sct {
ResultList* g_list; Searcher* g_s; std::shared_ptr<IndexData> g_d; int g_lvnClicks = 0;
LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_NOTIFY: { auto* nm = reinterpret_cast<NMHDR*>(l); if (nm->code == LVN_COLUMNCLICK) ++g_lvnClicks; LRESULT o = 0; if (g_list && g_list->OnNotify(nm, &o)) return o; return 0; }
    case WM_APP_SORT: g_s->Request(L"", g_d, g_list->Sort()); return 0;
    case WM_APP_RESULTS: { auto r = g_s->Latest(); if (r && r->generation == w) g_list->SetResultsForTest(r); return 0; }
    }
    return DefWindowProcW(h, m, w, l);
}
}
static int SortClickTest() {
    std::wstring out; wchar_t buf[512]; int fails = 0;
    OleInitialize(nullptr);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES}; InitCommonControlsEx(&icc);
    GetTheme().Resolve(ThemeMode::Light);
    GetSettings().sortCol = 0; GetSettings().sortDesc = false;
    for (int c = 0; c < 4; ++c) GetSettings().colVisible[c] = true;
    WNDCLASSW wc{}; wc.lpfnWndProc = sct::Proc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"FFSortClick"; RegisterClassW(&wc);
    HWND top = CreateWindowExW(WS_EX_TOPMOST, L"FFSortClick", L"sortclicktest", WS_POPUP | WS_VISIBLE, 60, 60, 900, 500, nullptr, nullptr, wc.hInstance, nullptr);
    ResultList list; sct::g_list = &list; HWND lv = list.Create(top, 1);
    MoveWindow(lv, 0, 0, 900, 500, TRUE);
    ListView_SetColumnWidth(lv, 0, 220); ListView_SetColumnWidth(lv, 1, 300); ListView_SetColumnWidth(lv, 2, 120); ListView_SetColumnWidth(lv, 3, 200);
    auto d = Synthesize(30000);
    for (size_t i = 0; i < d->entries.size(); ++i) { auto& e = d->entries.Mut(i); e.mtime = 133000000000000000ULL + (i * 2654435761ULL) % 100000000000ULL; if (!e.IsDir()) e.size = (i * 40503ULL) % 1000000; }
    sct::g_d = d;
    Searcher s(top); sct::g_s = &s;
    auto rank = BuildFolderPathRank(*d);
    SetForegroundWindow(top);
    auto pump = [](int ms) { MSG m; ULONGLONG end = GetTickCount64() + ms; while (GetTickCount64() < end) { while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); } Sleep(5); } };
    s.Request(L"", d, list.Sort()); pump(400);
    HWND hdr = ListView_GetHeader(lv);
    auto click = [&](int c) {
        RECT r; Header_GetItemRect(hdr, c, &r); POINT p{(r.left + r.right) / 2, (r.top + r.bottom) / 2}; ClientToScreen(hdr, &p);
        SetCursorPos(p.x, p.y);
        INPUT in[2]{}; in[0].type = in[1].type = INPUT_MOUSE; in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN; in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        SendInput(2, in, sizeof(INPUT));
        pump(500);
    };
    const wchar_t* cn[] = {L"名称", L"路径", L"扩展名", L"大小", L"时间"};
    auto key = [&](int c, uint32_t i) -> uint64_t { const Entry& e = d->entries[i];
        return c == 2 ? (e.IsDir() ? 0 : e.size + 1) : c == 3 ? e.mtime : (e.parent == kNoParent ? 0 : rank[e.parent]); };
    // 名称当前升序：点 路径(升) 路径(降) 大小(降) 大小(升) 时间(降) 时间(升) 名称(升) 名称(降)
    int seq[] = {1, 1, 2, 2, 3, 3, 0, 0};
    for (int c : seq) {
        int before = list.SortClicks();
        click(c);
        auto* r = list.Results();
        SortSpec so = list.Sort();
        bool ok = r && r->items.size() == d->byName.size() && so.col == static_cast<SortCol>(c) && list.SortClicks() == before + 1 && r->sort == so;
        for (size_t k = 1; ok && k < r->items.size(); ++k) {
            uint32_t a = r->items[k - 1], b = r->items[k];
            if (c == 0) { int cmp = CompareFold(d->Name(a), d->Name(b)); if (so.desc ? cmp < 0 : cmp > 0) ok = false; }
            else { uint64_t ka = key(c, a), kb = key(c, b); if (so.desc ? ka < kb : ka > kb) ok = false; }
        }
        HDITEMW hi{HDI_FORMAT}; Header_GetItem(hdr, c, &hi);
        bool arrow = (hi.fmt & (so.desc ? HDF_SORTDOWN : HDF_SORTUP)) != 0;
        swprintf_s(buf, L"  %s 点击 %s -> %s%s  箭头=%d\r\n", ok && arrow ? L"[OK]  " : L"[FAIL]", cn[c], cn[static_cast<int>(so.col)], so.desc ? L"降" : L"升", arrow); out += buf;
        if (!ok || !arrow) ++fails;
    }
    swprintf_s(buf, L"[diag] 主窗口收到 LVN_COLUMNCLICK %d 次，排序点击 %d 次\r\n", sct::g_lvnClicks, list.SortClicks()); out += buf;
    {   // 名称排序不依赖输入顺序：打乱后排序
        std::vector<uint32_t> it = d->byName; std::reverse(it.begin(), it.end()); std::rotate(it.begin(), it.begin() + it.size() / 3, it.end());
        SortItems(*d, it, {SortCol::Name, false}, nullptr);
        bool ok = it == d->byName; out += ok ? L"  [OK]   名称排序：乱序输入也正确\r\n" : L"  [FAIL] 名称排序：乱序输入\r\n"; if (!ok) ++fails;
    }
    sct::g_list = nullptr;
    DestroyWindow(top);
    swprintf_s(buf, L"[result] %s (%d failed)\r\n", fails ? L"FAIL" : L"PASS", fails); out += buf;
    WriteOut(L"ff_sortclicktest.txt", out);
    return fails ? 1 : 0;
}

// -icontest [db]：真实图标（SHGetFileInfo 读文件）与按扩展名图标的耗时对比
static int IconTest(std::wstring dbPath) {
    std::wstring out; wchar_t buf[512];
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::wstring err; if (dbPath.empty()) dbPath = Indexer::DefaultDbPath();
    auto d = LoadDbCopy(dbPath, err);
    if (!d) { WriteOut(L"ff_icontest.txt", L"[skip] 无数据库\r\n"); return 0; }
    std::vector<std::wstring> paths;
    for (uint32_t i : d->byName) { auto n = d->Name(i); if (n.size() > 4 && CompareFold(n.substr(n.size() - 4), L".exe") == 0) { paths.push_back(d->FullPath(i)); if (paths.size() >= 200) break; } }
    double a = NowMs(); SHFILEINFOW sfi{};
    for (auto& p : paths) SHGetFileInfoW(p.c_str(), 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
    double real = NowMs() - a;
    a = NowMs();
    for (auto& p : paths) SHGetFileInfoW(p.c_str(), 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
    double warm = NowMs() - a;
    a = NowMs();
    for (size_t k = 0; k < paths.size(); ++k) SHGetFileInfoW(L"x.exe", FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
    double ext = NowMs() - a;
    swprintf_s(buf, L"[icon] %zu 个 exe：真实图标首次 %.2f ms/个，再次 %.2f ms/个；按扩展名 %.3f ms/个\r\n", paths.size(), real / std::max<size_t>(1, paths.size()), warm / std::max<size_t>(1, paths.size()), ext / std::max<size_t>(1, paths.size()));
    out += buf;
    WriteOut(L"ff_icontest.txt", out);
    return 0;
}

// -pipetest：启动 -svc-console（非服务的同一管道服务器），通过管道测试协议
static int PipeTest(std::wstring vol) {
    std::wstring out; wchar_t buf[2048]; int fails = 0;
    auto check = [&](bool ok, const std::wstring& what) { out += (ok ? L"  [OK]   " : L"  [FAIL] "); out += what; out += L"\r\n"; if (!ok) ++fails; };
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" -svc-console";
    STARTUPINFOW si{sizeof(si)}; PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) { out += L"[FAIL] 无法启动 -svc-console\r\n"; WriteOut(L"ff_pipetest.txt", out); return 1; }
    std::unique_ptr<NtfsSource> src; std::wstring why;
    double t0 = NowMs();
    for (int k = 0; k < 100 && !src; ++k) { WaitNamedPipeW(L"\\\\.\\pipe\\FlashFind.Service", 100); src = ConnectService(true, &why); if (!src) Sleep(50); }
    swprintf_s(buf, L"[hello] 连接+HELLO %.1f ms %s\r\n", NowMs() - t0, why.c_str()); out += buf;
    check(src != nullptr, L"连接 -svc-console 管道并 HELLO");
    if (src) {
        // 正式连接（不允许控制台）必须拒绝非服务进程：防管道抢注
        std::wstring w2; auto strict = ConnectService(false, &w2);
        check(!strict, L"严格模式拒绝非 SCM 服务进程（" + w2 + L"）");
        uint64_t jid = 0, jid2 = 0; int64_t f = 0, nx = 0, f2 = 0, nx2 = 0; std::wstring e;
        bool q = src->QueryJournal(vol, false, jid, f, nx, e);
        bool q2 = MftIndexer::QueryJournal(vol, false, jid2, f2, nx2, e);
        swprintf_s(buf, L"[journal] via pipe ok=%d id=%llx next=%lld；直接 ok=%d id=%llx\r\n", q, jid, nx, q2, jid2); out += buf;
        check(q == q2 && jid == jid2, L"QUERY_JOURNAL 结果与本地一致");
        std::vector<uint8_t> raw; int64_t from = nx; e.clear();
        DWORD r = src->ReadUsn(vol, jid, from, raw, e);
        swprintf_s(buf, L"[read_usn] status=%lu bytes=%zu err=%s（中等完整性下应为 5）\r\n", r, raw.size(), e.c_str()); out += buf;
        check(MftIndexer::IsElevated() ? r == 0 : r == ERROR_ACCESS_DENIED, L"READ_USN 错误码经管道原样传回");
        std::vector<LookupResult> lr;
        bool lok = src->Lookup(vol, {5, 0x0005000000000005ULL}, lr);
        check(lok && lr.size() == 2 && (MftIndexer::IsElevated() || (!lr[0].sizeOk && !lr[1].sizeOk)), L"LOOKUP 批量：失败项标记为不可用（保留旧值）");
        VolumeData bv; bv.info.root = vol; std::atomic<bool> stop{false}; std::atomic<size_t> prog{0}; e.clear();
        bool bok = src->Build(bv, false, stop, prog, e);
        out += L"[build] ok=" + std::to_wstring(bok) + L" err=" + e + L"\r\n";
        check(MftIndexer::IsElevated() || (!bok && !e.empty()), L"BUILD 失败时错误信息经管道传回");
        e.clear(); VolumeData bad;
        check(!src->ScanDir(L"Z:", L"Z:\\x", {}, bad, e) && !e.empty(), L"非法根被服务端拒绝（" + e + L"）");
        e.clear();
        check(!src->ScanDir(vol, vol + L"\\a\\..\\..\\Windows", {}, bad, e), L"含 \\.. 的路径被服务端拒绝");
        // SCAN_DIR：在测试卷上建临时树，与本地 FindFirstFileEx 枚举比较
        std::wstring base = vol + L"\\ff_pipetest_dir";
        CreateDirectoryW(base.c_str(), nullptr);
        for (int k = 0; k < 5; ++k) { std::wstring d = base + L"\\sub" + std::to_wstring(k); CreateDirectoryW(d.c_str(), nullptr);
            for (int j = 0; j < 40; ++j) { std::wstring fpath = d + L"\\文件_" + std::to_wstring(j) + L".txt"; HANDLE h = CreateFileW(fpath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr); if (h != INVALID_HANDLE_VALUE) { DWORD wr; WriteFile(h, "hello", 5, &wr, nullptr); CloseHandle(h); } } }
        VolumeData sv; e.clear(); double ts = NowMs();
        bool sok = src->ScanDir(vol, base, {}, sv, e); double sms = NowMs() - ts;
        auto local = MakeLocalSource(); VolumeData lv; std::wstring e3;
        bool lok2 = local && local->ScanDir(vol, base, {}, lv, e3);
        bool same = sok && lok2 && sv.entries.size() == lv.entries.size() && sv.frn == lv.frn && sv.names == lv.names;
        size_t sized = 0; for (size_t i = 1; i < sv.entries.size(); ++i) if (!sv.entries[i].IsDir() && sv.entries[i].size == 5) ++sized;
        swprintf_s(buf, L"[scan_dir] %zu 个对象 %.2f ms（本地 %zu）err=%s\r\n", sv.entries.size(), sms, lv.entries.size(), e.c_str()); out += buf;
        check(same && sv.entries.size() == 206 && sized == 200 && sv.frn[1] != 0, L"SCAN_DIR 经管道与本地枚举一致（含 FileId、大小）");
        check(lok2 && !lv.entries.empty() && std::wstring(lv.names.data() + lv.entries[0].nameOff, lv.entries[0].nameLen) == L"ff_pipetest_dir", L"entries[0] 是起始目录（取路径最后一段为名）");
        double mbps = PipeBench(*src, 512);
        swprintf_s(buf, L"[bench] 管道吞吐 %.0f MB/s（512 MB，1 MB 帧）\r\n", mbps); out += buf;
        check(mbps > 100, L"管道吞吐 > 100 MB/s");
        // 端到端：Indexer 走服务（-svc-console）
        Indexer ix; Indexer::Options o; o.roots = {vol}; o.allowConsoleService = true; o.monitor = false;
        wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp); o.dbPath = std::wstring(tmp) + L"ff_pipetest.db"; DeleteFileW(o.dbPath.c_str());
        ix.Start(nullptr, o); Sleep(300);
        for (int k = 0; k < 600 && ix.Busy(); ++k) Sleep(50);
        auto st = ix.Stats();
        out += L"[indexer] source=" + st.source + L" note=" + st.note + L"\r\n";
        check(st.source == L"FlashFind 服务", L"Indexer 未提升时选择服务作为特权来源");
        ix.Stop(); DeleteFileW(o.dbPath.c_str());
        for (int k = 0; k < 5; ++k) { std::wstring d = base + L"\\sub" + std::to_wstring(k);
            for (int j = 0; j < 40; ++j) DeleteFileW((d + L"\\文件_" + std::to_wstring(j) + L".txt").c_str()); RemoveDirectoryW(d.c_str()); }
        RemoveDirectoryW(base.c_str());
        src.reset();
    }
    {   // 无服务时的严格连接
        HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\FlashFind.SvcConsoleStop");
        if (ev) { SetEvent(ev); CloseHandle(ev); }
        DWORD w = WaitForSingleObject(pi.hProcess, 5000);
        check(w == WAIT_OBJECT_0, L"-svc-console 收到停止事件后退出");
        if (w != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        std::wstring w2; auto none = ConnectService(false, &w2);
        check(!none, L"无服务：ConnectService 失败（" + w2 + L"）");
        check(!ServiceInstalled() || true, L"ServiceInstalled() = " + std::to_wstring(ServiceInstalled()));
    }
    swprintf_s(buf, L"[result] %s (%d failed)\r\n", fails ? L"FAIL" : L"PASS", fails); out += buf;
    WriteOut(L"ff_pipetest.txt", out);
    return fails ? 1 : 0;
}

static int SvcConsole() {
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, L"Local\\FlashFind.SvcConsoleStop");
    Log(L"-svc-console：管道服务器（非服务，仅测试）");
    int r = RunPipeServer(ev);
    CloseHandle(ev);
    return r;
}

// 测试模式的崩溃诊断：未捕获异常 / abort / CRT 无效参数（都会表现为 0xc0000409）时打印原因与调用栈（模块偏移）
static void DumpStack(const char* why) {
    void* fr[48]; USHORT n = CaptureStackBackTrace(1, 48, fr, nullptr);
    HMODULE self = GetModuleHandleW(nullptr);
    fprintf(stderr, "[crash] %s\n", why);
    for (USHORT i = 0; i < n; ++i) {
        HMODULE m = nullptr; GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(fr[i]), &m);
        fprintf(stderr, "  #%02u %s+0x%llx\n", i, m == self ? "FlashFind" : "other", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(fr[i]) - reinterpret_cast<uintptr_t>(m)));
    }
    fflush(stderr);
}
static void InstallCrashDiag() {
    std::set_terminate([] {
        const char* w = "terminate";
        try { if (auto e = std::current_exception()) std::rethrow_exception(e); } catch (const std::exception& e) { DumpStack(e.what()); TerminateProcess(GetCurrentProcess(), 3); } catch (...) {}
        DumpStack(w); TerminateProcess(GetCurrentProcess(), 3);
    });
    signal(SIGABRT, [](int) { DumpStack("abort"); TerminateProcess(GetCurrentProcess(), 3); });
    _set_invalid_parameter_handler([](const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) { DumpStack("CRT invalid parameter"); TerminateProcess(GetCurrentProcess(), 4); });
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* ep) -> LONG {
        char b[64]; sprintf_s(b, "SEH 0x%08lx", ep->ExceptionRecord->ExceptionCode); DumpStack(b);
        HMODULE m = nullptr; GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(ep->ExceptionRecord->ExceptionAddress), &m);
        fprintf(stderr, "  at +0x%llx\n", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress) - reinterpret_cast<uintptr_t>(m))); fflush(stderr);
        return EXCEPTION_EXECUTE_HANDLER; });
}

int App::Run(HINSTANCE inst, int show) {
    g_inst = inst;
    {
        int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        std::wstring mode = argc >= 2 ? argv[1] : L"", arg = argc >= 3 ? argv[2] : L"";
        LocalFree(argv);
        if (wcsstr(GetCommandLineW(), L"-debug-log")) g_debugLog = true;
        if (mode.size() > 5 && mode.compare(mode.size() - 4, 4, L"test") == 0) InstallCrashDiag();
        if (mode == L"-svc") return ServiceMain();
        if (mode == L"-optimizationtest") return OptimizationTest(arg == L"-bench");
        if (mode == L"-svc-console") return SvcConsole();
        if (mode == L"-install-service") return InstallService();
        if (mode == L"-uninstall-service") return UninstallService();
        if (mode == L"-icontest") { InitFold(); return IconTest(arg); }
        if (mode == L"-sortclicktest") { InitFold(); g_fastAscii = true; return SortClickTest(); }
        if (mode == L"-scrolltest") { InitFold(); return ScrollTest(); }
        if (mode == L"-regextest") { InitFold(); g_fastAscii = true; return RegexTest(arg); }
        if (mode == L"-syntaxtest") { InitFold(); g_fastAscii = true; return SyntaxTest(arg); }
        if (mode == L"-sorttest") { InitFold(); g_fastAscii = true; return SortTest(arg); }
        if (mode == L"-wildtest") { InitFold(); g_fastAscii = true; return WildTest(arg); }
        if (mode == L"-pipetest") { InitFold(); g_fastAscii = true; return PipeTest(arg.empty() ? L"G:" : arg); }
        if (mode == L"-incrtest" || mode == L"-initest" || mode == L"-gtest") {
            InitFold(); g_fastAscii = true;
            if (mode == L"-incrtest") return IncrTest(arg);
            if (mode == L"-initest") return IniTest();
            return GTest(arg.empty() ? L"G:" : arg);
        }
    }
    if (wcsstr(GetCommandLineW(), L"-selftest")) return SelfTest();

    std::wstring cliSearch, cliSelect;
    int cliFilter = -1; SortSpec cliSort{}; bool cliReindex = false, startHidden = false;
    {
        int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        auto eq = [](const wchar_t* a, const wchar_t* b) { return _wcsicmp(a, b) == 0; };
        auto need = [&](int& i) -> const wchar_t* { return (i + 1 < argc) ? argv[++i] : L""; };
        for (int i = 1; i < argc; ++i) {
            const wchar_t* a = argv[i];
            if (eq(a, L"-help") || eq(a, L"-h") || eq(a, L"-?")) {
                MessageBoxW(nullptr,
                    L"闪寻 FlashFind\n\n"
                    L"FlashFind.exe [选项]\n\n"
                    L"-s, -search <文本>     设置搜索\n"
                    L"-p, -path <路径>       搜索路径（path:前缀）\n"
                    L"-filter <序号|名称>    过滤器\n"
                    L"-sort <name|path|ext|size|date>\n"
                    L"-sort-ascending / -sort-descending\n"
                    L"-select <完整路径>     选中结果\n"
                    L"-reindex               强制重建索引\n"
                    L"-url <es:搜索>         从 es: URL 设置搜索\n"
                    L"-startup               后台启动（隐藏）\n"
                    L"-exit, -quit            退出已运行的实例\n"
                    L"-help                  显示本说明\n",
                    L"闪寻 — 命令行", MB_OK | MB_ICONINFORMATION);
                LocalFree(argv); return 0;
            }
            if (eq(a, L"-exit") || eq(a, L"-quit")) {
                LocalFree(argv);
                if (HWND h = FindWindowW(kMainClass, nullptr)) PostMessageW(h, WM_CLOSE, 0, 0);
                return 0;
            }
            if (eq(a, L"-startup")) startHidden = true;
            else if (eq(a, L"-reindex")) cliReindex = true;
            else if (eq(a, L"-s") || eq(a, L"-search")) cliSearch = need(i);
            else if (eq(a, L"-p") || eq(a, L"-path")) {
                std::wstring path = need(i);
                if (!path.empty()) cliSearch = L"path:" + path;
            } else if (eq(a, L"-filter")) {
                std::wstring f = need(i);
                if (!f.empty() && f[0] >= L'0' && f[0] <= L'9') cliFilter = _wtoi(f.c_str());
                else {
                    const wchar_t* names[] = {L"全部", L"音频", L"压缩文件", L"文档", L"可执行文件", L"文件夹", L"图片", L"视频"};
                    for (int k = 0; k < 8; ++k) if (_wcsicmp(f.c_str(), names[k]) == 0) { cliFilter = k; break; }
                }
            } else if (eq(a, L"-sort")) {
                std::wstring c = need(i);
                if (eq(c.c_str(), L"name")) cliSort.col = SortCol::Name;
                else if (eq(c.c_str(), L"path")) cliSort.col = SortCol::Path;
                else if (eq(c.c_str(), L"ext") || eq(c.c_str(), L"extension")) cliSort.col = SortCol::Ext;
                else if (eq(c.c_str(), L"size")) cliSort.col = SortCol::Size;
                else if (eq(c.c_str(), L"date") || eq(c.c_str(), L"date_modified")) cliSort.col = SortCol::Date;
            } else if (eq(a, L"-sort-ascending")) cliSort.desc = false;
            else if (eq(a, L"-sort-descending")) cliSort.desc = true;
            else if (eq(a, L"-select")) cliSelect = need(i);
            else if (eq(a, L"-url")) {
                const wchar_t* q = need(i);
                if (_wcsnicmp(q, L"es:", 3) == 0) q += 3;
                if (q[0] == L'/' && q[1] == L'/') q += 2;
                cliSearch = q;
            }
        }
        LocalFree(argv);
    }

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\FlashFind.Instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND h = FindWindowW(kMainClass, nullptr)) {
            DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(h, WM_APP_SHOW, 0, 0);
        }
        return 0;
    }

    OleInitialize(nullptr);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    InitFold();
    GetSettings().Load();
    GetTheme().Resolve(GetSettings().theme);

    MainWindow wnd;
    wnd.ApplyStartup(cliSearch, cliFilter, cliSort, cliSelect, cliReindex);
    if (!wnd.Create(startHidden ? SW_HIDE : show)) return 1;

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (wnd.PreTranslate(msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    GetSettings().Save();
    OleUninitialize();
    if (mutex) CloseHandle(mutex);
    return static_cast<int>(msg.wParam);
}

} // namespace ff
