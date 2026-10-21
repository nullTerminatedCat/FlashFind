#pragma once
// 文件操作：重命名 / 删除 / 拖出。全部基于 IFileOperation / Shell 数据对象（支持撤销、提权提示、冲突对话框）。
// 索引不在这里改：由 USN 监视（或服务）把变化送回 LiveUpdater。
#include <windows.h>
#include <string>
#include <vector>

namespace ff {

struct RenameReq { std::wstring path; std::wstring newName; };
// 返回成功改名的项数；err 为第一条错误
size_t RenameItems(HWND owner, const std::vector<RenameReq>& reqs, std::wstring& err);
// permanent=false 进回收站；true 永久删除（调用方先确认）。返回 false 表示失败或用户取消
bool DeleteItems(HWND owner, const std::vector<std::wstring>& paths, bool permanent, std::wstring& err);
// 拖出到资源管理器/其他程序：IDataObject（CF_HDROP + Shell IDList），可跨文件夹
bool DragOut(HWND owner, const std::vector<std::wstring>& paths);
// 名称是否合法（非空、无 \ / : * ? " < > |、不是 . 或 ..、不以空格结尾）
bool ValidFileName(const std::wstring& n);
// 多项重命名对话框：names 为原名称（就地修改为新名称）；返回 true = 确定
bool ShowRenameDialog(HWND owner, const std::vector<std::wstring>& paths, std::vector<std::wstring>& names);

} // namespace ff
