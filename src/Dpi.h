#pragma once
#include <windows.h>

namespace ff {
// 96 DPI 逻辑像素 -> 物理像素
inline int Scale(int v, UINT dpi) { return MulDiv(v, static_cast<int>(dpi), 96); }
inline UINT DpiOf(HWND h) { UINT d = GetDpiForWindow(h); return d ? d : 96; }
}
