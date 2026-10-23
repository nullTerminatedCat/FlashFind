#include "Search.h"
#include "App.h"
#include "Fold.h"
#include <algorithm>
#include <chrono>
#define PCRE2_CODE_UNIT_WIDTH 16
#include <pcre2.h>

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
enum class PatKind : uint8_t { Substr, All, Exact, Prefix, Suffix, PrefixSuffix, Glob, WholeWord };
struct Pattern {
    PatKind kind = PatKind::Substr;
    std::wstring a, b;       // Exact/Prefix: a；Suffix: b；PrefixSuffix: a+b；Substr/WholeWord: a；Glob: a
    bool ascii = false, path = false, caseSens = false;
};

inline bool IsWord(wchar_t c) {
    return (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || c >= 0x80;
}

template <bool A> inline wchar_t F(wchar_t c) { if constexpr (A) return FoldA(c); else return Fold(c); }

template <bool A> inline bool EqAt(const wchar_t* s, std::wstring_view lit, bool caseSens) {
    for (size_t k = 0; k < lit.size(); ++k) {
        wchar_t a = s[k], b = lit[k];
        if (caseSens ? a != b : F<A>(a) != b) return false;
    }
    return true;
}

template <bool A> bool WholeWordMatch(std::wstring_view s, std::wstring_view lit, bool caseSens) {
    if (lit.empty()) return true;
    if (lit.size() > s.size()) return false;
    for (size_t i = 0; i + lit.size() <= s.size(); ++i) {
        if (!EqAt<A>(s.data() + i, lit, caseSens)) continue;
        bool L = i == 0 || !IsWord(s[i - 1]);
        bool R = i + lit.size() == s.size() || !IsWord(s[i + lit.size()]);
        if (L && R) return true;
    }
    return false;
}

// 迭代式贪心 + 单回溯点（无递归）：最坏 O(n*m)，常见情况线性
template <bool A> bool GlobMatch(std::wstring_view s, std::wstring_view p, bool caseSens) {
    size_t i = 0, j = 0, star = SIZE_MAX, mark = 0;
    const size_t n = s.size(), m = p.size();
    while (i < n) {
        if (j < m && p[j] != L'*' && (p[j] == L'?' || p[j] == (caseSens ? s[i] : F<A>(s[i])))) { ++i; ++j; }
        else if (j < m && p[j] == L'*') { star = j++; mark = i; }
        else if (star != SIZE_MAX) { j = star + 1; i = ++mark; }
        else return false;
    }
    while (j < m && p[j] == L'*') ++j;
    return j == m;
}

Pattern Compile(const std::wstring& t, bool caseSens = false, bool wholeWord = false) {
    Pattern p;
    p.caseSens = caseSens;
    std::wstring src = t;
    if (!caseSens) for (auto& c : src) c = Fold(c);
    p.path = src.find(L'\\') != std::wstring::npos;
    p.ascii = g_fastAscii && IsAscii(src);
    if (src.find_first_of(L"*?") == std::wstring::npos) {
        p.kind = wholeWord ? PatKind::WholeWord : PatKind::Substr;
        p.a = std::move(src); return p;
    }
    std::wstring g; for (wchar_t c : src) if (!(c == L'*' && !g.empty() && g.back() == L'*')) g += c;   // ** -> *
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
    const bool cs = p.caseSens;
    switch (p.kind) {
    case PatKind::All: return true;
    case PatKind::Substr:
        if (cs) {
            for (size_t i = 0; i + p.a.size() <= s.size(); ++i) if (EqAt<A>(s.data() + i, p.a, true)) return true;
            return false;
        }
        if constexpr (A) return ContainsFoldAscii(s, p.a); else return ContainsFold(s, p.a);
    case PatKind::WholeWord: return WholeWordMatch<A>(s, p.a, cs);
    case PatKind::Exact: return s.size() == p.a.size() && EqAt<A>(s.data(), p.a, cs);
    case PatKind::Prefix: return s.size() >= p.a.size() && EqAt<A>(s.data(), p.a, cs);
    case PatKind::Suffix: return s.size() >= p.b.size() && EqAt<A>(s.data() + s.size() - p.b.size(), p.b, cs);
    case PatKind::PrefixSuffix:
        return s.size() >= p.a.size() + p.b.size() && EqAt<A>(s.data() + s.size() - p.b.size(), p.b, cs) && EqAt<A>(s.data(), p.a, cs);
    case PatKind::Glob: return GlobMatch<A>(s, p.a, cs);
    }
    return false;
}
inline bool Match(std::wstring_view s, const Pattern& p) { return p.ascii ? MatchT<true>(s, p) : MatchT<false>(s, p); }
} // namespace

