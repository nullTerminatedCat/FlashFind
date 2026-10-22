#pragma once
#include "Index.h"
#include <string>
#include <vector>

namespace ff {

enum class ThemeMode { System = 0, Light = 1, Dark = 2 };

struct Settings {
    // 窗口
    int x = CW_USEDEFAULT_SENTINEL, y = 0, w = 900, h = 600; // 逻辑像素
    bool maximized = false;
    int colName = 260, colPath = 380, colSize = 90, colDate = 140;
    int sortCol = 0; bool sortDesc = false;
    bool colVisible[5] = {true, true, true, true, true};   // 名称/路径/扩展名/大小/修改时间
    // 行为：文件操作
    bool allowRename = true;                 // [general] allow_rename：F2 重命名
    bool singleParentContextMenu = true;     // [general] single_parent_context_menu：选择在同一文件夹时使用系统右键菜单
    bool matchRegex = false;       // [search] match_regex
    bool matchCase = false;        // [search] match_case
    bool matchWholeWord = false;   // [search] match_whole_word
    bool matchPath = false;        // [search] match_path（所有词匹配完整路径）
    bool historyEnabled = true;    // [history] enabled
    int historyMax = 100;          // [history] max_entries（64–128 常用）
    int colExt = 70;               // [columns] extension
    // colVisible[0]=名称(恒真) [1]路径 [2]扩展名 [3]大小 [4]修改时间
    int filter = 0;   // [search] filter
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
    bool fastAsciiSearch = true;          // fast_ascii_search
    int monitorUpdateDelay = 1000;        // monitor_update_delay（毫秒）：USN 批量合并间隔
    // 每卷设置：ini 中的并行数组 ntfs_volume_paths / guids / includes / monitors / load_recent_changes /
    // include_onlys / enable_usn_journals（逗号分隔；含逗号或引号的项用双引号）
    std::vector<VolumeConfig> ntfsVolumes;
    // 索引：空 = 所有固定磁盘
    std::vector<std::wstring> indexRoots;
    std::vector<std::wstring> excludeFolders = {L"C:\\Windows\\WinSxS", L"C:\\$Recycle.Bin", L"C:\\System Volume Information"};

    static constexpr int CW_USEDEFAULT_SENTINEL = -32768;

    std::wstring IniPath() const;
    void Load() { LoadFrom(IniPath()); }
    void Save() const { SaveTo(IniPath()); }
    void LoadFrom(const std::wstring& ini, bool detect = true);
    void SaveTo(const std::wstring& ini) const;
    static std::vector<std::wstring> SplitArray(const std::wstring& s);
    static std::wstring JoinArray(const std::vector<std::wstring>& v);
};

Settings& GetSettings();

} // namespace ff
