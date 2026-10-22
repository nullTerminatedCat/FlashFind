#include "History.h"
#include "Settings.h"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>

namespace ff {

std::wstring HistoryPath() {
    wchar_t dir[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, dir))) return L"";
    return std::wstring(dir) + L"\\FlashFind\\history.txt";
}

static void EnsureParent(const std::wstring& path) {
    auto slash = path.find_last_of(L'\\');
    if (slash != std::wstring::npos) {
        std::wstring dir = path.substr(0, slash);
        CreateDirectoryW(dir.c_str(), nullptr);
    }
}

std::vector<std::wstring> LoadSearchHistory() {
    std::vector<std::wstring> out;
    HANDLE h = CreateFileW(HistoryPath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;
    LARGE_INTEGER sz{}; GetFileSizeEx(h, &sz);
    if (sz.QuadPart < 2 || sz.QuadPart > 4 << 20) { CloseHandle(h); return out; }
    std::wstring buf(static_cast<size_t>(sz.QuadPart / 2), L'\0');
    DWORD rd = 0; ReadFile(h, buf.data(), static_cast<DWORD>(sz.QuadPart), &rd, nullptr); CloseHandle(h);
    size_t i = 0;
    if (!buf.empty() && buf[0] == 0xFEFF) ++i;
    std::wstring line;
    for (; i < buf.size(); ++i) {
        wchar_t c = buf[i];
        if (c == L'\r') continue;
        if (c == L'\n') { if (!line.empty()) out.push_back(line); line.clear(); }
        else line += c;
    }
    if (!line.empty()) out.push_back(line);
    return out;
}

void SaveSearchHistory(const std::vector<std::wstring>& items) {
    auto path = HistoryPath();
    if (path.empty()) return;
    EnsureParent(path);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    const wchar_t bom = 0xFEFF; DWORD wr = 0;
    WriteFile(h, &bom, 2, &wr, nullptr);
    for (auto& s : items) {
        WriteFile(h, s.c_str(), static_cast<DWORD>(s.size() * 2), &wr, nullptr);
        WriteFile(h, L"\r\n", 4, &wr, nullptr);
    }
    CloseHandle(h);
}

void AddSearchHistory(const std::wstring& q) {
    if (!GetSettings().historyEnabled) return;
    std::wstring t = q;
    while (!t.empty() && (t.back() == L' ' || t.back() == L'\t')) t.pop_back();
    size_t a = 0; while (a < t.size() && (t[a] == L' ' || t[a] == L'\t')) ++a;
    t = t.substr(a);
    if (t.empty()) return;
    auto items = LoadSearchHistory();
    items.erase(std::remove_if(items.begin(), items.end(), [&](const std::wstring& x) {
        return _wcsicmp(x.c_str(), t.c_str()) == 0;
    }), items.end());
    items.insert(items.begin(), t);
    int maxn = GetSettings().historyMax;
    if (static_cast<int>(items.size()) > maxn) items.resize(static_cast<size_t>(maxn));
    SaveSearchHistory(items);
}

void ClearSearchHistory() { SaveSearchHistory({}); }

} // namespace ff
