#pragma once
// 选项对话框：常规 / 键盘 / NTFS 卷 / 排除。模态窗口 + 选项卡，控件按页显示/隐藏。
#include <windows.h>
#include <string>

namespace ff {

struct OptionsResult {
    bool applied = false;
    bool hotkeyChanged = false;   // 切换热键或双击 Ctrl 改变
    bool indexChanged = false;    // 卷设置、排除列表、监视间隔改变：需要重新启动索引
    bool themeChanged = false;
};

std::wstring FormatHotkey(UINT mods, UINT vk);   // 与 ParseHotkey 互逆，例如 "Ctrl+Alt+Space"
OptionsResult ShowOptions(HWND owner, int page = 0);

} // namespace ff
