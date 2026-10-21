#include "Settings.h"
#include <algorithm>
#include <windows.h>
#include <shlobj.h>
#include <sstream>

namespace ff {

Settings& GetSettings() { static Settings s; return s; }

std::wstring Settings::IniPath() const {
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) { dir = p; CoTaskMemFree(p); }
    dir += L"\\FlashFind";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\FlashFind.ini";
}

static std::wstring ReadStr(const std::wstring& ini, const wchar_t* sec, const wchar_t* key, const std::wstring& def) {
    std::wstring buf(32768, L'\0');
    DWORD n = GetPrivateProfileStringW(sec, key, def.c_str(), buf.data(), 32768, ini.c_str());
    buf.resize(n);
    return buf;
}
static int ReadInt(const std::wstring& ini, const wchar_t* sec, const wchar_t* key, int def) {
    return static_cast<int>(GetPrivateProfileIntW(sec, key, def, ini.c_str()));
}
static void WriteStr(const std::wstring& ini, const wchar_t* sec, const wchar_t* key, const std::wstring& v) {
    WritePrivateProfileStringW(sec, key, v.c_str(), ini.c_str());
}
static void WriteInt(const std::wstring& ini, const wchar_t* sec, const wchar_t* key, int v) {
    WriteStr(ini, sec, key, std::to_wstring(v));
}
static std::vector<std::wstring> Split(const std::wstring& s) {
    std::vector<std::wstring> r; std::wstringstream ss(s); std::wstring t;
    while (std::getline(ss, t, L';')) if (!t.empty()) r.push_back(t);
    return r;
}
static std::wstring Join(const std::vector<std::wstring>& v) {
    std::wstring r; for (auto& s : v) { if (!r.empty()) r += L';'; r += s; } return r;
}

std::vector<std::wstring> Settings::SplitArray(const std::wstring& s) {
    std::vector<std::wstring> r;
    if (s.empty()) return r;
    size_t i = 0;
    for (;;) {
        std::wstring item;
        if (i < s.size() && s[i] == L'"') {
            ++i;
            while (i < s.size()) {
                if (s[i] == L'"') { if (i + 1 < s.size() && s[i + 1] == L'"') { item += L'"'; i += 2; continue; } ++i; break; }
                item += s[i++];
            }
            while (i < s.size() && s[i] != L',') ++i;
        } else {
            while (i < s.size() && s[i] != L',') item += s[i++];
        }
        r.push_back(std::move(item));
        if (i >= s.size()) break;
        ++i;   // ','
    }
    return r;
}

std::wstring Settings::JoinArray(const std::vector<std::wstring>& v) {
    std::wstring r;
    for (size_t k = 0; k < v.size(); ++k) {
        if (k) r += L',';
        if (v[k].find_first_of(L",\"") != std::wstring::npos) {
            r += L'"';
            for (wchar_t c : v[k]) { if (c == L'"') r += L'"'; r += c; }
            r += L'"';
        } else r += v[k];
    }
    return r;
}

static std::vector<VolumeConfig> DetectNtfsVolumes() {
    std::vector<VolumeConfig> r;
    DWORD mask = GetLogicalDrives();
    for (int d = 0; d < 26; ++d) {
        if (!(mask & (1u << d))) continue;
        wchar_t root[] = {static_cast<wchar_t>(L'A' + d), L':', L'\\', 0};
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
        wchar_t fs[32] = {};
        if (!GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, nullptr, fs, 32) || wcscmp(fs, L"NTFS")) continue;
        VolumeConfig v; v.path.assign(root, 2);
        wchar_t g[64] = {};
        if (GetVolumeNameForVolumeMountPointW(root, g, 64)) v.guid = g;
        r.push_back(std::move(v));
    }
    return r;
}

