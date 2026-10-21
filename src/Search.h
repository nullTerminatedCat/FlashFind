#pragma once
// 后台搜索：新请求自动取消旧请求（代号递增）；多线程分块匹配。
#include "Index.h"
#include <condition_variable>
#include <functional>

namespace ff {

struct SearchResult {
    uint64_t generation = 0;
    std::shared_ptr<const IndexData> data;
    std::vector<uint32_t> items;      // 已按名称排序
    std::vector<std::wstring> terms;  // 折叠后的搜索词（用于高亮）
    double elapsedMs = 0;
};

std::vector<std::wstring> ParseTerms(const std::wstring& query);
// 返回 false 表示被取消
bool SearchCore(const IndexData& d, const std::vector<std::wstring>& terms, std::vector<uint32_t>& out,
                const std::function<bool()>& cancelled);

class Searcher {
public:
    explicit Searcher(HWND notify);
    ~Searcher();
    // data 为空（索引未就绪）时直接丢弃请求，绝不发布 0 结果
    void Request(const std::wstring& query, std::shared_ptr<const IndexData> data);
    std::shared_ptr<SearchResult> Latest();

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
