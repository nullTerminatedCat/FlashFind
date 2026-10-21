#pragma once
#include "App.h"
#include "Hotkey.h"
#include "Index.h"
#include "ResultList.h"
#include "Search.h"
#include "Tray.h"
#include <memory>
#include <shlobj.h>

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
    void StartIndex(bool force);
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
    void ServiceAction(bool install);
    void SavePlacement();
    // 文件操作（FileOps）
    std::vector<std::wstring> SelectedPaths() const;
    void Rename();                       // F2：单项就地编辑，多项对话框
    void BeginInlineRename();
    void EndInlineRename(bool commit);
    static LRESULT CALLBACK RenameEditProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    void Delete(bool permanent);         // Del 回收站；Shift+Del 永久删除（确认）
    bool DarkMenuBar(UINT m, WPARAM w, LPARAM l, LRESULT* out);   // 深色菜单栏（UAH 自绘，未公开消息）

    HWND hwnd_ = nullptr, edit_ = nullptr, svcBtn_ = nullptr, filter_ = nullptr;
    HBITMAP shieldBmp_ = nullptr;
    HWND renameEdit_ = nullptr; std::wstring renamePath_, renameOrig_; bool renameEnding_ = false;
    IContextMenu2* cm2_ = nullptr;
    IContextMenu3* cm3_ = nullptr;
    int svcBtnW_ = 0;
    ResultList list_;
    Indexer indexer_;
    std::unique_ptr<Searcher> searcher_;
    Tray tray_;
    bool noteShown_ = false;
    HotkeyManager hotkeys_;
    bool hotkeyFailed_ = false;
    HFONT font_ = nullptr, bold_ = nullptr;
    RECT searchRc_{}, statusRc_{};
    std::wstring statusLeft_;
    double lastSearchMs_ = 0;
    UINT taskbarCreated_ = 0;
    HICON iconBig_ = nullptr, iconSmall_ = nullptr;
};

} // namespace ff
