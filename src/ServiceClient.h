#pragma once
// TODO(M3): 与 FlashFind 索引服务通信的客户端。
// 协议草案：命名管道 \\.\pipe\FlashFind（可由 -service-pipe-name 指定），消息 = [u32 长度][u16 类型][payload]
//   QUERY(搜索词, 匹配选项, 排序, 偏移, 数量) -> RESULTS(总数, 条目[])
//   SUBSCRIBE -> CHANGED(卷, USN 增量摘要)
// 服务以 LocalSystem 运行，读取 NTFS MFT 与 USN Journal（见 MftIndexer）。
#include <string>

namespace ff {
class ServiceClient {
public:
    static constexpr const wchar_t* kDefaultPipe = L"\\\\.\\pipe\\FlashFind";
    // 目前总是返回 false，界面会回退到进程内的文件夹遍历索引
    bool Connect(const std::wstring& pipe = kDefaultPipe);
    bool Connected() const { return false; }
};
}
