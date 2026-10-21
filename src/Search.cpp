#include "Search.h"
#include "App.h"
#include "Util.h"
#include <algorithm>
#include <chrono>

namespace ff {

Searcher::Searcher(HWND notify) : hwnd_(notify) { th_ = std::thread([this] { Loop(); }); }

Searcher::~Searcher() {
    { std::lock_guard lk(mu_); quit_ = true; ++gen_; }
    cv_.notify_all();
    th_.join();
}

void Searcher::Request(const std::wstring& query, std::shared_ptr<const IndexData> data) {
    { std::lock_guard lk(mu_); query_ = query; data_ = std::move(data); pending_ = true; ++gen_; }
    cv_.notify_all();
}

std::shared_ptr<SearchResult> Searcher::Latest() { std::lock_guard lk(mu_); return latest_; }

std::vector<std::wstring> Searcher::ParseTerms(const std::wstring& query) {
    // M1：空格分隔 = AND；"..." 引号内为整体短语
    std::vector<std::wstring> terms; std::wstring cur; bool quote = false;
    for (wchar_t c : query) {
        if (c == L'"') { quote = !quote; continue; }
        if (c == L' ' && !quote) { if (!cur.empty()) terms.push_back(ToLower(cur)); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) terms.push_back(ToLower(cur));
    return terms;
}

void Searcher::Loop() {
    for (;;) {
        std::wstring q; std::shared_ptr<const IndexData> d; uint64_t g;
        {
            std::unique_lock lk(mu_);
            cv_.wait(lk, [&] { return quit_ || pending_; });
            if (quit_) return;
            pending_ = false; q = query_; d = data_; g = gen_;
        }
        auto t0 = std::chrono::steady_clock::now();
        auto res = std::make_shared<SearchResult>();
        res->generation = g; res->data = d; res->terms = ParseTerms(q);
        std::atomic<bool> cancelled{false};
        if (d) {
            const auto& order = d->byName;
            if (res->terms.empty()) {
                res->items = order;
            } else {
                // 多线程：按名称序切块并行匹配，各块结果按块序拼接 => 保持排序，无需再排
                const size_t n = order.size();
                unsigned nt = std::max(1u, std::min(std::thread::hardware_concurrency(), 16u));
                if (n < 200000) nt = 1;
                std::vector<std::vector<uint32_t>> parts(nt);
                const auto& terms = res->terms;
                auto work = [&](unsigned t) {
                    size_t b = n * t / nt, e = n * (t + 1) / nt;
                    auto& out = parts[t];
                    std::vector<wchar_t> full(32768);  // 每线程一次分配
                    for (size_t k = b; k < e; ++k) {
                        if ((k & 0x3FFF) == 0 && gen_ != g) { cancelled = true; return; }
                        uint32_t i = order[k];
                        auto name = d->LowerName(i);
                        bool ok = true; size_t flen = SIZE_MAX;
                        for (auto& term : terms) {
                            if (term.find(L'\\') != std::wstring::npos) {
                                if (flen == SIZE_MAX) {
                                    flen = d->FullPathInto(i, full.data(), full.size());
                                    CharLowerBuffW(full.data(), static_cast<DWORD>(flen));
                                }
                                if (std::wstring_view(full.data(), flen).find(term) == std::wstring_view::npos) { ok = false; break; }
                            } else if (name.find(term) == std::wstring_view::npos) { ok = false; break; }
                        }
                        if (ok) out.push_back(i);
                    }
                };
                std::vector<std::thread> ths;
                for (unsigned t = 1; t < nt; ++t) ths.emplace_back(work, t);
                work(0);
                for (auto& th : ths) th.join();
                size_t total = 0; for (auto& p : parts) total += p.size();
                res->items.reserve(total);
                for (auto& p : parts) res->items.insert(res->items.end(), p.begin(), p.end());
            }
        }
        if (cancelled) continue;
        res->elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        { std::lock_guard lk(mu_); if (g != gen_) continue; latest_ = res; }
        PostMessageW(hwnd_, WM_APP_RESULTS, static_cast<WPARAM>(g), 0);
    }
}

} // namespace ff
