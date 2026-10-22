#pragma once
#include <windows.h>
#include <string>

namespace ff {

bool UrlProtocolInstalled();
bool FolderContextMenuInstalled();
// 安装/卸载 es: 协议；失败时 err 非空
bool InstallUrlProtocol(bool install, std::wstring* err = nullptr);
// 安装/卸载文件夹右键「用闪寻搜索」
bool InstallFolderContextMenu(bool install, std::wstring* err = nullptr);

} // namespace ff
