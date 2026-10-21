#include "NtfsSource.h"
#include "Log.h"
#include "MftIndexer.h"
#include <sddl.h>
#include <winioctl.h>
#include <chrono>
#include <map>
#include <thread>

namespace ff {

// ---------------- 序列化 ----------------
namespace {
struct Out {
    std::vector<uint8_t> b;
    template <class T> void Put(const T& v) { auto* p = reinterpret_cast<const uint8_t*>(&v); b.insert(b.end(), p, p + sizeof(T)); }
    void Raw(const void* p, size_t n) { auto* q = static_cast<const uint8_t*>(p); b.insert(b.end(), q, q + n); }
    void Str(const std::wstring& s) { Put<uint16_t>(static_cast<uint16_t>(std::min<size_t>(s.size(), 0xFFFF))); Raw(s.data(), std::min<size_t>(s.size(), 0xFFFF) * 2); }
};
struct In {
    const uint8_t* p; size_t n, pos = 0; bool ok = true;
    template <class T> T Get() { T v{}; if (pos + sizeof(T) > n) { ok = false; return v; } memcpy(&v, p + pos, sizeof(T)); pos += sizeof(T); return v; }
    const uint8_t* Take(size_t k) { if (!ok || k > n - pos) { ok = false; return nullptr; } auto q = p + pos; pos += k; return q; }
    std::wstring Str() { uint16_t c = Get<uint16_t>(); auto q = Take(c * 2u); return q ? std::wstring(reinterpret_cast<const wchar_t*>(q), c) : std::wstring(); }
};
struct Hdr { uint32_t len; uint16_t op; uint16_t status; };
static_assert(sizeof(Hdr) == 8);
constexpr uint32_t kMaxFrame = 1u << 30;

void PutVolume(Out& o, const VolumeData& v) {
    o.Str(v.info.root); o.Put<uint32_t>(v.info.serial); o.Put<uint8_t>(v.info.ntfs); o.Put<uint8_t>(v.info.mft);
    o.Put<uint64_t>(v.info.journalId); o.Put<int64_t>(v.info.nextUsn);
    o.Put<uint64_t>(v.entries.size()); o.Put<uint64_t>(v.names.size()); o.Put<uint64_t>(v.frn.size());
    o.b.reserve(o.b.size() + v.entries.size() * 32 + v.names.size() * 2 + v.frn.size() * 8);
    o.Raw(v.entries.data(), v.entries.size() * sizeof(Entry));
    o.Raw(v.names.data(), v.names.size() * 2);
    o.Raw(v.frn.data(), v.frn.size() * 8);
}
bool GetVolume(In& in, VolumeData& v) {
    v.info.root = in.Str(); v.info.serial = in.Get<uint32_t>(); v.info.ntfs = in.Get<uint8_t>() != 0; v.info.mft = in.Get<uint8_t>() != 0;
    v.info.journalId = in.Get<uint64_t>(); v.info.nextUsn = in.Get<int64_t>();
    uint64_t ne = in.Get<uint64_t>(), nc = in.Get<uint64_t>(), nf = in.Get<uint64_t>();
    if (!in.ok || ne > in.n / 32 || nc > in.n / 2 || (nf && nf != ne)) return false;
    auto e = in.Take(ne * sizeof(Entry)); auto c = in.Take(nc * 2); auto f = in.Take(nf * 8);
    if (!in.ok) return false;
    v.entries.resize(ne); memcpy(v.entries.data(), e, ne * sizeof(Entry));
    v.names.resize(nc); memcpy(v.names.data(), c, nc * 2);
    v.frn.resize(nf); memcpy(v.frn.data(), f, nf * 8);
    for (auto& x : v.entries) if (static_cast<uint64_t>(x.nameOff) + x.nameLen > nc || (x.parent != kNoParent && x.parent >= ne)) return false;
    return true;
}

// 管道 I/O（句柄可能是重叠模式：统一用 OVERLAPPED + 等待）
bool IoAll(HANDLE h, void* buf, size_t n, bool write) {
    auto* p = static_cast<uint8_t*>(buf);
    OVERLAPPED ov{}; ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool ok = true;
    while (n && ok) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(n, 16u << 20)), done = 0;
        ResetEvent(ov.hEvent);
        BOOL r = write ? WriteFile(h, p, chunk, nullptr, &ov) : ReadFile(h, p, chunk, nullptr, &ov);
        if (!r && GetLastError() != ERROR_IO_PENDING) { ok = false; break; }
        if (!GetOverlappedResult(h, &ov, &done, TRUE) || done == 0) { ok = false; break; }
        p += done; n -= done;
    }
    CloseHandle(ov.hEvent);
    return ok;
}
bool SendFrame(HANDLE h, uint16_t op, uint16_t status, const std::vector<uint8_t>& payload) {
    Hdr hd{static_cast<uint32_t>(payload.size()), op, status};
    return IoAll(h, &hd, sizeof(hd), true) && (payload.empty() || IoAll(h, const_cast<uint8_t*>(payload.data()), payload.size(), true));
}
bool RecvFrame(HANDLE h, Hdr& hd, std::vector<uint8_t>& payload) {
    if (!IoAll(h, &hd, sizeof(hd), false) || hd.len > kMaxFrame) return false;
    payload.resize(hd.len);
    return hd.len == 0 || IoAll(h, payload.data(), hd.len, false);
}
std::vector<uint8_t> ErrPayload(DWORD e, const std::wstring& msg) { Out o; o.Put<uint32_t>(e); o.Str(msg); return std::move(o.b); }
} // namespace

