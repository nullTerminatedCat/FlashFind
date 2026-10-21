#pragma once
// 特权 NTFS 访问的来源：
//  * LocalSource：本进程已提升（管理员）时直接读 MFT/USN；
//  * PipeSource：未提升的界面通过命名管道 \\.\pipe\FlashFind.Service 请求 FlashFind 服务（LocalSystem）。
// 设计（最简单且高性能）：服务只做“特权读取器”，索引/排序/数据库仍由界面进程负责。
//   - READ_USN：服务返回内核给出的原始 USN_RECORD_V2 字节流（带文件名），界面用同一解析器处理；
//   - BUILD：服务顺序读 $MFT，把整卷 VolumeData（条目/名称/FRN 原始数组）一次流回，界面合并排序；
//   - LOOKUP：批量 FRN -> 大小/修改时间；SCAN_DIR：移入文件夹的子树枚举。
// 帧格式：[u32 载荷长度][u16 操作][u16 状态] + 载荷（小端，字符串 = u16 字符数 + UTF-16）。
#include "Index.h"
#include "Usn.h"
#include <atomic>
#include <memory>

namespace ff {

constexpr const wchar_t* kServicePipe = L"\\\\.\\pipe\\FlashFind.Service";
constexpr const wchar_t* kServiceName = L"FlashFind";
constexpr uint32_t kProtocolVersion = 1;

enum PipeOp : uint16_t { OpHello = 1, OpQueryJournal = 2, OpReadUsn = 3, OpBuild = 4, OpLookup = 5, OpScanDir = 6, OpBench = 7, OpProgress = 0x84 };

class NtfsSource {
public:
    virtual ~NtfsSource() = default;
    virtual const wchar_t* Kind() const = 0;
    virtual bool QueryJournal(const std::wstring& root, bool create, uint64_t& jid, int64_t& first, int64_t& next, std::wstring& err) = 0;
    // 返回 0 或 Win32 错误；raw 为连续的 USN_RECORD_V2
    virtual DWORD ReadUsn(const std::wstring& root, uint64_t jid, int64_t& from, std::vector<uint8_t>& raw, std::wstring& err) = 0;
    virtual bool Build(VolumeData& v, bool enableUsn, std::atomic<bool>& stop, std::atomic<size_t>& progress, std::wstring& err) = 0;
    virtual bool Lookup(const std::wstring& root, const std::vector<uint64_t>& frns, std::vector<LookupResult>& out) = 0;
    virtual bool ScanDir(const std::wstring& root, const std::wstring& path, const std::vector<std::wstring>& excludes, VolumeData& out, std::wstring& err) = 0;
    virtual bool Alive() const { return true; }
};

std::unique_ptr<NtfsSource> MakeLocalSource();
// 连接服务；allowConsole=false 时校验管道服务器进程就是已安装的 FlashFind 服务（防止伪造管道）
std::unique_ptr<NtfsSource> ConnectService(bool allowConsole, std::wstring* why);
// 管道服务器（服务与 -svc-console 共用），stopEvent 置位时退出
int RunPipeServer(HANDLE stopEvent);
// 测试用：吞吐量（MB/s）
double PipeBench(NtfsSource& s, uint32_t mb);

// 服务管理（Service.cpp）
int ServiceMain();          // -svc
int InstallService();       // -install-service（需管理员）
int UninstallService();     // -uninstall-service（需管理员）
bool ServiceInstalled();
// 界面：以 runas 启动本程序执行安装/卸载，等待并返回结果（ERROR_CANCELLED = 用户取消 UAC）
DWORD RunElevated(HWND owner, const wchar_t* args);

} // namespace ff
