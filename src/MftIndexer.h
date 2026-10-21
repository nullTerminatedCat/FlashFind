#pragma once
// TODO(M3): NTFS MFT + USN Journal 索引器，运行在服务进程中（需要管理员 / LocalSystem）。
// 计划：
//  1. CreateFileW(L"\\\\.\\C:") 打开卷句柄
//  2. FSCTL_QUERY_USN_JOURNAL（不存在时 FSCTL_CREATE_USN_JOURNAL）
//  3. FSCTL_ENUM_USN_DATA 枚举全部 MFT 记录 -> (FRN, 父 FRN, 名称, 属性)
//  4. 大小/时间：FSCTL_GET_NTFS_FILE_RECORD 或延迟按需 GetFileInformationByHandleEx
//  5. FSCTL_READ_USN_JOURNAL 持续监听增量（创建/删除/重命名）
//  6. 数据库持久化：%ProgramData%\FlashFind\FlashFind.db（压缩，带卷 USN 检查点）
#include <string>
#include <vector>

namespace ff {
class MftIndexer {
public:
    static bool Available();                 // 当前进程是否有权限读取卷（M1 恒为 false）
    static std::vector<std::wstring> NtfsVolumes();
};
}