bool WildcardMatch(std::wstring_view s, const std::wstring& foldedTerm) { return Match(s, Compile(foldedTerm)); }

// ---------- 搜索语法 ----------
// 空格 = AND；| = OR（优先级高于空格）；! 前缀 = NOT；< > 分组；"..." 短语
// 函数：ext:a;b  size:[比较]N[kb|mb|gb] / A..B / empty tiny small medium large huge gigantic
//       dm:today yesterday thisweek lastweek thismonth lastmonth thisyear lastyear / YYYY[-MM[-DD]] / 比较 / A..B
//       path:词（匹配完整路径） file:[词] folder:[词]
// PCRE2（UTF-16，JIT）：每次搜索编译一次，每个工作线程一个 match_data
struct RegexProg {
    pcre2_code_16* code = nullptr; bool jit = false;
    ~RegexProg() { if (code) pcre2_code_free_16(code); }
};
struct QNode {
    enum Kind : uint8_t { And, Or, Not, Pat, Ext, Size, Date, IsFile, IsDir, Regex, Parent, Depth } kind;
    std::shared_ptr<RegexProg> re;
    std::vector<uint32_t> kids;
    Pattern pat;
    std::vector<std::wstring> exts;
    uint64_t lo = 0, hi = UINT64_MAX;   // 闭区间
    bool needPath = false;
};
struct Query {
    std::vector<QNode> nodes;
    int root = -1;
    std::vector<std::wstring> highlight;
    bool everything = true;   // 空查询或全部为 *
    bool hasRegex = false;
    std::wstring error;
    MatchOpts opts;
};

namespace {
std::wstring FoldStr(std::wstring_view v) { std::wstring r(v); for (auto& c : r) c = Fold(c); return r; }

// 词法：返回 token 列表；特殊 token：L"|" L"<" L">"；! 作为前缀保留在 token 中
std::vector<std::wstring> Tokenize(const std::wstring& q) {
    std::vector<std::wstring> out; std::wstring cur; bool quote = false, any = false;
    auto flush = [&] { if (any) out.push_back(cur); cur.clear(); any = false; };
    for (wchar_t c : q) {
        if (c == L'"') { quote = !quote; any = true; cur += L'\x01'; continue; }   // \x01 标记“出现过引号”，之后剔除
        if (!quote && (c == L' ' || c == L'\t')) { flush(); continue; }
        if (!quote) {   // regex:参数里的 | < > 是正则语法，不是分组（含空格须加引号）
            size_t s0 = cur.find_first_not_of(L'!');
            if (s0 != std::wstring::npos && cur.size() >= s0 + 6 && _wcsnicmp(cur.c_str() + s0, L"regex:", 6) == 0) { cur += c; any = true; continue; }
        }
        if (!quote && (c == L'|' || c == L'<' || c == L'>')) {
            // size:>10mb 之类：函数参数里的 < > 不是分组
            size_t colon = cur.find(L':');
            if (c != L'|' && colon != std::wstring::npos && colon + 1 == cur.size()) { cur += c; any = true; continue; }
            if (c != L'|' && colon != std::wstring::npos && cur.size() > colon + 1 && (cur.back() == L'.' )) { cur += c; any = true; continue; }
            flush(); out.push_back(std::wstring(1, c)); continue;
        }
        cur += c; any = true;
    }
    flush();
    return out;
}

bool ParseSize(std::wstring_view v, uint64_t& out) {
    size_t k = 0; double x = 0; bool dig = false, frac = false; double f = 0.1;
    for (; k < v.size(); ++k) {
        wchar_t c = v[k];
        if (c >= L'0' && c <= L'9') { dig = true; if (frac) { x += (c - L'0') * f; f /= 10; } else x = x * 10 + (c - L'0'); }
        else if (c == L'.' && !frac && k + 1 < v.size() && v[k + 1] != L'.') frac = true;
        else break;
    }
    if (!dig) return false;
    std::wstring u = FoldStr(v.substr(k));
    double m = 1;
    if (u.empty() || u == L"b") m = 1;
    else if (u == L"k" || u == L"kb") m = 1024.0;
    else if (u == L"m" || u == L"mb") m = 1024.0 * 1024;
    else if (u == L"g" || u == L"gb") m = 1024.0 * 1024 * 1024;
    else if (u == L"t" || u == L"tb") m = 1024.0 * 1024 * 1024 * 1024;
    else return false;
    out = static_cast<uint64_t>(x * m);
    return true;
}

uint64_t LocalToFt(int y, int mo, int d) {
    SYSTEMTIME lt{}; lt.wYear = static_cast<WORD>(y); lt.wMonth = static_cast<WORD>(mo); lt.wDay = static_cast<WORD>(d);
    SYSTEMTIME ut{}; FILETIME ft{};
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &lt, &ut) || !SystemTimeToFileTime(&ut, &ft)) return 0;
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}
constexpr uint64_t kDay = 864000000000ULL;

