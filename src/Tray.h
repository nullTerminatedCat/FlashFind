#pragma once
#include <windows.h>
#include <shellapi.h>

namespace ff {
class Tray {
public:
    void Add(HWND owner, HICON icon);
    void Remove();
    void ShowBalloon(const wchar_t* title, const wchar_t* text);
    void ReAdd() { if (owner_) { NOTIFYICONDATAW n = nid_; Shell_NotifyIconW(NIM_ADD, &n); } } // Explorer 重启
    static constexpr UINT kId = 1;
private:
    HWND owner_ = nullptr;
    NOTIFYICONDATAW nid_{};
};
}
