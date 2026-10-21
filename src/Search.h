#pragma once
// 后台搜索：新请求自动取消旧请求（代号递增）。
#include "Index.h"
#include <condition_variable>

namespace ff {

struct SearchResult {
    uint64_t generation = 0;
    std::shared_ptr<const IndexData> data;
    std::vector<uint32_t> items;      // 已按名称排序
    std::vector<std::wstring> terms;  // 小写搜索词（用于高亮）
    double elapsedMs = 0;
};

class Searcher {
public:
    explicit Searcher(HWND notify);
    ~Searcher();
    // 结果就绪时向 notify 发送 WM_APP_RESULTS(wParam = generation)
    void Request(const std::wstring& query, std::shared_ptr<const IndexData> data);
    std::shared_ptr<SearchResult> Latest();

    static std::vector<std::wstring> ParseTerms(const std::wstring& query);

private:
    void Loop();
    HWND hwnd_;
    std::thread th_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool quit_ = false, pending_ = false;
    std::wstring query_;
    std::shared_ptr<const IndexData> data_;
    std::atomic<uint64_t> gen_{0};
    std::shared_ptr<SearchResult> latest_;
};

} // namespace ff