// 日期：返回 [lo, hi) 的本地日历区间
bool ParseDate(std::wstring_view v, uint64_t& lo, uint64_t& hi) {
    std::wstring w = FoldStr(v);
    SYSTEMTIME now; GetLocalTime(&now);
    uint64_t today = LocalToFt(now.wYear, now.wMonth, now.wDay);
    int dow = (now.wDayOfWeek + 6) % 7;   // 周一为一周开始
    auto month = [&](int y, int m) { if (m < 1) { m += 12; --y; } lo = LocalToFt(y, m, 1); int y2 = y, m2 = m + 1; if (m2 > 12) { m2 = 1; ++y2; } hi = LocalToFt(y2, m2, 1); };
    if (w == L"today") { lo = today; hi = today + kDay; return true; }
    if (w == L"yesterday") { lo = today - kDay; hi = today; return true; }
    if (w == L"thisweek") { lo = today - dow * kDay; hi = lo + 7 * kDay; return true; }
    if (w == L"lastweek") { hi = today - dow * kDay; lo = hi - 7 * kDay; return true; }
    if (w == L"thismonth") { month(now.wYear, now.wMonth); return true; }
    if (w == L"lastmonth") { month(now.wYear, now.wMonth - 1); return true; }
    if (w == L"thisyear") { lo = LocalToFt(now.wYear, 1, 1); hi = LocalToFt(now.wYear + 1, 1, 1); return true; }
    if (w == L"lastyear") { lo = LocalToFt(now.wYear - 1, 1, 1); hi = LocalToFt(now.wYear, 1, 1); return true; }
    int p[3] = {0, 0, 0}, n = 0; size_t k = 0;
    while (k < w.size() && n < 3) {
        if (w[k] < L'0' || w[k] > L'9') return false;
        int x = 0; size_t s0 = k; while (k < w.size() && w[k] >= L'0' && w[k] <= L'9') x = x * 10 + (w[k++] - L'0');
        if (k - s0 > 4) return false;
        p[n++] = x;
        if (k < w.size()) { if (w[k] != L'-' && w[k] != L'/' && w[k] != L'.') return false; ++k; }
    }
    if (k != w.size() || n == 0 || p[0] < 1601) return false;
    if (n == 1) { lo = LocalToFt(p[0], 1, 1); hi = LocalToFt(p[0] + 1, 1, 1); }
    else if (n == 2) { if (p[1] < 1 || p[1] > 12) return false; month(p[0], p[1]); }
    else { if (p[1] < 1 || p[1] > 12 || p[2] < 1 || p[2] > 31) return false; lo = LocalToFt(p[0], p[1], p[2]); hi = lo + kDay; }
    return lo != 0 && hi != 0;
}

// 比较/区间的通用解析：unit(v, lo, hi) 给出值的区间 [lo, hi)
template <class F> bool ParseRange(std::wstring_view v, F unit, uint64_t& lo, uint64_t& hi) {
    size_t dd = v.find(L"..");
    if (dd != std::wstring_view::npos) {
        uint64_t a0, a1, b0, b1;
        bool okA = dd == 0 ? (a0 = 0, a1 = 0, true) : unit(v.substr(0, dd), a0, a1);
        bool okB = dd + 2 == v.size() ? (b0 = UINT64_MAX, b1 = UINT64_MAX, true) : unit(v.substr(dd + 2), b0, b1);
        if (!okA || !okB) return false;
        lo = a0; hi = b1 == UINT64_MAX ? UINT64_MAX : b1 - 1; return true;
    }
    std::wstring_view op;
    for (auto o : {L">=", L"<=", L">", L"<", L"="}) { std::wstring_view ov(o); if (v.substr(0, ov.size()) == ov) { op = ov; break; } }
    uint64_t a, b;
    if (!unit(v.substr(op.size()), a, b)) return false;
    if (op == L">") { lo = b; hi = UINT64_MAX; }
    else if (op == L">=") { lo = a; hi = UINT64_MAX; }
    else if (op == L"<") { lo = 0; hi = a ? a - 1 : 0; if (!a) return false; }
    else if (op == L"<=") { lo = 0; hi = b - 1; }
    else { lo = a; hi = b - 1; }
    return true;
}

