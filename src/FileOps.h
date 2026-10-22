#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace ff {

struct RenameReq { std::wstring path; std::wstring newName; };
size_t RenameItems(HWND owner, const std::vector<RenameReq>& reqs, std::wstring& err);
bool DeleteItems(HWND owner, const std::vector<std::wstring>& paths, bool permanent, std::wstring& err);
bool DragOut(HWND owner, const std::vector<std::wstring>& paths);
bool ValidFileName(const std::wstring& n);
bool ShowRenameDialog(HWND owner, const std::vector<std::wstring>& paths, std::vector<std::wstring>& names);

// 剪贴板：CF_HDROP；cut=true 写入 Preferred DropEffect = MOVE
bool ClipboardSetFiles(HWND owner, const std::vector<std::wstring>& paths, bool cut, std::wstring& err);
// 粘贴到 destDir（须以 \ 结尾或为盘符根）；cut 由剪贴板 Preferred DropEffect 决定
bool ClipboardPaste(HWND owner, const std::wstring& destDir, std::wstring& err);
// 复制/移动到用户选择的文件夹
bool CopyOrMoveToFolder(HWND owner, const std::vector<std::wstring>& paths, bool move, std::wstring& err);

} // namespace ff
