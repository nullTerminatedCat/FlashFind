#include "Tray.h"
#include "App.h"

namespace ff {

void Tray::Add(HWND owner, HICON icon) {
    owner_ = owner;
    nid_ = {sizeof(nid_)};
    nid_.hWnd = owner;
    nid_.uID = kId;
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid_.uCallbackMessage = WM_APP_TRAY;
    nid_.hIcon = icon;
    wcscpy_s(nid_.szTip, L"闪寻 FlashFind");
    Shell_NotifyIconW(NIM_ADD, &nid_);
    nid_.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid_);
}

void Tray::Remove() {
    if (owner_) Shell_NotifyIconW(NIM_DELETE, &nid_);
    owner_ = nullptr;
}

void Tray::ShowBalloon(const wchar_t* title, const wchar_t* text) {
    if (!owner_) return;
    NOTIFYICONDATAW n = nid_;
    n.uFlags = NIF_INFO;
    wcscpy_s(n.szInfoTitle, title);
    wcscpy_s(n.szInfo, text);
    n.dwInfoFlags = NIIF_WARNING;
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

}