bool SizeUnit(std::wstring_view v, uint64_t& a, uint64_t& b) {
    std::wstring w = FoldStr(v);
    struct { const wchar_t* k; uint64_t lo, hi; } kw[] = {
        {L"empty", 0, 1}, {L"tiny", 0, 10ULL << 10}, {L"small", 10ULL << 10, 100ULL << 10}, {L"medium", 100ULL << 10, 1ULL << 20},
        {L"large", 1ULL << 20, 16ULL << 20}, {L"huge", 16ULL << 20, 128ULL << 20}, {L"gigantic", 128ULL << 20, UINT64_MAX}};
    for (auto& k : kw) if (w == k.k) { a = k.lo; b = k.hi; return true; }
    uint64_t x; if (!ParseSize(v, x)) return false;
    a = x; b = x + 1; return true;
}

class Parser {
public:
    Parser(Query& q, std::vector<std::wstring> t) : q_(q), t_(std::move(t)), opts_(q.opts) {}
    int ParseAnd() {
        std::vector<uint32_t> kids;
        while (pos_ < t_.size() && t_[pos_] != L">") {
            if (t_[pos_] == L"|") { ++pos_; continue; }   // 孤立的 |
            int n = ParseOr(); if (n >= 0) kids.push_back(static_cast<uint32_t>(n));
        }
        if (kids.size() == 1) return static_cast<int>(kids[0]);
        if (kids.empty()) return -1;
        return Add(QNode::And, std::move(kids));
    }
private:
    int ParseOr() {
        std::vector<uint32_t> kids;
        int n = ParseUnary(); if (n >= 0) kids.push_back(static_cast<uint32_t>(n));
        while (pos_ < t_.size() && t_[pos_] == L"|") {
            ++pos_;
            if (pos_ >= t_.size() || t_[pos_] == L">") break;
            int m = ParseUnary(); if (m >= 0) kids.push_back(static_cast<uint32_t>(m));
        }
        if (kids.size() == 1) return static_cast<int>(kids[0]);
        if (kids.empty()) return -1;
        return Add(QNode::Or, std::move(kids));
    }
    int ParseUnary() {
        std::wstring tok = t_[pos_];
        bool neg = false;
        while (!tok.empty() && tok[0] == L'!') { neg = !neg; tok.erase(0, 1); }
        int n;
        if (tok == L"<" || (tok.empty() && pos_ + 1 < t_.size() && t_[pos_ + 1] == L"<")) {
            if (tok.empty()) ++pos_;
            ++pos_; n = ParseAnd();
            if (pos_ < t_.size() && t_[pos_] == L">") ++pos_;
        } else { ++pos_; n = Leaf(tok, neg); }
        if (n >= 0 && neg) n = Add(QNode::Not, {static_cast<uint32_t>(n)});
        return n;
    }
    int Leaf(std::wstring tok, bool neg) {
        tok.erase(std::remove(tok.begin(), tok.end(), L'\x01'), tok.end());
        if (tok.empty()) return -1;
        size_t c = tok.find(L':');
        if (c != std::wstring::npos && c > 0) {
            std::wstring fn = FoldStr(std::wstring_view(tok).substr(0, c)), arg = tok.substr(c + 1);
            QNode nd{};
            if (fn == L"ext") {
                nd.kind = QNode::Ext;
                for (size_t b = 0; b <= arg.size();) { size_t e = arg.find(L';', b); if (e == std::wstring::npos) e = arg.size();
                    std::wstring x = FoldStr(std::wstring_view(arg).substr(b, e - b)); if (!x.empty() && x[0] == L'.') x.erase(0, 1);
                    nd.exts.push_back(x); b = e + 1; }
                return AddNode(std::move(nd));
            }
            if (fn == L"size") { nd.kind = QNode::Size; if (ParseRange(arg, SizeUnit, nd.lo, nd.hi)) return AddNode(std::move(nd)); }
            if (fn == L"dm" || fn == L"datemodified") { nd.kind = QNode::Date; if (ParseRange(arg, ParseDate, nd.lo, nd.hi)) return AddNode(std::move(nd)); }
            if (fn == L"file" || fn == L"folder") {
                int f = AddNode(QNode{fn == L"file" ? QNode::IsFile : QNode::IsDir});
                if (arg.empty()) return f;
                int pnode = PatLeaf(arg, false, neg, false);
                return Add(QNode::And, {static_cast<uint32_t>(f), static_cast<uint32_t>(pnode)});
            }
            if (fn == L"path") return PatLeaf(arg, true, neg, false);
            if (fn == L"nopath") return PatLeaf(arg, false, neg, true);   // 强制只匹配名称
            if (fn == L"startwith" || fn == L"startswith") {
                QNode nd{}; nd.kind = QNode::Pat; nd.pat = Compile(arg, opts_.matchCase, false);
                nd.pat.kind = PatKind::Prefix; if (opts_.matchPath) nd.pat.path = true; nd.needPath = nd.pat.path;
                q_.everything = false; if (!neg) q_.highlight.push_back(nd.pat.a); return AddNode(std::move(nd));
            }
            if (fn == L"endwith" || fn == L"endswith") {
                QNode nd{}; nd.kind = QNode::Pat; nd.pat = Compile(arg, opts_.matchCase, false);
                nd.pat.kind = PatKind::Suffix; nd.pat.b = nd.pat.a; nd.pat.a.clear();
                if (opts_.matchPath) nd.pat.path = true; nd.needPath = nd.pat.path;
                q_.everything = false; if (!neg) q_.highlight.push_back(nd.pat.b); return AddNode(std::move(nd));
            }
            if (fn == L"exact" || fn == L"wholefilename" || fn == L"wholefile") {
                QNode nd{}; nd.kind = QNode::Pat; nd.pat = Compile(arg, opts_.matchCase, false);
                nd.pat.kind = PatKind::Exact; if (opts_.matchPath) nd.pat.path = true; nd.needPath = nd.pat.path;
                q_.everything = false; if (!neg) q_.highlight.push_back(nd.pat.a); return AddNode(std::move(nd));
            }
            if (fn == L"parent" || fn == L"infolder") {
                QNode nd{}; nd.kind = QNode::Parent; nd.pat = Compile(arg, opts_.matchCase, opts_.matchWholeWord);
                q_.everything = false; nd.needPath = false; return AddNode(std::move(nd));
            }
            if (fn == L"depth" || fn == L"parents") {
                QNode nd{}; nd.kind = QNode::Depth;
                auto unit = [](std::wstring_view v, uint64_t& a, uint64_t& b) -> bool {
                    uint64_t x = 0; for (wchar_t c : v) { if (c < L'0' || c > L'9') return false; x = x * 10 + (c - L'0'); }
                    a = x; b = x + 1; return true;
                };
                if (ParseRange(arg, unit, nd.lo, nd.hi)) { q_.everything = false; nd.needPath = true; return AddNode(std::move(nd)); }
            }
            if (fn == L"regex") return RegexLeaf(arg);
        }
        return PatLeaf(tok, false, neg, false);
    }
public:
    // 匹配名称；正则含 \\（转义的反斜杠）时匹配完整路径
    int RegexLeaf(const std::wstring& pat) {
        QNode nd{}; nd.kind = QNode::Regex; nd.re = std::make_shared<RegexProg>();
        int ec = 0; PCRE2_SIZE eo = 0;
        uint32_t fl = PCRE2_UTF | PCRE2_MATCH_INVALID_UTF | (opts_.matchCase ? 0 : PCRE2_CASELESS);
        nd.re->code = pcre2_compile_16(reinterpret_cast<PCRE2_SPTR16>(pat.c_str()), pat.size(), fl, &ec, &eo, nullptr);
        if (!nd.re->code) {
            PCRE2_UCHAR16 msg[256] = {}; pcre2_get_error_message_16(ec, msg, 256);
            wchar_t b[64]; swprintf_s(b, L"（位置 %zu）", static_cast<size_t>(eo));
            if (q_.error.empty()) q_.error = L"正则错误：" + std::wstring(reinterpret_cast<const wchar_t*>(msg)) + b;
        } else {
            nd.re->jit = pcre2_jit_compile_16(nd.re->code, PCRE2_JIT_COMPLETE) == 0;
        }
        nd.needPath = pat.find(L"\\\\") != std::wstring::npos;
        q_.hasRegex = true;
        return AddNode(std::move(nd));
    }
private:
    int PatLeaf(const std::wstring& raw, bool forcePath, bool neg, bool forceNoPath) {
        QNode nd{}; nd.kind = QNode::Pat;
        nd.pat = Compile(raw, opts_.matchCase, opts_.matchWholeWord && raw.find_first_of(L"*?") == std::wstring::npos);
        if (forcePath || (opts_.matchPath && !forceNoPath)) nd.pat.path = true;
        if (forceNoPath) nd.pat.path = false;
        nd.needPath = nd.pat.path;
        if (nd.pat.kind != PatKind::All) q_.everything = false;
        if (!neg) q_.highlight.push_back(nd.pat.a.empty() ? nd.pat.b : nd.pat.a);
        return AddNode(std::move(nd));
    }
    int AddNode(QNode n) { if (n.kind != QNode::Pat) q_.everything = false; q_.nodes.push_back(std::move(n)); return static_cast<int>(q_.nodes.size() - 1); }
    int Add(QNode::Kind k, std::vector<uint32_t> kids) { QNode n{}; n.kind = k; n.kids = std::move(kids); return AddNode(std::move(n)); }
    Query& q_; std::vector<std::wstring> t_; MatchOpts opts_; size_t pos_ = 0;
};

