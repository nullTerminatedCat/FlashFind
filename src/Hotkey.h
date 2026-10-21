#pragma once
#include <windows.h>
#include <string>

namespace ff {

struct HotkeyDef { UINT mods = 0; UINT vk = 0; bool Valid() const { return vk != 0; } };
HotkeyDef ParseHotkey(const std::wstring& s);   // "Ctrl+Alt+Space"

class HotkeyManager {
public:
    // 注册切换热键；失败（被占用）返回 false
    bool RegisterToggle(HWND hwnd, const std::wstring& spec);
    void UnregisterToggle();
    // 双击 Ctrl：低级键盘钩子，检测到后 PostMessage(WM_APP_TOGGLE)
    bool EnableDoubleCtrl(HWND hwnd);
    void DisableDoubleCtrl();
    ~HotkeyManager() { UnregisterToggle(); DisableDoubleCtrl(); }
    static constexpr int kToggleId = 1;
private:
    HWND hwnd_ = nullptr;
    bool registered_ = false;
};

} // namespace ff
