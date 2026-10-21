#pragma once
// 大小写折叠表：64K 项 uint16，启动时用 CharLowerBuffW 生成一次（约 0.3 ms）。
// 名称池只保存原始 UTF-16，比较时逐字符查表折叠，不转码、不复制。
#include <windows.h>
#include <cstdint>
#include <string_view>

namespace ff {

extern uint16_t g_fold[65536];
void InitFold();

inline wchar_t Fold(wchar_t c) { return static_cast<wchar_t>(g_fold[static_cast<uint16_t>(c)]); }

// term 必须已折叠
inline bool ContainsFold(std::wstring_view s, std::wstring_view term) {
    const size_t m = term.size(), n = s.size();
    if (m == 0) return true;
    if (m > n) return false;
    const wchar_t t0 = term[0];
    const wchar_t* p = s.data();
    for (size_t i = 0, last = n - m; i <= last; ++i) {
        if (Fold(p[i]) != t0) continue;
        size_t k = 1;
        while (k < m && Fold(p[i + k]) == term[k]) ++k;
        if (k == m) return true;
    }
    return false;
}

// 返回下一个匹配位置（npos 表示无）
inline size_t FindFold(std::wstring_view s, std::wstring_view term, size_t from) {
    const size_t m = term.size(), n = s.size();
    if (m == 0 || m > n) return std::wstring_view::npos;
    for (size_t i = from; i + m <= n; ++i) {
        size_t k = 0;
        while (k < m && Fold(s[i + k]) == term[k]) ++k;
        if (k == m) return i;
    }
    return std::wstring_view::npos;
}

inline int CompareFold(std::wstring_view a, std::wstring_view b) {
    size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
        wchar_t x = Fold(a[i]), y = Fold(b[i]);
        if (x != y) return x < y ? -1 : 1;
    }
    return a.size() < b.size() ? -1 : (a.size() > b.size() ? 1 : 0);
}

} // namespace ff