// 子树是否需要完整路径；AND/OR 子项把便宜的排前面
bool Prepare(Query& q, uint32_t i) {
    QNode& n = q.nodes[i];
    bool need = n.needPath;
    for (uint32_t k : n.kids) need |= Prepare(q, k);
    n.needPath = need;
    if (n.kind == QNode::And || n.kind == QNode::Or)
        std::stable_sort(n.kids.begin(), n.kids.end(), [&](uint32_t a, uint32_t b) { return q.nodes[a].needPath < q.nodes[b].needPath; });
    return need;
}

struct EvalCtx {
    const IndexData& d; uint32_t i; std::wstring_view name; std::vector<wchar_t>& full; pcre2_match_data_16* md = nullptr; size_t flen = SIZE_MAX;
    std::wstring_view Full() { if (flen == SIZE_MAX) { if (full.empty()) full.resize(32768); flen = d.FullPathInto(i, full.data(), full.size()); } return {full.data(), flen}; }
};

bool Eval(const Query& q, uint32_t ni, EvalCtx& c) {
    const QNode& n = q.nodes[ni];
    const Entry& e = c.d.entries[c.i];
    switch (n.kind) {
    case QNode::And: for (uint32_t k : n.kids) if (!Eval(q, k, c)) return false; return true;
    case QNode::Or: for (uint32_t k : n.kids) if (Eval(q, k, c)) return true; return false;
    case QNode::Not: return !Eval(q, n.kids[0], c);
    case QNode::Pat: return n.pat.path ? Match(c.Full(), n.pat) : Match(c.name, n.pat);
    case QNode::Regex: {
        if (!n.re->code || !c.md) return false;
        std::wstring_view s = n.needPath ? c.Full() : c.name;
        auto sp = reinterpret_cast<PCRE2_SPTR16>(s.data());
        int rc = n.re->jit ? pcre2_jit_match_16(n.re->code, sp, s.size(), 0, 0, c.md, nullptr)
                           : pcre2_match_16(n.re->code, sp, s.size(), 0, 0, c.md, nullptr);
        return rc >= 0;
    }
    case QNode::IsFile: return !e.IsDir();
    case QNode::IsDir: return e.IsDir();
    case QNode::Size: return !e.IsDir() && e.size >= n.lo && e.size <= n.hi;
    case QNode::Date: return e.mtime >= n.lo && e.mtime <= n.hi;
    case QNode::Ext: {
        if (e.IsDir()) return false;
        size_t dot = c.name.rfind(L'.');
        std::wstring_view ext = dot == std::wstring_view::npos ? std::wstring_view{} : c.name.substr(dot + 1);
        for (auto& x : n.exts) {
            if (x.size() != ext.size()) continue;
            size_t k = 0; while (k < x.size() && Fold(ext[k]) == x[k]) ++k;
            if (k == x.size()) return true;
        }
        return false;
    }
    case QNode::Parent: {
        if (e.parent == kNoParent) return Match(std::wstring_view{}, n.pat);  // 卷根：空父名
        return Match(c.d.Name(e.parent), n.pat);
    }
    case QNode::Depth: {
        // 深度 = 完整路径中 \ 的个数（T: 为 0，T:\Docs 为 1）
        auto fp = c.Full();
        uint64_t dpt = 0; for (wchar_t ch : fp) if (ch == L'\\') ++dpt;
        return dpt >= n.lo && dpt <= n.hi;
    }
    }
    return false;
}
} // namespace