void Settings::LoadFrom(const std::wstring& ini, bool detect) {
    // GetPrivateProfileInt 不支持负数，窗口坐标按字符串读
    x = _wtoi(ReadStr(ini, L"window", L"x", std::to_wstring(CW_USEDEFAULT_SENTINEL)).c_str());
    y = _wtoi(ReadStr(ini, L"window", L"y", L"0").c_str());
    w = ReadInt(ini, L"window", L"w", w);
    h = ReadInt(ini, L"window", L"h", h);
    maximized = ReadInt(ini, L"window", L"maximized", 0) != 0;
    colName = ReadInt(ini, L"columns", L"name", colName);
    colPath = ReadInt(ini, L"columns", L"path", colPath);
    colSize = ReadInt(ini, L"columns", L"size", colSize);
    colDate = ReadInt(ini, L"columns", L"date_modified", colDate);
    colVisible[1] = ReadInt(ini, L"columns", L"path_visible", 1) != 0;
    colVisible[2] = ReadInt(ini, L"columns", L"size_visible", 1) != 0;
    colVisible[3] = ReadInt(ini, L"columns", L"date_modified_visible", 1) != 0;
    allowRename = ReadInt(ini, L"general", L"allow_rename", 1) != 0;
    singleParentContextMenu = ReadInt(ini, L"general", L"single_parent_context_menu", 1) != 0;
    filter = ReadInt(ini, L"search", L"filter", 0);
    matchRegex = ReadInt(ini, L"search", L"match_regex", 0) != 0;
    sortCol = std::clamp(ReadInt(ini, L"columns", L"sort", 0), 0, 3);
    sortDesc = ReadInt(ini, L"columns", L"sort_ascending", 1) == 0;
    escHides = ReadInt(ini, L"behavior", L"esc_hides_window", 1) != 0;
    closeToTray = ReadInt(ini, L"behavior", L"close_to_tray", 1) != 0;
    selectAllOnShow = ReadInt(ini, L"behavior", L"select_all_on_show", 1) != 0;
    toggleKey = ReadStr(ini, L"hotkey", L"toggle_window_key", toggleKey);
    doubleCtrl = ReadInt(ini, L"hotkey", L"toggle_window_double_ctrl", 1) != 0;
    theme = static_cast<ThemeMode>(ReadInt(ini, L"ui", L"theme", 0));
    compressDb = ReadInt(ini, L"index", L"compress_db", 0) != 0;
    fastAsciiSearch = ReadInt(ini, L"search", L"fast_ascii_search", 1) != 0;
    monitorUpdateDelay = ReadInt(ini, L"index", L"monitor_update_delay", 1000);
    {
        auto paths = SplitArray(ReadStr(ini, L"index", L"ntfs_volume_paths", L""));
        auto guids = SplitArray(ReadStr(ini, L"index", L"ntfs_volume_guids", L""));
        auto incs = SplitArray(ReadStr(ini, L"index", L"ntfs_volume_includes", L""));
        auto mons = SplitArray(ReadStr(ini, L"index", L"ntfs_volume_monitors", L""));
        auto recs = SplitArray(ReadStr(ini, L"index", L"ntfs_volume_load_recent_changes", L""));
        auto only = SplitArray(ReadStr(ini, L"index", L"ntfs_volume_include_onlys", L""));
        auto jrn = SplitArray(ReadStr(ini, L"index", L"ntfs_volume_enable_usn_journals", L""));
        auto flag = [](const std::vector<std::wstring>& a, size_t i, bool def) { return i < a.size() && !a[i].empty() ? _wtoi(a[i].c_str()) != 0 : def; };
        ntfsVolumes.clear();
        for (size_t i = 0; i < paths.size(); ++i) {
            if (paths[i].empty()) continue;
            VolumeConfig v; v.path = paths[i];
            v.guid = i < guids.size() ? guids[i] : L"";
            v.include = flag(incs, i, true); v.monitor = flag(mons, i, true);
            v.loadRecentChanges = flag(recs, i, false); v.enableUsnJournal = flag(jrn, i, true);
            if (i < only.size()) v.includeOnly = Split(only[i]);
            ntfsVolumes.push_back(std::move(v));
        }
        if (detect)
            for (auto& d : DetectNtfsVolumes()) {
                bool known = false;
                for (auto& v : ntfsVolumes) if (_wcsicmp(v.path.c_str(), d.path.c_str()) == 0) { known = true; if (v.guid.empty()) v.guid = d.guid; }
                if (!known) ntfsVolumes.push_back(std::move(d));
            }
    }
    indexRoots = Split(ReadStr(ini, L"index", L"roots", L""));
    auto ex = ReadStr(ini, L"index", L"exclude_folders", L"\x01");
    if (ex != L"\x01") excludeFolders = Split(ex);
}