// ---------------- 本进程（已提升） ----------------
class LocalSource : public NtfsSource {
public:
    ~LocalSource() override { for (auto& [k, v] : vols_) CloseHandle(v.h); }
    const wchar_t* Kind() const override { return L"本进程（管理员）"; }
    bool QueryJournal(const std::wstring& root, bool create, uint64_t& jid, int64_t& first, int64_t& next, std::wstring& err) override {
        return MftIndexer::QueryJournal(root, create, jid, first, next, err);
    }
    DWORD ReadUsn(const std::wstring& root, uint64_t jid, int64_t& from, std::vector<uint8_t>& raw, std::wstring& err) override {
        auto* v = Vol(root);
        if (!v) { err = L"打开卷失败"; return GetLastError() ? GetLastError() : ERROR_ACCESS_DENIED; }
        DWORD e = MftIndexer::ReadUsnRaw(v->h, jid, from, raw);
        if (e && e != ERROR_JOURNAL_ENTRY_DELETED) Drop(root);
        return e;
    }
    bool Build(VolumeData& v, bool enableUsn, std::atomic<bool>& stop, std::atomic<size_t>& progress, std::wstring& err) override {
        return MftIndexer::Build(v, enableUsn, stop, progress, err);
    }
    bool Lookup(const std::wstring& root, const std::vector<uint64_t>& frns, std::vector<LookupResult>& out) override {
        out.assign(frns.size(), {});
        auto* v = Vol(root);
        if (!v) return false;
        for (size_t i = 0; i < frns.size(); ++i) out[i] = MftIndexer::LookupRecord(v->h, v->g, frns[i]);
        return true;
    }
    bool ScanDir(const std::wstring& root, const std::wstring& path, const std::vector<std::wstring>& excludes, VolumeData& out, std::wstring& err) override {
        if (!PathUnder(path, root)) { err = L"路径不在卷内"; return false; }
        std::atomic<bool> stop{false}; std::atomic<size_t> prog{0};
        out.info.root = root;
        MftIndexer::EnumerateWithIds(out, excludes, stop, prog, path);
        return !out.entries.empty();
    }
private:
    struct V { HANDLE h; MftIndexer::Geometry g; };
    V* Vol(const std::wstring& root) {
        auto it = vols_.find(root);
        if (it != vols_.end()) return &it->second;
        HANDLE h = MftIndexer::OpenVolumeRead(root);
        if (h == INVALID_HANDLE_VALUE) return nullptr;
        return &(vols_[root] = V{h, MftIndexer::GetGeometry(h)});
    }
    void Drop(const std::wstring& root) { auto it = vols_.find(root); if (it != vols_.end()) { CloseHandle(it->second.h); vols_.erase(it); } }
    std::map<std::wstring, V> vols_;
};

std::unique_ptr<NtfsSource> MakeLocalSource() { return std::make_unique<LocalSource>(); }