std::shared_ptr<Query> CompileQuery(const std::wstring& text, MatchOpts opts, const std::wstring& extra) {
    InitFold();
    auto q = std::make_shared<Query>();
    q->opts = opts;
    if (opts.matchRegex) {
        std::vector<uint32_t> kids;
        if (!text.empty()) { Parser rp(*q, {}); kids.push_back(static_cast<uint32_t>(rp.RegexLeaf(text))); q->everything = false; }
        if (!extra.empty()) { Parser ep(*q, Tokenize(extra)); int r = ep.ParseAnd(); if (r >= 0) kids.push_back(static_cast<uint32_t>(r)); }
        if (kids.size() == 1) q->root = static_cast<int>(kids[0]);
        else if (kids.size() > 1) { QNode a{}; a.kind = QNode::And; a.kids = kids; q->nodes.push_back(std::move(a)); q->root = static_cast<int>(q->nodes.size() - 1); }
        if (q->root >= 0) { q->everything = false; Prepare(*q, static_cast<uint32_t>(q->root)); } else q->everything = true;
        return q;
    }
    std::wstring combined = extra.empty() ? text : (text.find_first_not_of(L" \t") == std::wstring::npos ? extra : L"<" + text + L"> " + extra);
    Parser p(*q, Tokenize(combined));
    q->root = p.ParseAnd();
    if (q->hasRegex) q->everything = false;
    if (q->root >= 0) Prepare(*q, static_cast<uint32_t>(q->root));
    if (q->root < 0) q->everything = true;
    return q;
}

