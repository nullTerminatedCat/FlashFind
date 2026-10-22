#pragma once
// 后台搜索：新请求自动取消旧请求（代号递增）；多线程分块匹配。
#include "Index.h"
#include <condition_variable>
#include <functional>

namespace ff {

enum class SortCol : uint8_t { Name = 0, Path, Ext, Size, Date };
struct SortSpec { SortCol col = SortCol::Name; bool desc = false; bool operator==(const SortSpec&) const = default; };

// 文件夹按路径的先序排名（子项按名称排序的深度优先序）：O(n)，每个快照只算一次
std::vector<uint32_t> BuildFolderPathRank(const IndexData& d);
// items 须为名称顺序（SearchCore 的输出）；按列排序，同键保持名称顺序。多线程分块排序 + 归并。
void SortItems(const IndexData& d, std::vector<uint32_t>& items, SortSpec s, const std::vector<uint32_t>* folderRank);

// 匹配开关（搜索菜单）；编译进 Query，影响所有 Pat/Regex 节点
struct MatchOpts {
    bool matchCase = false;
    bool matchWholeWord = false;
    bool matchPath = false;
    bool matchRegex = false;   // 整个查询文本当作一条正则
    bool operator==(const MatchOpts&) const = default;
};

// 完整搜索语法（| ! < > ext: size: dm: path: file: folder: startwith: endwith: exact: parent: depth: nopath: regex: 通配符）
struct Query;
std::shared_ptr<Query> CompileQuery(const std::wstring& text, MatchOpts opts = {}, const std::wstring& extra = {});
// 正则编译错误（空 = 无错误）
const std::wstring& QueryError(const Query& q);
const std::vector<std::wstring>& QueryHighlight(const Query& q);
bool SearchQuery(const IndexData& d, const Query& q, std::vector<uint32_t>& out, const std::function<bool()>& cancelled);

struct SearchResult {
    uint64_t generation = 0;
    std::shared_ptr<const IndexData> data;
    std::vector<uint32_t> items;      // 已按 sort 排序
    SortSpec sort;
    std::vector<std::wstring> terms;  // 折叠后的搜索词（用于高亮）
    double elapsedMs = 0;
    std::wstring error;               // 正则编译错误
};

std::vector<std::wstring> ParseTerms(const std::wstring& query);
// 单个词的匹配（含通配符 * ? 时锚定整名；用于测试）。foldedTerm 须已折叠
bool WildcardMatch(std::wstring_view s, const std::wstring& foldedTerm);
// 返回 false 表示被取消
bool SearchCore(const IndexData& d, const std::vector<std::wstring>& terms, std::vector<uint32_t>& out,
                const std::function<bool()>& cancelled);

class Searcher {
public:
    explicit Searcher(HWND notify);
    ~Searcher();
    // data 为空（索引未就绪）时直接丢弃请求，绝不发布 0 结果
    void Request(const std::wstring& query, std::shared_ptr<const IndexData> data, SortSpec sort = {}, MatchOpts opts = {}, std::wstring filter = {});
    std::shared_ptr<SearchResult> Latest();

private:
    void Loop();
    HWND hwnd_;
    std::thread th_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool quit_ = false, pending_ = false;
    std::wstring query_, filter_; MatchOpts opts_;
    SortSpec sort_;
    // 只改排序时复用上次的匹配结果（仅搜索线程访问）
    std::wstring lastQ_; std::shared_ptr<const IndexData> lastD_; std::vector<uint32_t> lastItems_;
    const IndexData* rankFor_ = nullptr; std::shared_ptr<const IndexData> rankHold_; std::vector<uint32_t> rank_;
    std::shared_ptr<const IndexData> data_;
    std::atomic<uint64_t> gen_{0};
    std::shared_ptr<SearchResult> latest_;
};

} // namespace ff
