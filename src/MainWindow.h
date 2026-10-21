#pragma once
#include "App.h"
#include "Hotkey.h"
#include "Index.h"
#include "ResultList.h"
#include "Search.h"
#include "Tray.h"
#include <memory>

namespace ff {

class MainWindow {
public:
    bool Create(int show);
    bool PreTranslate(MSG& msg);   // 键盘快捷键
    HWND Hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT, WPARAM, LPARAM);
    void OnCreate();
    void Layout();
    void UpdateFonts();
    void ApplyTheme();
    void Paint();
    void BuildMenu();
    void StartIndex();
    void RunSearch();
    void UpdateStatus();
    void UpdateTitle();

    void Toggle();
    void ShowAndFocus();
    void HideToTray();

    void OpenItems(bool openPath);
    void CopyPaths(bool nameOnly);
    void ShowProperties();
    void ContextMenu(POINT screenPt);
    void TrayMenu();
    void SavePlacement();

    HWND hwnd_ = nullptr, edit_ = nullptr;
    ResultList list_;
    Indexer indexer_;
    std::unique_ptr<Searcher> searcher_;
    Tray tray_;
    HotkeyManager hotkeys_;
    bool hotkeyFailed_ = false;
    HFONT font_ = nullptr, bold_ = nullptr;
    RECT searchRc_{}, statusRc_{};
    size_t indexProgress_ = 0;
    std::wstring statusLeft_;
    double lastSearchMs_ = 0;
    UINT taskbarCreated_ = 0;
    HICON iconBig_ = nullptr, iconSmall_ = nullptr;
};

} // namespace ff