void Settings::SaveTo(const std::wstring& ini) const {
    WriteInt(ini, L"window", L"x", x); WriteInt(ini, L"window", L"y", y);
    WriteInt(ini, L"window", L"w", w); WriteInt(ini, L"window", L"h", h);
    WriteInt(ini, L"window", L"maximized", maximized);
    WriteInt(ini, L"columns", L"name", colName); WriteInt(ini, L"columns", L"path", colPath);
    WriteInt(ini, L"columns", L"size", colSize); WriteInt(ini, L"columns", L"date_modified", colDate);
    WriteInt(ini, L"columns", L"path_visible", colVisible[1]); WriteInt(ini, L"columns", L"size_visible", colVisible[2]);
    WriteInt(ini, L"columns", L"date_modified_visible", colVisible[3]);
    WriteInt(ini, L"general", L"allow_rename", allowRename);
    WriteInt(ini, L"general", L"single_parent_context_menu", singleParentContextMenu);
    WriteInt(ini, L"search", L"filter", filter);
    WriteInt(ini, L"search", L"match_regex", matchRegex);
    WriteInt(ini, L"columns", L"sort", sortCol); WriteInt(ini, L"columns", L"sort_ascending", !sortDesc);
    WriteInt(ini, L"behavior", L"esc_hides_window", escHides);
    WriteInt(ini, L"behavior", L"close_to_tray", closeToTray);
    WriteInt(ini, L"behavior", L"select_all_on_show", selectAllOnShow);
    WriteStr(ini, L"hotkey", L"toggle_window_key", toggleKey);
    WriteInt(ini, L"hotkey", L"toggle_window_double_ctrl", doubleCtrl);
    WriteInt(ini, L"ui", L"theme", static_cast<int>(theme));
    WriteInt(ini, L"index", L"compress_db", compressDb);
    WriteInt(ini, L"search", L"fast_ascii_search", fastAsciiSearch);
    WriteInt(ini, L"index", L"monitor_update_delay", monitorUpdateDelay);
    {
        std::vector<std::wstring> paths, guids, incs, mons, recs, only, jrn;
        for (auto& v : ntfsVolumes) {
            paths.push_back(v.path); guids.push_back(v.guid);
            incs.push_back(v.include ? L"1" : L"0"); mons.push_back(v.monitor ? L"1" : L"0");
            recs.push_back(v.loadRecentChanges ? L"1" : L"0"); jrn.push_back(v.enableUsnJournal ? L"1" : L"0");
            only.push_back(Join(v.includeOnly));
        }
        // 整个值包一层引号：GetPrivateProfileString 会去掉最外层的一对引号，保护内部的引号
        auto q = [](const std::wstring& s) { return L"\"" + s + L"\""; };
        WriteStr(ini, L"index", L"ntfs_volume_paths", q(JoinArray(paths)));
        WriteStr(ini, L"index", L"ntfs_volume_guids", q(JoinArray(guids)));
        WriteStr(ini, L"index", L"ntfs_volume_includes", q(JoinArray(incs)));
        WriteStr(ini, L"index", L"ntfs_volume_monitors", q(JoinArray(mons)));
        WriteStr(ini, L"index", L"ntfs_volume_load_recent_changes", q(JoinArray(recs)));
        WriteStr(ini, L"index", L"ntfs_volume_include_onlys", q(JoinArray(only)));
        WriteStr(ini, L"index", L"ntfs_volume_enable_usn_journals", q(JoinArray(jrn)));
    }
    WriteStr(ini, L"index", L"roots", Join(indexRoots));
    WriteStr(ini, L"index", L"exclude_folders", Join(excludeFolders));
}

} // namespace ff
