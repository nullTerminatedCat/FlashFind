#include "Search.h"
#include "App.h"
#include "Fold.h"
#include <algorithm>
#include <chrono>

namespace ff {

std::vector<std::wstring> ParseTerms(const std::wstring& query) {
    // 空格分隔 = AND；"..." 为整体短语；词按折叠表折叠
    InitFold();
    std::vector<std::wstring> terms; std::wstring cur; bool quote = false;
    auto flush = [&] { if (!cur.empty()) { for (auto& c : cur) c = Fold(c); terms.push_back(cur); } cur.clear(); };
    for (wchar_t c : query) {
        if (c == L'"') { quote = !quote; continue; }
        if (c == L' ' && !quote) flush(); else cur += c;
    }
    flush();
    return terms;
}

// ---------- 通配符（Everything 风格）----------
// 含 * 或 ? 的词匹配整个名称（含 \ 时匹配完整路径），锚定首尾；不含通配符的词仍为子串匹配。
// 每次搜索只编译一次；常见形态走无回溯快速路径。
namespace {
enum class PatKind : uint8_t { Substr, All, Exact, Prefix, Suffix, PrefixSuffix, Glob };
struct Pattern {
    PatKind kind = PatKind::Substr;
    std::wstring a, b;       // Exact/Prefix: a；Suffix: b；PrefixSuffix: a+b；Substr: a；Glob: a = 规范化模式（连续 * 合并）
    bool ascii = false, path = false;
};

template <bool A> inline wchar_t F(wchar_t c) { if constexpr (A) return FoldA(c); else return Fold(c); }

template <bool A> inline bool EqAt(const wchar_t* s, std::wstring_view lit) {
    for (size_t k = 0; k < lit.size(); ++k) if (F<A>(s[k]) != lit[k]) return false;
    return true;
}

// 迭代式贪心 + 单回溯点（无递归）：最坏 O(n*m)，常见情况线性
template <bool A> bool GlobMatch(std::wstring_view s, std::wstring_view p) {
    size_t i = 0, j = 0, star = SIZE_MAX, mark = 0;
    const size_t n = s.size(), m = p.size();
    while (i < n) {
        if (j < m && p[j] != L'*' && (p[j] == L'?' || p[j] == F<A>(s[i]))) { ++i; ++j; }
        else if (j < m && p[j] == L'*') { star = j++; mark = i; }
        else if (star != SIZE_MAX) { j = star + 1; i = ++mark; }
        else return false;
    }
    while (j < m && p[j] == L'*') ++j;
    return j == m;
}

Pattern Compile(const std::wstring& t) {
    Pattern p;
    p.path = t.find(L'\\') != std::wstring::npos;
    p.ascii = g_fastAscii && IsAscii(t);
    if (t.find_first_of(L"*?") == std::wstring::npos) { p.kind = PatKind::Substr; p.a = t; return p; }
    std::wstring g; for (wchar_t c : t) if (!(c == L'*' && !g.empty() && g.back() == L'*')) g += c;   // ** -> *
    if (g == L"*") { p.kind = PatKind::All; return p; }
    if (g.find(L'?') == std::wstring::npos) {
        size_t stars = std::count(g.begin(), g.end(), L'*');
        if (stars == 0) { p.kind = PatKind::Exact; p.a = g; return p; }
        if (stars == 1) {
            size_t k = g.find(L'*');
            p.a = g.substr(0, k); p.b = g.substr(k + 1);
            p.kind = p.a.empty() ? PatKind::Suffix : (p.b.empty() ? PatKind::Prefix : PatKind::PrefixSuffix);
            return p;
        }
        if (stars == 2 && g.front() == L'*' && g.back() == L'*') { p.kind = PatKind::Substr; p.a = g.substr(1, g.size() - 2); return p; }   // *x* = 包含
    }
    p.kind = PatKind::Glob; p.a = g;
    return p;
}

template <bool A> inline bool MatchT(std::wstring_view s, const Pattern& p) {
    switch (p.kind) {
    case PatKind::All: return true;
    case PatKind::Substr: if constexpr (A) return ContainsFoldAscii(s, p.a); else return ContainsFold(s, p.a);
    case PatKind::Exact: return s.size() == p.a.size() && EqAt<A>(s.data(), p.a);
    case PatKind::Prefix: return s.size() >= p.a.size() && EqAt<A>(s.data(), p.a);
    case PatKind::Suffix: return s.size() >= p.b.size() && EqAt<A>(s.data() + s.size() - p.b.size(), p.b);
    case PatKind::PrefixSuffix:
        return s.size() >= p.a.size() + p.b.size() && EqAt<A>(s.data() + s.size() - p.b.size(), p.b) && EqAt<A>(s.data(), p.a);
    case PatKind::Glob: return GlobMatch<A>(s, p.a);
    }
    return false;
}
inline bool Match(std::wstring_view s, const Pattern& p) { return p.ascii ? MatchT<true>(s, p) : MatchT<false>(s, p); }
} // namespace

bool WildcardMatch(std::wstring_view s, const std::wstring& foldedTerm) { return Match(s, Compile(foldedTerm)); }

bool SearchCore(const IndexData& d, const std::vector<std::wstring>& terms, std::vector<uint32_t>& out,
                const std::function<bool()>& cancelled) {
    const auto& order = d.byName;
    out.clear();
    if (terms.empty()) { out = order; return true; }
    const size_t n = order.size();
    unsigned nt = std::max(1u, std::min(std::thread::hardware_concurrency(), 16u));
    if (n < 100000) nt = 1;
    std::vector<std::vector<uint32_t>> parts(nt);
    std::atomic<bool> stop{false};
    std::vector<Pattern> pats; pats.reserve(terms.size());
    for (auto& t : terms) pats.push_back(Compile(t));
    // 先测便宜的：名称词在前，路径词在后（AND 顺序无关）
    std::stable_sort(pats.begin(), pats.end(), [](const Pattern& x, const Pattern& y) { return x.path < y.path; });
    if (std::all_of(pats.begin(), pats.end(), [](const Pattern& p) { return p.kind == PatKind::All; })) { out = order; return true; }
    auto work = [&](unsigned t) {
        size_t b = n * t / nt, e = n * (t + 1) / nt;
        auto& o = parts[t];
        std::vector<wchar_t> full;
        for (size_t k = b; k < e; ++k) {
            if ((k & 0x3FFF) == 0 && (stop || cancelled())) { stop = true; return; }
            uint32_t i = order[k];
            auto name = d.Name(i);
            bool ok = true; size_t flen = SIZE_MAX;
            for (const Pattern& p : pats) {
                if (p.path) {    // 含 \ 匹配完整路径
                    if (flen == SIZE_MAX) { if (full.empty()) full.resize(32768); flen = d.FullPathInto(i, full.data(), full.size()); }
                    if (!Match({full.data(), flen}, p)) { ok = false; break; }
                } else if (!Match(name, p)) { ok = false; break; }
            }
            if (ok) o.push_back(i);
        }
    };
    std::vector<std::thread> ths;
    for (unsigned t = 1; t < nt; ++t) ths.emplace_back(work, t);
    work(0);
    for (auto& th : ths) th.join();
    if (stop) return false;
    size_t total = 0; for (auto& p : parts) total += p.size();
    out.reserve(total);
    for (auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return true;
}

std::vector<uint32_t> BuildFolderPathRank(const IndexData& d) {
    const size_t n = d.entries.size();
    std::vector<uint32_t> rank(n, 0), first(n, kNoParent), next(n, kNoParent);
    std::vector<uint32_t> roots;
    // 逆序遍历 byName 并头插：每个文件夹的子文件夹链表按名称升序
    for (size_t k = d.byName.size(); k-- > 0;) {
        uint32_t i = d.byName[k];
        const Entry& e = d.entries[i];
        if (!e.IsDir()) continue;
        if (e.parent == kNoParent) { roots.push_back(i); continue; }
        next[i] = first[e.parent]; first[e.parent] = i;
    }
    // 卷根不在 byName 中（或在）：补齐未出现的根
    for (size_t i = 0; i < n; ++i) if (d.entries[i].parent == kNoParent && d.entries[i].IsDir() && std::find(roots.begin(), roots.end(), static_cast<uint32_t>(i)) == roots.end()) roots.push_back(static_cast<uint32_t>(i));
    std::sort(roots.begin(), roots.end(), [&](uint32_t a, uint32_t b) { return CompareFold(d.Name(a), d.Name(b)) < 0; });
    uint32_t r = 0;
    std::vector<uint32_t> st;
    for (auto it = roots.rbegin(); it != roots.rend(); ++it) st.push_back(*it);
    std::vector<uint32_t> tmp;
    while (!st.empty()) {
        uint32_t i = st.back(); st.pop_back();
        rank[i] = ++r;
        tmp.clear();
        for (uint32_t c = first[i]; c != kNoParent; c = next[c]) tmp.push_back(c);
        for (auto it = tmp.rbegin(); it != tmp.rend(); ++it) st.push_back(*it);
    }
    return rank;
}

void SortItems(const IndexData& d, std::vector<uint32_t>& items, SortSpec s, const std::vector<uint32_t>* folderRank) {
    const size_t n = items.size();
    if (s.col == SortCol::Name) { if (s.desc) std::reverse(items.begin(), items.end()); return; }
    struct K { uint64_t k; uint32_t pos; uint32_t id; };
    std::vector<K> v(n);
    const bool desc = s.desc;
    unsigned nt = std::max(1u, std::min(std::thread::hardware_concurrency(), 16u));
    if (n < 50000) nt = 1;
    auto fill = [&](size_t b, size_t e) {
        for (size_t k = b; k < e; ++k) {
            uint32_t i = items[k]; const Entry& en = d.entries[i];
            uint64_t key;
            switch (s.col) {
            case SortCol::Size: key = en.IsDir() ? 0 : en.size + 1; break;     // 文件夹排在最小处
            case SortCol::Date: key = en.mtime; break;
            default: key = en.parent == kNoParent || !folderRank ? 0 : (*folderRank)[en.parent]; break;
            }
            v[k] = {desc ? ~key : key, static_cast<uint32_t>(k), i};
        }
    };
    auto less = [](const K& a, const K& b) { return a.k != b.k ? a.k < b.k : a.pos < b.pos; };
    // 分块：填键 + 排序并行；然后两两归并
    std::vector<size_t> bounds(nt + 1);
    for (unsigned t = 0; t <= nt; ++t) bounds[t] = n * t / nt;
    {
        std::vector<std::thread> ths;
        auto job = [&](unsigned t) { fill(bounds[t], bounds[t + 1]); std::sort(v.begin() + bounds[t], v.begin() + bounds[t + 1], less); };
        for (unsigned t = 1; t < nt; ++t) ths.emplace_back(job, t);
        job(0);
        for (auto& th : ths) th.join();
    }
    for (size_t width = 1; width < nt; width *= 2) {
        std::vector<std::thread> ths;
        for (size_t t = 0; t + width < nt; t += 2 * width) {
            size_t b = bounds[t], m = bounds[t + width], e = bounds[std::min<size_t>(t + 2 * width, nt)];
            ths.emplace_back([&, b, m, e] { std::inplace_merge(v.begin() + b, v.begin() + m, v.begin() + e, less); });
        }
        for (auto& th : ths) th.join();
    }
    for (size_t k = 0; k < n; ++k) items[k] = v[k].id;
}

Searcher::Searcher(HWND notify) : hwnd_(notify) { th_ = std::thread([this] { Loop(); }); }

Searcher::~Searcher() {
    { std::lock_guard lk(mu_); quit_ = true; ++gen_; }
    cv_.notify_all();
    th_.join();
}

void Searcher::Request(const std::wstring& query, std::shared_ptr<const IndexData> data, SortSpec sort) {
    if (!data) return;
    { std::lock_guard lk(mu_); query_ = query; data_ = std::move(data); sort_ = sort; pending_ = true; ++gen_; }
    cv_.notify_all();
}

std::shared_ptr<SearchResult> Searcher::Latest() { std::lock_guard lk(mu_); return latest_; }

void Searcher::Loop() {
    for (;;) {
        std::wstring q; std::shared_ptr<const IndexData> d; uint64_t g; SortSpec so;
        {
            std::unique_lock lk(mu_);
            cv_.wait(lk, [&] { return quit_ || pending_; });
            if (quit_) return;
            pending_ = false; q = query_; d = data_; g = gen_; so = sort_;
        }
        if (!d) continue;
        auto t0 = std::chrono::steady_clock::now();
        auto res = std::make_shared<SearchResult>();
        res->generation = g; res->data = d; res->terms = ParseTerms(q); res->sort = so;
        if (d == lastD_ && q == lastQ_) res->items = lastItems_;   // 只改了排序
        else {
            if (!SearchCore(*d, res->terms, res->items, [&] { return gen_ != g; })) continue;
            lastD_ = d; lastQ_ = q; lastItems_ = res->items;
        }
        if (so.col == SortCol::Path && rankFor_ != d.get()) { rank_ = BuildFolderPathRank(*d); rankFor_ = d.get(); rankHold_ = d; }
        if (gen_ != g) continue;
        SortItems(*d, res->items, so, so.col == SortCol::Path ? &rank_ : nullptr);
        res->elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        { std::lock_guard lk(mu_); if (g != gen_) continue; latest_ = res; }
        if (hwnd_) PostMessageW(hwnd_, WM_APP_RESULTS, static_cast<WPARAM>(g), 0);
    }
}

} // namespace ff