// ---------------- 管道客户端 ----------------
class PipeSource : public NtfsSource {
public:
    explicit PipeSource(HANDLE h) : h_(h) {}
    ~PipeSource() override { if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_); }
    const wchar_t* Kind() const override { return L"FlashFind 服务"; }
    bool Alive() const override { return alive_; }
    bool Call(uint16_t op, const Out& req, Hdr& hd, std::vector<uint8_t>& resp) {
        if (!alive_) return false;
        if (!SendFrame(h_, op, 0, req.b) || !RecvFrame(h_, hd, resp)) { alive_ = false; return false; }
        return true;
    }
    static std::wstring ErrOf(const std::vector<uint8_t>& p, DWORD* code) {
        In in{p.data(), p.size()}; DWORD e = in.Get<uint32_t>(); if (code) *code = e; return in.Str();
    }
    bool Hello(uint32_t& ver, uint32_t& pid) {
        Out o; o.Put<uint32_t>(kProtocolVersion); Hdr hd; std::vector<uint8_t> r;
        if (!Call(OpHello, o, hd, r) || hd.status) return false;
        In in{r.data(), r.size()}; ver = in.Get<uint32_t>(); pid = in.Get<uint32_t>(); return in.ok;
    }
    bool QueryJournal(const std::wstring& root, bool create, uint64_t& jid, int64_t& first, int64_t& next, std::wstring& err) override {
        Out o; o.Str(root); o.Put<uint8_t>(create); Hdr hd; std::vector<uint8_t> r;
        if (!Call(OpQueryJournal, o, hd, r)) { err = L"服务连接断开"; return false; }
        if (hd.status) { err = ErrOf(r, nullptr); return false; }
        In in{r.data(), r.size()}; jid = in.Get<uint64_t>(); first = in.Get<int64_t>(); next = in.Get<int64_t>(); return in.ok;
    }
    DWORD ReadUsn(const std::wstring& root, uint64_t jid, int64_t& from, std::vector<uint8_t>& raw, std::wstring& err) override {
        Out o; o.Str(root); o.Put<uint64_t>(jid); o.Put<int64_t>(from); Hdr hd; std::vector<uint8_t> r;
        if (!Call(OpReadUsn, o, hd, r)) { err = L"服务连接断开"; return ERROR_BROKEN_PIPE; }
        if (hd.status) { DWORD e = 0; err = ErrOf(r, &e); return e ? e : ERROR_GEN_FAILURE; }
        In in{r.data(), r.size()}; int64_t nx = in.Get<int64_t>(); uint64_t n = in.Get<uint64_t>(); auto p = in.Take(n);
        if (!in.ok) return ERROR_INVALID_DATA;
        raw.insert(raw.end(), p, p + n); from = nx; return 0;
    }
    bool Build(VolumeData& v, bool enableUsn, std::atomic<bool>& stop, std::atomic<size_t>& progress, std::wstring& err) override {
        Out o; o.Str(v.info.root); o.Put<uint8_t>(enableUsn);
        if (!alive_ || !SendFrame(h_, OpBuild, 0, o.b)) { alive_ = false; err = L"服务连接断开"; return false; }
        for (;;) {
            Hdr hd; std::vector<uint8_t> r;
            if (!RecvFrame(h_, hd, r)) { alive_ = false; err = L"服务连接断开"; return false; }
            if (hd.op == OpProgress) {
                In in{r.data(), r.size()}; progress = static_cast<size_t>(in.Get<uint64_t>());
                if (stop) { alive_ = false; CancelIoEx(h_, nullptr); err = L"已取消"; return false; }   // 断开即通知服务取消
                continue;
            }
            if (hd.status) { err = ErrOf(r, nullptr); return false; }
            In in{r.data(), r.size()};
            VolumeData tmp;
            if (!GetVolume(in, tmp)) { err = L"服务返回的数据损坏"; return false; }
            v = std::move(tmp);
            return true;
        }
    }
    bool Lookup(const std::wstring& root, const std::vector<uint64_t>& frns, std::vector<LookupResult>& out) override {
        out.assign(frns.size(), {});
        Out o; o.Str(root); o.Put<uint32_t>(static_cast<uint32_t>(frns.size())); o.Raw(frns.data(), frns.size() * 8);
        Hdr hd; std::vector<uint8_t> r;
        if (!Call(OpLookup, o, hd, r) || hd.status || r.size() != frns.size() * sizeof(LookupResult)) return false;
        memcpy(out.data(), r.data(), r.size()); return true;
    }
    bool ScanDir(const std::wstring& root, const std::wstring& path, const std::vector<std::wstring>& excludes, VolumeData& out, std::wstring& err) override {
        Out o; o.Str(root); o.Str(path); o.Put<uint16_t>(static_cast<uint16_t>(excludes.size())); for (auto& x : excludes) o.Str(x);
        Hdr hd; std::vector<uint8_t> r;
        if (!Call(OpScanDir, o, hd, r)) { err = L"服务连接断开"; return false; }
        if (hd.status) { err = ErrOf(r, nullptr); return false; }
        In in{r.data(), r.size()}; return GetVolume(in, out);
    }
    double Bench(uint32_t mb) {
        Out o; o.Put<uint32_t>(mb); Hdr hd; std::vector<uint8_t> r;
        auto t = std::chrono::steady_clock::now();
        if (!Call(OpBench, o, hd, r) || r.size() != (size_t(mb) << 20)) return 0;
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
        return mb / s;
    }
