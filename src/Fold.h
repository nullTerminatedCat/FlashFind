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

// ASCII 快速路径（fast_ascii_search=1）：term 全为 ASCII 时，ASCII 字符用寄存器内运算折叠，
// 只有非 ASCII 字符才查 64K 表（保持与 Fold 完全一致的结果，例如 U+212A 开尔文符号仍折叠为 k）。
extern bool g_fastAscii;
inline bool IsAscii(std::wstring_view s) { for (wchar_t c : s) if (c >= 128) return false; return true; }
inline wchar_t FoldA(wchar_t c) {
    if (c < 128) return static_cast<wchar_t>(c - L'A' < 26u ? c | 0x20 : c);
    return Fold(c);
}
inline bool ContainsFoldAscii(std::wstring_view s, std::wstring_view term) {
    const size_t m = term.size(), n = s.size();
    if (m == 0) return true;
    if (m > n) return false;
    const wchar_t t0 = term[0];
    const wchar_t* p = s.data();
    for (size_t i = 0, last = n - m; i <= last; ++i) {
        if (FoldA(p[i]) != t0) continue;
        size_t k = 1;
        while (k < m && FoldA(p[i + k]) == term[k]) ++k;
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
