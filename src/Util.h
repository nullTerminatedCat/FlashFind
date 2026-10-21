#pragma once
#include <windows.h>
#include <string>
#include <cwctype>

namespace ff {

inline std::wstring ToLower(std::wstring_view s) {
    std::wstring r(s);
    if (!r.empty()) CharLowerBuffW(r.data(), static_cast<DWORD>(r.size()));
    return r;
}

inline std::wstring FormatSize(unsigned long long bytes) {
    wchar_t buf[64];
    if (bytes < 1024ULL * 1024 * 1024) {
        unsigned long long kb = (bytes + 1023) / 1024;
        // 千分位
        std::wstring n = std::to_wstring(kb);
        for (int i = static_cast<int>(n.size()) - 3; i > 0; i -= 3) n.insert(static_cast<size_t>(i), L",");
        return n + L" KB";
    }
    swprintf_s(buf, L"%.2f GB", bytes / (1024.0 * 1024 * 1024));
    return buf;
}

inline std::wstring FormatTime(unsigned long long ft) {
    if (!ft) return {};
    FILETIME f{static_cast<DWORD>(ft), static_cast<DWORD>(ft >> 32)}, lf;
    SYSTEMTIME st;
    FileTimeToLocalFileTime(&f, &lf);
    FileTimeToSystemTime(&lf, &st);
    wchar_t buf[32];
    swprintf_s(buf, L"%04u/%02u/%02u %02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
}

inline std::wstring WithThousands(size_t v) {
    std::wstring n = std::to_wstring(v);
    for (int i = static_cast<int>(n.size()) - 3; i > 0; i -= 3) n.insert(static_cast<size_t>(i), L",");
    return n;
}

// 无分配版本（绘制路径使用）
inline int FormatSizeInto(unsigned long long bytes, wchar_t* buf, size_t cap) {
    if (bytes >= 1024ULL * 1024 * 1024) return swprintf_s(buf, cap, L"%.2f GB", bytes / (1024.0 * 1024 * 1024));
    unsigned long long kb = (bytes + 1023) / 1024;
    wchar_t tmp[32]; int n = swprintf_s(tmp, L"%llu", kb), o = 0;
    for (int k = 0; k < n && static_cast<size_t>(o) + 5 < cap; ++k) {
        if (k && (n - k) % 3 == 0) buf[o++] = L',';
        buf[o++] = tmp[k];
    }
    buf[o] = 0;
    return o + swprintf_s(buf + o, cap - static_cast<size_t>(o), L" KB");
}

inline int FormatTimeInto(unsigned long long ft, wchar_t* buf, size_t cap) {
    if (!ft) { buf[0] = 0; return 0; }
    FILETIME f{static_cast<DWORD>(ft), static_cast<DWORD>(ft >> 32)}, lf;
    SYSTEMTIME st;
    FileTimeToLocalFileTime(&f, &lf);
    FileTimeToSystemTime(&lf, &st);
    return swprintf_s(buf, cap, L"%04u/%02u/%02u %02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
}

} // namespace ff