private:
    HANDLE h_;
    bool alive_ = true;
};

double PipeBench(NtfsSource& s, uint32_t mb) { auto* p = dynamic_cast<PipeSource*>(&s); return p ? p->Bench(mb) : 0; }

static DWORD ServicePid() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return 0;
    DWORD pid = 0;
    if (SC_HANDLE s = OpenServiceW(scm, kServiceName, SERVICE_QUERY_STATUS)) {
        SERVICE_STATUS_PROCESS sp{}; DWORD need = 0;
        if (QueryServiceStatusEx(s, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&sp), sizeof(sp), &need) && sp.dwCurrentState == SERVICE_RUNNING) pid = sp.dwProcessId;
        CloseServiceHandle(s);
    }
    CloseServiceHandle(scm);
    return pid;
}

std::unique_ptr<NtfsSource> ConnectService(bool allowConsole, std::wstring* why) {
    DWORD svcPid = ServicePid();
    if (!svcPid && !allowConsole) { if (why) *why = L"服务未安装或未运行"; return nullptr; }
    HANDLE h = CreateFileW(kServicePipe, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY && WaitNamedPipeW(kServicePipe, 2000))
        h = CreateFileW(kServicePipe, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (h == INVALID_HANDLE_VALUE) { if (why) *why = L"无法连接服务管道（错误 " + std::to_wstring(GetLastError()) + L"）"; return nullptr; }
    ULONG serverPid = 0;
    GetNamedPipeServerProcessId(h, &serverPid);
    if (!allowConsole && serverPid != svcPid) {   // 管道必须由已安装的服务创建
        CloseHandle(h);
        if (why) *why = L"管道服务器不是 FlashFind 服务（pid " + std::to_wstring(serverPid) + L"）";
        Log(L"拒绝连接：管道服务器 pid %lu 不是 FlashFind 服务 pid %lu", serverPid, svcPid);
        return nullptr;
    }
    auto p = std::make_unique<PipeSource>(h);
    uint32_t ver = 0, pid = 0;
    if (!p->Hello(ver, pid) || ver != kProtocolVersion) { if (why) *why = L"服务协议版本不符"; return nullptr; }
    Log(L"已连接 FlashFind 服务（pid %lu，协议 %u）", pid, ver);
    return p;
}

// ---------------- 管道服务器 ----------------
static bool ValidRoot(const std::wstring& r) {
    if (r.size() != 2 || r[1] != L':' || !iswalpha(r[0])) return false;
    wchar_t root[] = {r[0], L':', L'\\', 0};
    return GetDriveTypeW(root) == DRIVE_FIXED;
}

static void ServeClient(HANDLE h, HANDLE stopEvent) {
    LocalSource src;
    std::vector<uint8_t> req;
    while (WaitForSingleObject(stopEvent, 0) != WAIT_OBJECT_0) {
        Hdr hd;
        if (!RecvFrame(h, hd, req)) break;
        In in{req.data(), req.size()};
        Out o; uint16_t status = 0;
        auto fail = [&](DWORD e, const std::wstring& m) { o.b = ErrPayload(e, m); status = 1; };
        switch (hd.op) {
        case OpHello: o.Put<uint32_t>(kProtocolVersion); o.Put<uint32_t>(GetCurrentProcessId()); break;
        case OpQueryJournal: {
            auto root = in.Str(); bool create = in.Get<uint8_t>() != 0;
            if (!in.ok || !ValidRoot(root)) { fail(ERROR_INVALID_PARAMETER, L"无效的卷"); break; }
            uint64_t jid = 0; int64_t first = 0, next = 0; std::wstring err;
            if (src.QueryJournal(root, create, jid, first, next, err)) { o.Put(jid); o.Put(first); o.Put(next); } else fail(GetLastError(), err);
            break;
        }
        case OpReadUsn: {
            auto root = in.Str(); uint64_t jid = in.Get<uint64_t>(); int64_t from = in.Get<int64_t>();
            if (!in.ok || !ValidRoot(root)) { fail(ERROR_INVALID_PARAMETER, L"无效的卷"); break; }
            std::vector<uint8_t> raw; std::wstring err;
            DWORD e = src.ReadUsn(root, jid, from, raw, err);
            if (e) { fail(e, err); break; }
            o.Put<int64_t>(from); o.Put<uint64_t>(raw.size()); o.Raw(raw.data(), raw.size());
            break;
        }
        case OpBuild: {
            auto root = in.Str(); bool enable = in.Get<uint8_t>() != 0;
            if (!in.ok || !ValidRoot(root)) { fail(ERROR_INVALID_PARAMETER, L"无效的卷"); break; }
            VolumeData v; v.info.root = root; std::wstring err; bool ok = false;
            std::atomic<bool> stop{false}, done{false}; std::atomic<size_t> prog{0};
            std::thread w([&] { ok = src.Build(v, enable, stop, prog, err); done = true; });
            while (!done) {
                Sleep(200);
                if (done) break;
                Out p; p.Put<uint64_t>(prog.load());
                if (!SendFrame(h, OpProgress, 0, p.b)) stop = true;   // 客户端断开：取消
                if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) stop = true;
            }
            w.join();
            if (ok) PutVolume(o, v); else fail(ERROR_GEN_FAILURE, err);
            break;
        }
        case OpLookup: {
            auto root = in.Str(); uint32_t n = in.Get<uint32_t>(); auto p = in.Take(size_t(n) * 8);
            if (!in.ok || !ValidRoot(root) || n > 1000000) { fail(ERROR_INVALID_PARAMETER, L"无效的请求"); break; }
            std::vector<uint64_t> frns(n); memcpy(frns.data(), p, size_t(n) * 8);
            std::vector<LookupResult> res;
            src.Lookup(root, frns, res);
            o.Raw(res.data(), res.size() * sizeof(LookupResult));
            break;
        }
        case OpScanDir: {
            auto root = in.Str(); auto path = in.Str(); uint16_t ne = in.Get<uint16_t>();
            std::vector<std::wstring> ex; for (uint16_t k = 0; k < ne && in.ok; ++k) ex.push_back(in.Str());
            if (!in.ok || !ValidRoot(root) || !PathUnder(path, root) || path.find(L"\\..") != std::wstring::npos) { fail(ERROR_INVALID_PARAMETER, L"无效的路径"); break; }
            VolumeData v; std::wstring err;
            if (src.ScanDir(root, path, ex, v, err)) PutVolume(o, v); else fail(ERROR_PATH_NOT_FOUND, err.empty() ? L"扫描失败" : err);
            break;
        }
        case OpBench: {
            uint32_t mb = std::min<uint32_t>(in.Get<uint32_t>(), 1024);
            o.b.assign(size_t(mb) << 20, 0x5A);
            break;
        }
        default: fail(ERROR_INVALID_FUNCTION, L"未知操作");
        }
        if (!SendFrame(h, hd.op, status, o.b)) break;
    }
    FlushFileBuffers(h);
    DisconnectNamedPipe(h);
    CloseHandle(h);
}

