#pragma once
#include <string>
#include <vector>

namespace ff {

std::wstring HistoryPath();
std::vector<std::wstring> LoadSearchHistory();
void SaveSearchHistory(const std::vector<std::wstring>& items);
void AddSearchHistory(const std::wstring& q);   // 空串忽略；去重后插到最前，截断到 historyMax
void ClearSearchHistory();

} // namespace ff
