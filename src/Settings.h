#pragma once
#include <string>
#include <vector>

namespace ff {

enum class ThemeMode { System = 0, Light = 1, Dark = 2 };

struct Settings {
    // 窗口
    int x = CW_USEDEFAULT_SENTINEL, y = 0, w = 900, h = 600; // 逻辑像素
    bool maximized = false;
    int colName = 260, colPath = 380, colSize = 90, colDate = 140;
    // 行为
    bool escHides = true;
    bool closeToTray = true;
    bool selectAllOnShow = true;
    // 热键
    std::wstring toggleKey = L"Ctrl+Alt+Space";
    bool doubleCtrl = true;
    // 外观
    ThemeMode theme = ThemeMode::System;
    bool compressDb = false;   // 数据库是否 XPRESS_HUFF 压缩
    bool ntfsVolumeMonitors = true;       // ntfs_volume_monitors：持续读 USN
    bool ntfsLoadRecentChanges = false;   // ntfs_volume_load_recent_changes（预留：最近更改列表）
    bool ntfsEnableUsnJournal = true;     // ntfs_enable_usn_journal：Journal 未启用时创建（目前全卷共用）
    bool fastAsciiSearch = true;          // fast_ascii_search
    // 索引：空 = 所有固定磁盘
    std::vector<std::wstring> indexRoots;
    std::vector<std::wstring> excludeFolders = {L"C:\\Windows\\WinSxS", L"C:\\$Recycle.Bin", L"C:\\System Volume Information"};

    static constexpr int CW_USEDEFAULT_SENTINEL = -32768;

    std::wstring IniPath() const;
    void Load();
    void Save() const;
};

Settings& GetSettings();

} // namespace ff
