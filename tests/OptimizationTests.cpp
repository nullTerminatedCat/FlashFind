#include "OptimizationTests.h"
#include "../src/Index.h"
#include "../src/Usn.h"
#include "../src/Search.h"
#include "../src/Fold.h"
#include <winioctl.h>
#include <cstdio>
#include <chrono>

namespace ff {
namespace {
struct Checks {
    int failed = 0;
    void operator()(bool ok, const char* label) {
        printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        if (!ok) ++failed;
    }
};

uint32_t Add(VolumeData& v, const wchar_t* name, uint32_t parent, bool dir, uint64_t frn) {
    auto id = v.Add(name, wcslen(name), parent, dir ? kFlagDir : 0, dir ? 0 : 123, 1000);
    v.frn.push_back(frn);
    return id;
}

std::shared_ptr<IndexData> Fixture() {
    VolumeData v; v.info.root = L"T:"; v.info.mft = v.info.ntfs = true;
    Add(v, L"T:", kNoParent, true, 1);
    Add(v, L"Report.txt", 0, false, 2);
    Add(v, L"Docs", 0, true, 3);
    std::vector<VolumeData> vols; vols.push_back(std::move(v));
    return Merge(vols, 0);
}

UsnEvent Event(uint64_t frn, uint64_t parent, uint32_t reason, const wchar_t* name, bool dir = false) {
    UsnEvent e; e.frn = frn; e.parent = parent; e.reason = reason;
    e.attrs = dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
    e.time = 9000; e.name = name;
    return e;
}

void MetadataTests(Checks& check) {
    auto base = Fixture();
    LiveUpdater u(base);
    auto ev = Event(2, 1, USN_REASON_BASIC_INFO_CHANGE | USN_REASON_CLOSE, L"Report.txt");
    size_t calls = 0;
    auto lookup = [&](uint32_t vol, const std::vector<uint64_t>& frns, std::vector<LookupResult>& out) {
        ++calls;
        check(vol == 0 && frns == std::vector<uint64_t>{2}, "metadata lookup batches/deduplicates FRNs");
        out = {{1, 1, 456, 2000}};
    };
    u.Apply(0, {ev, ev}, lookup);
    auto snap = u.Commit();
    check(calls == 1 && snap && snap->entries[1].size == 456 && snap->entries[1].mtime == 2000,
          "standalone BASIC_INFO_CHANGE publishes fresh metadata");
    check(base->entries[1].size == 123 && base->entries[1].mtime == 1000,
          "previous snapshot remains immutable");
    if (snap) {
        check(snap->byName == base->byName, "metadata-only update preserves name order");
        u.Apply(0, {ev}, lookup);
        check(!u.Dirty() && !u.Commit(), "unchanged metadata does not publish/save a snapshot");
        ev.reason = USN_REASON_DATA_EXTEND | USN_REASON_CLOSE;
        u.Apply(0, {ev}, lookup);
        check(!u.Dirty(), "unchanged data event does not publish/save a snapshot");
        u.Apply(0, {ev}, [](uint32_t, const auto&, auto& out) { out = {{}}; });
        check(!u.Dirty(), "failed lookup preserves known size/time, not the USN event time");
        u.Apply(0, {ev}, [](uint32_t, const auto&, auto& out) { out = {{1, 0, 789, 0}}; });
        auto partial = u.Commit();
        check(partial && partial->entries[1].size == 789 && partial->entries[1].mtime == 2000,
              "partial lookup updates size while preserving known modification time");
    }
    LiveUpdater deleted(base);
    deleted.Apply(0, {ev, Event(2, 1, USN_REASON_FILE_DELETE, L"Report.txt")},
        [&](uint32_t, const auto&, auto&) { check(false, "deleted file must not be looked up"); });
    auto gone = deleted.Commit();
    check(gone && gone->entries[1].IsDeleted(), "delete supersedes queued metadata lookup");
}

void MovedInTests(Checks& check) {
    auto base = Fixture();
    LiveUpdater u(base);
    u.SetFilter(0, {L"T:\\Inbox\\Keep\\Skip"}, {L"T:\\Inbox\\Keep"});
    u.SetScanner([&](uint32_t, const std::wstring& path, VolumeData& out) {
        check(path == L"T:\\Inbox", "moved directory is scanned at its new path");
        Add(out, L"Inbox", kNoParent, true, 10);
        Add(out, L"Keep", 0, true, 11);
        Add(out, L"yes.txt", 1, false, 12);
        Add(out, L"Other", 0, true, 13);
        Add(out, L"no.txt", 3, false, 14);
        Add(out, L"Skip", 1, true, 15);
        Add(out, L"excluded.txt", 5, false, 16);
        Add(out, L"outside.txt", 0, false, 17);
        return true;
    });
    u.Apply(0, {Event(10, 1, USN_REASON_RENAME_NEW_NAME, L"Inbox", true)}, {});
    auto snap = u.Commit();
    check(snap && u.Find(0, 10) != SIZE_MAX && u.Find(0, 11) != SIZE_MAX && u.Find(0, 12) != SIZE_MAX,
          "moved-in scan keeps include-only ancestors and selected subtree");
    check(u.Find(0, 13) == SIZE_MAX && u.Find(0, 14) == SIZE_MAX && u.Find(0, 17) == SIZE_MAX,
          "moved-in scan rejects include-only siblings and their descendants");
    check(u.Find(0, 15) != SIZE_MAX && u.Find(0, 16) == SIZE_MAX,
          "moved-in scan keeps excluded directory itself but rejects its contents");
}

void GlobTests(Checks& check) {
    auto base = Fixture();
    for (bool fast : {false, true}) {
        g_fastAscii = fast;
        MatchOpts opt; opt.matchCase = true;
        auto matches = [&](const wchar_t* pattern, size_t want) {
            auto q = CompileQuery(pattern, opt);
            std::vector<uint32_t> out;
            SearchQuery(*base, *q, out, [] { return false; });
            check(out.size() == want, "case-sensitive glob respects literals with ? and multiple *");
        };
        matches(L"R?port.txt", 1); matches(L"r?port.txt", 0);
        matches(L"R*p*t.txt", 1); matches(L"r*p*t.txt", 0);
        opt.matchCase = false;
        matches(L"r?port.txt", 1); matches(L"R*p*t.txt", 1);
    }
    g_fastAscii = true;
}

void NameOrderTests(Checks& check) {
    VolumeData v; v.info.root = L"T:";
    Add(v, L"T:", kNoParent, true, 1);
    for (const auto* name : {L"alpha.txt", L"Alpha.txt", L"ALPHA.txt", L"report.txt", L"Report.txt", L"中文.txt", L"z.txt"})
        Add(v, name, 0, false, v.entries.size() + 1);
    std::vector<VolumeData> vols; vols.push_back(std::move(v));
    auto d = Merge(vols, 0);
    for (const auto* query : {L"", L"txt", L"alpha", L"folder:", L"no-such-name"}) {
        auto q = CompileQuery(query);
        std::vector<uint32_t> matches;
        SearchQuery(*d, *q, matches, [] { return false; });
        for (bool desc : {false, true}) {
            auto reference = matches, fast = matches;
            SortItems(*d, reference, {SortCol::Name, desc}, nullptr);
            SortItems(*d, fast, {SortCol::Name, desc}, nullptr, true);
            check(fast == reference, "name-order fast path matches general sort (ties, Unicode, empty/subset)");
        }
    }
    // 真实搜索线程：切换到其他列后，再切回名称排序，缓存仍须是名称升序。
    Searcher search(nullptr);
    uint64_t previous = 0;
    for (auto sort : {SortSpec{SortCol::Name, false}, SortSpec{SortCol::Size, true},
                      SortSpec{SortCol::Name, true}, SortSpec{SortCol::Name, false}}) {
        search.Request(L"", d, sort);
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        std::shared_ptr<SearchResult> result;
        do {
            result = search.Latest();
            if (result && result->generation > previous) break;
            Sleep(1);
        } while (std::chrono::steady_clock::now() < until);
        auto reference = d->byName;
        SortItems(*d, reference, sort, nullptr);
        check(result && result->generation > previous && result->items == reference,
              "search thread reuses name-ordered cache across sort changes");
        if (result) previous = result->generation;
    }
}

double Millis(std::chrono::steady_clock::time_point begin) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
}

void Benchmark(Checks& check) {
    constexpr size_t count = 2260000;
    VolumeData v; v.info.root = L"T:"; v.info.mft = true;
    Add(v, L"T:", kNoParent, true, 1);
    wchar_t name[64];
    for (size_t i = 1; i < count; ++i) {
        swprintf_s(name, L"file_%07zu.txt", i);
        Add(v, name, 0, false, i + 1);
    }
    std::vector<VolumeData> vols; vols.push_back(std::move(v));
    auto d = Merge(vols, 0);
    vols.clear();
    for (bool desc : {false, true}) {
        std::vector<double> oldMs, fastMs;
        for (int pass = 0; pass < 6; ++pass) {
            // 交替执行，第一轮预热不计；计入结果复制，避免报告空操作的虚高倍数。
            for (int path = 0; path < 2; ++path) {
                const bool fast = ((pass + path) & 1) != 0;
                auto begin = std::chrono::steady_clock::now();
                auto items = d->byName;
                SortItems(*d, items, {SortCol::Name, desc}, nullptr, fast);
                auto ms = Millis(begin);
                check(desc ? std::equal(items.begin(), items.end(), d->byName.rbegin()) : items == d->byName,
                      "benchmark result order is unchanged");
                if (pass) (fast ? fastMs : oldMs).push_back(ms);
            }
        }
        std::sort(oldMs.begin(), oldMs.end()); std::sort(fastMs.begin(), fastMs.end());
        printf("[bench] %zu objects name %s: copy+sort median old=%.3f ms new=%.3f ms speedup=%.1fx\n",
               count, desc ? "descending" : "ascending", oldMs[2], fastMs[2], oldMs[2] / fastMs[2]);
    }
    printf("[bench] avoided name-sort arrays: %.2f MiB (name positions + sort keys; excludes merge scratch)\n",
           count * (sizeof(uint32_t) + 16.0) / (1024.0 * 1024.0));
    LiveUpdater u(d);
    const auto begin = std::chrono::steady_clock::now();
    u.Apply(0, {}, {});
    const double emptyMs = Millis(begin);
    check(!u.Dirty() && u.LastStats().frnIndexMs == 0, "empty USN batch skips building FRN and child indexes");
    printf("[bench] empty USN batch: %.3f ms\n", emptyMs);
}
} // namespace

int OptimizationTest(bool benchmark) {
    InitFold();
    Checks check;
    MetadataTests(check);
    MovedInTests(check);
    GlobTests(check);
    NameOrderTests(check);
    if (benchmark) Benchmark(check);
    printf("[result] %s (%d failed)\n", check.failed ? "FAIL" : "PASS", check.failed);
    return check.failed ? 1 : 0;
}
} // namespace ff
