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
    std::vector<uint8_t> slash(terms.size()), ascii(terms.size());
    for (size_t j = 0; j < terms.size(); ++j) {
        slash[j] = terms[j].find(L'\\') != std::wstring::npos;
        ascii[j] = g_fastAscii && IsAscii(terms[j]);
    }
    auto match = [&](std::wstring_view s, size_t j) { return ascii[j] ? ContainsFoldAscii(s, terms[j]) : ContainsFold(s, terms[j]); };
    auto work = [&](unsigned t) {
        size_t b = n * t / nt, e = n * (t + 1) / nt;
        auto& o = parts[t];
        std::vector<wchar_t> full;
        for (size_t k = b; k < e; ++k) {
            if ((k & 0x3FFF) == 0 && (stop || cancelled())) { stop = true; return; }
            uint32_t i = order[k];
            auto name = d.Name(i);
            bool ok = true; size_t flen = SIZE_MAX;
            for (size_t j = 0; j < terms.size(); ++j) {
                if (slash[j]) {    // 含 \ 匹配完整路径
                    if (flen == SIZE_MAX) { if (full.empty()) full.resize(32768); flen = d.FullPathInto(i, full.data(), full.size()); }
                    if (!match({full.data(), flen}, j)) { ok = false; break; }
                } else if (!match(name, j)) { ok = false; break; }
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

Searcher::Searcher(HWND notify) : hwnd_(notify) { th_ = std::thread([this] { Loop(); }); }

Searcher::~Searcher() {
    { std::lock_guard lk(mu_); quit_ = true; ++gen_; }
    cv_.notify_all();
    th_.join();
}

void Searcher::Request(const std::wstring& query, std::shared_ptr<const IndexData> data) {
    if (!data) return;
    { std::lock_guard lk(mu_); query_ = query; data_ = std::move(data); pending_ = true; ++gen_; }
    cv_.notify_all();
}

std::shared_ptr<SearchResult> Searcher::Latest() { std::lock_guard lk(mu_); return latest_; }

void Searcher::Loop() {
    for (;;) {
        std::wstring q; std::shared_ptr<const IndexData> d; uint64_t g;
        {
            std::unique_lock lk(mu_);
            cv_.wait(lk, [&] { return quit_ || pending_; });
            if (quit_) return;
            pending_ = false; q = query_; d = data_; g = gen_;
        }
        if (!d) continue;
        auto t0 = std::chrono::steady_clock::now();
        auto res = std::make_shared<SearchResult>();
        res->generation = g; res->data = d; res->terms = ParseTerms(q);
        if (!SearchCore(*d, res->terms, res->items, [&] { return gen_ != g; })) continue;
        res->elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        { std::lock_guard lk(mu_); if (g != gen_) continue; latest_ = res; }
        if (hwnd_) PostMessageW(hwnd_, WM_APP_RESULTS, static_cast<WPARAM>(g), 0);
    }
}

} // namespace ff
