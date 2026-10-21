#include "Hotkey.h"
#include "App.h"
#include "Util.h"
#include <sstream>

namespace ff {

HotkeyDef ParseHotkey(const std::wstring& s) {
    HotkeyDef h; std::wstringstream ss(s); std::wstring t;
    while (std::getline(ss, t, L'+')) {
        auto k = ToLower(t);
        while (!k.empty() && k.back() == L' ') k.pop_back();
        while (!k.empty() && k.front() == L' ') k.erase(0, 1);
        if (k == L"ctrl" || k == L"control") h.mods |= MOD_CONTROL;
        else if (k == L"alt") h.mods |= MOD_ALT;
        else if (k == L"shift") h.mods |= MOD_SHIFT;
        else if (k == L"win") h.mods |= MOD_WIN;
        else if (k == L"space") h.vk = VK_SPACE;
        else if (k == L"tab") h.vk = VK_TAB;
        else if (k == L"`" || k == L"backquote") h.vk = VK_OEM_3;
        else if (k.size() == 1 && iswalnum(k[0])) h.vk = towupper(k[0]);
        else if (k.size() >= 2 && k[0] == L'f') { int n = _wtoi(k.c_str() + 1); if (n >= 1 && n <= 24) h.vk = VK_F1 + n - 1; }
    }
    return h;
}

bool HotkeyManager::RegisterToggle(HWND hwnd, const std::wstring& spec) {
    UnregisterToggle();
    auto d = ParseHotkey(spec);
    if (!d.Valid()) return false;
    hwnd_ = hwnd;
    registered_ = RegisterHotKey(hwnd, kToggleId, d.mods | MOD_NOREPEAT, d.vk) != FALSE;
    return registered_;
}

void HotkeyManager::UnregisterToggle() {
    if (registered_) UnregisterHotKey(hwnd_, kToggleId);
    registered_ = false;
}

// ---- 双击 Ctrl ----
static HHOOK g_hook = nullptr;
static HWND g_target = nullptr;
static DWORD g_lastCtrlUp = 0;
static bool g_ctrlDown = false, g_dirty = false;

static LRESULT CALLBACK LLKeyboard(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        auto* k = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
        bool isCtrl = k->vkCode == VK_LCONTROL || k->vkCode == VK_RCONTROL || k->vkCode == VK_CONTROL;
        bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
        if (!isCtrl) {
            if (down) { g_dirty = true; g_lastCtrlUp = 0; }   // 中间按了别的键，不算双击
        } else if (down) {
            if (!g_ctrlDown) g_dirty = false;
            g_ctrlDown = true;
        } else {
            g_ctrlDown = false;
            if (!g_dirty) {
                DWORD now = k->time;
                if (g_lastCtrlUp && now - g_lastCtrlUp <= 300) {
                    g_lastCtrlUp = 0;
                    PostMessageW(g_target, WM_APP_TOGGLE, 1, 0);  // 钩子回调必须尽快返回
                } else {
                    g_lastCtrlUp = now;
                }
            }
        }
    }
    return CallNextHookEx(g_hook, code, wp, lp);
}

bool HotkeyManager::EnableDoubleCtrl(HWND hwnd) {
    if (g_hook) return true;
    g_target = hwnd;
    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LLKeyboard, GetModuleHandleW(nullptr), 0);
    return g_hook != nullptr;
}

void HotkeyManager::DisableDoubleCtrl() {
    if (g_hook) UnhookWindowsHookEx(g_hook);
    g_hook = nullptr;
}

} // namespace ff