const std::vector<std::wstring>& QueryHighlight(const Query& q) { return q.highlight; }
const std::wstring& QueryError(const Query& q) { return q.error; }

bool SearchQuery(const IndexData& d, const Query& q, std::vector<uint32_t>& out, const std::function<bool()>& cancelled) {
    const auto& order = d.byName;
    out.clear();
    if (!q.error.empty()) return true;   // 正则错误：无结果
    if (q.everything || q.root < 0) { out = order; return true; }
    const size_t n = order.size();
    unsigned nt = std::max(1u, std::min(std::thread::hardware_concurrency(), 16u));
    if (n < 100000) nt = 1;
    std::vector<std::vector<uint32_t>> parts(nt);
    std::atomic<bool> stop{false};
    const uint32_t root = static_cast<uint32_t>(q.root);
    auto work = [&](unsigned t) {
        size_t b = n * t / nt, e = n * (t + 1) / nt;
        auto& o = parts[t];
        std::vector<wchar_t> full;
        pcre2_match_data_16* md = q.hasRegex ? pcre2_match_data_create_16(1, nullptr) : nullptr;
        struct MdFree { pcre2_match_data_16* m; ~MdFree() { if (m) pcre2_match_data_free_16(m); } } mdf{md};
        for (size_t k = b; k < e; ++k) {
            if ((k & 0x3FFF) == 0 && (stop || cancelled())) { stop = true; return; }
            uint32_t i = order[k];
            EvalCtx c{d, i, d.Name(i), full, md};
            if (Eval(q, root, c)) o.push_back(i);
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

// 旧接口：已折叠的词做 AND（测试使用）
bool SearchCore(const IndexData& d, const std::vector<std::wstring>& terms, std::vector<uint32_t>& out,
                const std::function<bool()>& cancelled) {
    Query q;
    std::vector<uint32_t> kids;
    for (auto& t : terms) {
        QNode n{}; n.kind = QNode::Pat; n.pat = Compile(t); n.needPath = n.pat.path;
        if (n.pat.kind != PatKind::All) q.everything = false;
        q.nodes.push_back(std::move(n)); kids.push_back(static_cast<uint32_t>(q.nodes.size() - 1));
    }
    if (!kids.empty()) { QNode a{}; a.kind = QNode::And; a.kids = kids; q.nodes.push_back(std::move(a)); q.root = static_cast<int>(q.nodes.size() - 1); Prepare(q, static_cast<uint32_t>(q.root)); }
    return SearchQuery(d, q, out, cancelled);
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

void SortItems(const IndexData& d, std::vector<uint32_t>& items, SortSpec s,
               const std::vector<uint32_t>* folderRank, bool nameOrdered) {
    const size_t n = items.size();
    if (s.col == SortCol::Name && nameOrdered) {
        if (s.desc) std::reverse(items.begin(), items.end());
        return;
    }
    // 名称：不假设输入已按名称排列；键 = 该对象在 byName（CompareFold 排好的全局名称序）中的位置
    std::vector<uint32_t> namePos;
    if (s.col == SortCol::Name && n * 64 < d.byName.size()) {   // 结果少：直接比较名称（与 byName 同序：折叠比较，同名按 id）
        auto lt = [&](uint32_t a, uint32_t b) { int c = CompareFold(d.Name(a), d.Name(b)); return c != 0 ? c < 0 : a < b; };
        if (s.desc) std::sort(items.begin(), items.end(), [&](uint32_t a, uint32_t b) { return lt(b, a); });
        else std::sort(items.begin(), items.end(), lt);
        return;
    }
    if (s.col == SortCol::Name) {
        namePos.assign(d.entries.size(), UINT32_MAX);
        for (size_t k = 0; k < d.byName.size(); ++k) namePos[d.byName[k]] = static_cast<uint32_t>(k);
    }
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
            case SortCol::Size: key = en.IsDir() ? 0 : en.size + 1; break;
            case SortCol::Date: key = en.mtime; break;
            case SortCol::Name: key = namePos[i]; break;
            case SortCol::Ext: {
                // 键：扩展名折叠序（按字符打包进 uint64，超过的截断；同扩展名保持原序）
                auto nm = d.Name(i);
                size_t dot = en.IsDir() ? std::wstring_view::npos : nm.rfind(L'.');
                uint64_t h = 0;
                if (dot != std::wstring_view::npos && dot + 1 < nm.size()) {
                    for (size_t k = dot + 1; k < nm.size() && k < dot + 1 + 8; ++k) h = (h << 8) | (Fold(nm[k]) & 0xFF);
                }
                key = h;
                break;
            }
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

void Searcher::Request(const std::wstring& query, std::shared_ptr<const IndexData> data, SortSpec sort, MatchOpts opts, std::wstring filter) {
    if (!data) return;
    { std::lock_guard lk(mu_); query_ = query; filter_ = std::move(filter); opts_ = opts; data_ = std::move(data); sort_ = sort; pending_ = true; ++gen_; }
    cv_.notify_all();
}

std::shared_ptr<SearchResult> Searcher::Latest() { std::lock_guard lk(mu_); return latest_; }

void Searcher::Loop() {
    for (;;) {
        std::wstring q, flt; MatchOpts ox; std::shared_ptr<const IndexData> d; uint64_t g; SortSpec so;
        {
            std::unique_lock lk(mu_);
            cv_.wait(lk, [&] { return quit_ || pending_; });
            if (quit_) return;
            pending_ = false; q = query_; flt = filter_; ox = opts_; d = data_; g = gen_; so = sort_;
        }
        if (!d) continue;
        auto t0 = std::chrono::steady_clock::now();
        auto res = std::make_shared<SearchResult>();
        auto cq = CompileQuery(q, ox, flt);
        res->generation = g; res->data = d; res->terms = cq->highlight; res->sort = so; res->error = cq->error;
        // 复用键：选项变化也必须重搜
        wchar_t ob[8]; swprintf_s(ob, L"%d%d%d%d", ox.matchCase, ox.matchWholeWord, ox.matchPath, ox.matchRegex);
        q = std::wstring(ob) + L"\x04" + flt + L"\x05" + q;
        if (d == lastD_ && q == lastQ_) res->items = lastItems_;   // 只改了排序
        else {
            if (!SearchQuery(*d, *cq, res->items, [&] { return gen_ != g; })) continue;
            lastD_ = d; lastQ_ = q; lastItems_ = res->items;
        }
        if (so.col == SortCol::Path && rankFor_ != d.get()) { rank_ = BuildFolderPathRank(*d); rankFor_ = d.get(); rankHold_ = d; }
        if (gen_ != g) continue;
        // lastItems_ 缓存排序前的匹配结果；新搜索和仅切换排序均保持 byName 顺序。
        SortItems(*d, res->items, so, so.col == SortCol::Path ? &rank_ : nullptr, true);
        res->elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        { std::lock_guard lk(mu_); if (g != gen_) continue; latest_ = res; }
        if (hwnd_) PostMessageW(hwnd_, WM_APP_RESULTS, static_cast<WPARAM>(g), 0);
    }
}

} // namespace ff
