#include "Theme.h"
#include <dwmapi.h>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif

namespace ff {

Theme& GetTheme() { static Theme t; return t; }

Theme::~Theme() { if (bg_) DeleteObject(bg_); if (surface_) DeleteObject(surface_); }

bool Theme::SystemIsDark() {
    DWORD v = 1, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v == 0;
}

bool Theme::IsWin11() {
    // RtlGetVersion 不受兼容性清单影响
    using Fn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    static DWORD build = [] {
        RTL_OSVERSIONINFOW vi{sizeof(vi)};
        if (auto f = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))) f(&vi);
        return vi.dwBuildNumber;
    }();
    return build >= 22000;
}

void Theme::Resolve(ThemeMode mode) {
    dark_ = mode == ThemeMode::Dark || (mode == ThemeMode::System && SystemIsDark());
    if (dark_)
        p_ = {RGB(0x20,0x20,0x20), RGB(0x27,0x27,0x27), RGB(0xF0,0xF0,0xF0), RGB(0xA8,0xA8,0xA8),
              RGB(0x2B,0x2B,0x2B), RGB(0x33,0x33,0x33), RGB(0x1F,0x4A,0x66), RGB(0xFF,0xFF,0xFF),
              RGB(0x4C,0xC2,0xFF), RGB(0x5C,0x4A,0x00), RGB(0xFF,0xE5,0x8A), RGB(0x3A,0x3A,0x3A)};
    else
        p_ = {RGB(0xF3,0xF3,0xF3), RGB(0xFF,0xFF,0xFF), RGB(0x1B,0x1B,0x1B), RGB(0x5D,0x5D,0x5D),
              RGB(0xF8,0xF8,0xF8), RGB(0xF0,0xF0,0xF0), RGB(0xCC,0xE4,0xF7), RGB(0x1B,0x1B,0x1B),
              RGB(0x00,0x67,0xC0), RGB(0xFF,0xF1,0xB8), RGB(0x1B,0x1B,0x1B), RGB(0xE5,0xE5,0xE5)};
    if (bg_) DeleteObject(bg_);
    if (surface_) DeleteObject(surface_);
    bg_ = CreateSolidBrush(p_.bg);
    surface_ = CreateSolidBrush(p_.surface);
}

void Theme::ApplyToWindow(HWND hwnd) const {
    BOOL d = dark_;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &d, sizeof(d));
    if (IsWin11()) {
        int corner = 2; // DWMWCP_ROUND
        DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
        int backdrop = 2; // DWMSBT_MAINWINDOW (Mica)，仅标题栏区域可见；客户区为实色
        DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
    }
}

} // namespace ff