int RunPipeServer(HANDLE stopEvent) {
    // 安全描述符：SYSTEM 与管理员完全控制；交互式登录用户可读写；拒绝网络访问
    SECURITY_ATTRIBUTES sa{sizeof(sa)};
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(D;;GA;;;NU)(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)", SDDL_REVISION_1, &sd, nullptr)) return 1;
    sa.lpSecurityDescriptor = sd;
    bool first = true;
    Log(L"管道服务器启动：%s", kServicePipe);
    while (WaitForSingleObject(stopEvent, 0) != WAIT_OBJECT_0) {
        HANDLE h = CreateNamedPipeW(kServicePipe, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                    PIPE_UNLIMITED_INSTANCES, 1 << 20, 1 << 20, 0, &sa);
        if (h == INVALID_HANDLE_VALUE) { Log(L"CreateNamedPipe 失败 %lu（是否已有服务在运行？）", GetLastError()); LocalFree(sd); return 2; }
        first = false;
        OVERLAPPED ov{}; ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        BOOL c = ConnectNamedPipe(h, &ov);
        DWORD e = GetLastError();
        bool connected = c || e == ERROR_PIPE_CONNECTED;
        if (!connected && e == ERROR_IO_PENDING) {
            HANDLE w[2] = {ov.hEvent, stopEvent};
            DWORD r = WaitForMultipleObjects(2, w, FALSE, INFINITE);
            DWORD dummy;
            connected = r == WAIT_OBJECT_0 && GetOverlappedResult(h, &ov, &dummy, FALSE);
            if (!connected) CancelIoEx(h, &ov);
        }
        CloseHandle(ov.hEvent);
        if (!connected) { CloseHandle(h); continue; }
        std::thread(ServeClient, h, stopEvent).detach();   // 进程随服务停止退出
    }
    LocalFree(sd);
    Log(L"管道服务器停止");
    return 0;
}

} // namespace ff
