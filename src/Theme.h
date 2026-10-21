#pragma once
#include <windows.h>
#include "Settings.h"

namespace ff {

struct Palette {
    COLORREF bg, surface, text, text2, alt, hover, sel, selText, accent, hl, hlText, border;
};

class Theme {
public:
    static bool SystemIsDark();
    static bool IsWin11();
    void Resolve(ThemeMode mode);          // 计算 dark_ 与调色板
    bool Dark() const { return dark_; }
    const Palette& P() const { return p_; }
    HBRUSH BgBrush() const { return bg_; }
    HBRUSH SurfaceBrush() const { return surface_; }
    // 标题栏深色 + Win11 Mica / 圆角
    void ApplyToWindow(HWND hwnd) const;
    ~Theme();
private:
    bool dark_ = false;
    Palette p_{};
    HBRUSH bg_ = nullptr, surface_ = nullptr;
};

Theme& GetTheme();

} // namespace ff
