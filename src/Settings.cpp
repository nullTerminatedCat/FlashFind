#include "Settings.h"
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
    wchar_t buf[4096];
    GetPrivateProfileStringW(sec, key, def.c_str(), buf, 4096, ini.c_str());
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

void Settings::Load() {
    const auto ini = IniPath();
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
    escHides = ReadInt(ini, L"behavior", L"esc_hides_window", 1) != 0;
    closeToTray = ReadInt(ini, L"behavior", L"close_to_tray", 1) != 0;
    selectAllOnShow = ReadInt(ini, L"behavior", L"select_all_on_show", 1) != 0;
    toggleKey = ReadStr(ini, L"hotkey", L"toggle_window_key", toggleKey);
    doubleCtrl = ReadInt(ini, L"hotkey", L"toggle_window_double_ctrl", 1) != 0;
    theme = static_cast<ThemeMode>(ReadInt(ini, L"ui", L"theme", 0));
    indexRoots = Split(ReadStr(ini, L"index", L"roots", L""));
    auto ex = ReadStr(ini, L"index", L"exclude_folders", L"\x01");
    if (ex != L"\x01") excludeFolders = Split(ex);
}

void Settings::Save() const {
    const auto ini = IniPath();
    WriteInt(ini, L"window", L"x", x); WriteInt(ini, L"window", L"y", y);
    WriteInt(ini, L"window", L"w", w); WriteInt(ini, L"window", L"h", h);
    WriteInt(ini, L"window", L"maximized", maximized);
    WriteInt(ini, L"columns", L"name", colName); WriteInt(ini, L"columns", L"path", colPath);
    WriteInt(ini, L"columns", L"size", colSize); WriteInt(ini, L"columns", L"date_modified", colDate);
    WriteInt(ini, L"behavior", L"esc_hides_window", escHides);
    WriteInt(ini, L"behavior", L"close_to_tray", closeToTray);
    WriteInt(ini, L"behavior", L"select_all_on_show", selectAllOnShow);
    WriteStr(ini, L"hotkey", L"toggle_window_key", toggleKey);
    WriteInt(ini, L"hotkey", L"toggle_window_double_ctrl", doubleCtrl);
    WriteInt(ini, L"ui", L"theme", static_cast<int>(theme));
    WriteStr(ini, L"index", L"roots", Join(indexRoots));
    WriteStr(ini, L"index", L"exclude_folders", Join(excludeFolders));
}

} // namespace ff
