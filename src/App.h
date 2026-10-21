#pragma once
#include <windows.h>

namespace ff {

// 自定义消息
enum : UINT {
    WM_APP_RESULTS = WM_APP + 1,       // wParam = 搜索代号
    WM_APP_INDEX_PROGRESS,             // wParam = 已发现对象数
    WM_APP_INDEX_DONE,
    WM_APP_TRAY,                       // 托盘回调
    WM_APP_TOGGLE,                     // 热键/双击 Ctrl/第二实例 -> 显示或隐藏
    WM_APP_SHOW,                       // 第二实例 -> 仅显示
};

constexpr const wchar_t* kMainClass = L"FlashFind.MainWindow";
constexpr const wchar_t* kAppName = L"闪寻 FlashFind";

class App {
public:
    int Run(HINSTANCE inst, int show);
    static HINSTANCE Instance();
};

} // namespace ff
