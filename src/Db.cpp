#include "Db.h"
#include "Log.h"
#include <windows.h>
#include <compressapi.h>
#include <algorithm>
#include <cstring>

namespace ff {

#pragma pack(push, 1)
struct DbHeader {           // v2 / v3 共用的前 48 字节
    char magic[4];          // "FFDB"
    uint32_t version;
    uint32_t flags;         // bit0 = 载荷 XPRESS_HUFF 压缩
    uint32_t headerSize;    // v3: DbHeader + 卷表
    uint64_t rawSize;       // 解压后载荷大小
    uint64_t storedSize;    // 文件中载荷大小
    uint64_t configHash;
    uint64_t checksum;      // 存储载荷的 FNV-1a(64 位字)
};
// v3：头部之后紧跟固定大小卷表（不压缩，可原地更新 Journal ID / Next USN），再是载荷
struct VolTableHeader { uint32_t count; uint32_t reserved; uint64_t checksum; };
struct VolSlot {
    wchar_t root[8];
    uint32_t serial;
    uint32_t flags;         // bit0 NTFS，bit1 条目带 FRN（可 USN 增量）
    uint64_t journalId;
    int64_t nextUsn;
    uint8_t reserved[24];
};
static_assert(sizeof(VolSlot) == 64);
#pragma pack(pop)

static uint64_t Checksum(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    size_t w = n / 8;
    for (size_t i = 0; i < w; ++i) { uint64_t v; memcpy(&v, p + i * 8, 8); h = (h ^ v) * 1099511628211ULL; }
    for (size_t i = w * 8; i < n; ++i) h = (h ^ p[i]) * 1099511628211ULL;
    return h;
}

struct Writer {
    std::vector<uint8_t> b;
    template <class T> void Put(const T& v) { auto* p = reinterpret_cast<const uint8_t*>(&v); b.insert(b.end(), p, p + sizeof(T)); }
    void Raw(const void* p, size_t n) { auto* q = static_cast<const uint8_t*>(p); b.insert(b.end(), q, q + n); }
};

struct Reader {
    const uint8_t* p; size_t n, pos = 0; bool ok = true;
    template <class T> T Get() { T v{}; if (pos + sizeof(T) > n) { ok = false; return v; } memcpy(&v, p + pos, sizeof(T)); pos += sizeof(T); return v; }
    template <class T> void Array(std::vector<T>& out, uint64_t count) {
        if (!ok || count > (n - pos) / sizeof(T)) { ok = false; return; }
        out.resize(static_cast<size_t>(count));
        memcpy(out.data(), p + pos, static_cast<size_t>(count) * sizeof(T));
        pos += static_cast<size_t>(count) * sizeof(T);
    }
};

static std::vector<uint8_t> BuildVolTable(const std::vector<VolumeInfo>& vols) {
    std::vector<uint8_t> b(sizeof(VolTableHeader) + vols.size() * sizeof(VolSlot));
    auto* th = reinterpret_cast<VolTableHeader*>(b.data());
    th->count = static_cast<uint32_t>(vols.size());
    auto* slots = reinterpret_cast<VolSlot*>(b.data() + sizeof(VolTableHeader));
    for (size_t i = 0; i < vols.size(); ++i) {
        VolSlot& s = slots[i];
        wcsncpy_s(s.root, vols[i].root.c_str(), _TRUNCATE);
        s.serial = vols[i].serial;
        s.flags = (vols[i].ntfs ? 1u : 0u) | (vols[i].mft ? 2u : 0u);
        s.journalId = vols[i].journalId; s.nextUsn = vols[i].nextUsn;
    }
    th->checksum = Checksum(b.data() + sizeof(VolTableHeader), b.size() - sizeof(VolTableHeader));
    return b;
}

static bool ParseVolTable(const uint8_t* p, size_t n, std::vector<VolumeInfo>& out) {
    if (n < sizeof(VolTableHeader)) return false;
    VolTableHeader th; memcpy(&th, p, sizeof(th));
    if (th.count > 64 || sizeof(VolTableHeader) + th.count * sizeof(VolSlot) != n) return false;
    if (Checksum(p + sizeof(VolTableHeader), n - sizeof(VolTableHeader)) != th.checksum) return false;
    for (uint32_t i = 0; i < th.count; ++i) {
        VolSlot s; memcpy(&s, p + sizeof(VolTableHeader) + i * sizeof(VolSlot), sizeof(s));
        VolumeInfo v; s.root[7] = 0; v.root = s.root; v.serial = s.serial; v.ntfs = s.flags & 1; v.mft = (s.flags & 2) != 0;
        v.journalId = s.journalId; v.nextUsn = s.nextUsn;
        out.push_back(std::move(v));
    }
    return true;
}

bool UpdateDbVolumes(const std::wstring& path, const std::vector<VolumeInfo>& vols) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DbHeader h{}; DWORD rd = 0;
    bool ok = ReadFile(f, &h, sizeof(h), &rd, nullptr) && rd == sizeof(h) && !memcmp(h.magic, "FFDB", 4) && h.version == kDbVersion;
    auto vt = BuildVolTable(vols);
    ok = ok && h.headerSize == sizeof(DbHeader) + vt.size();   // 卷数必须一致，否则需整库保存
    if (ok) {
        DWORD wr = 0;
        ok = WriteFile(f, vt.data(), static_cast<DWORD>(vt.size()), &wr, nullptr) && wr == vt.size() && FlushFileBuffers(f);
    }
    CloseHandle(f);
    return ok;
}

bool SaveDb(const std::wstring& path, const IndexData& d, bool compress, uint64_t* bytesWritten) {
    Writer w;
    w.b.reserve(d.entries.size() * sizeof(Entry) + d.names.size() * 2 + d.byName.size() * 4 + d.frn.size() * 8 + 4096);
    w.Put<uint64_t>(d.folders); w.Put<uint64_t>(d.files);
    w.Put<uint64_t>(d.entries.size()); w.Put<uint64_t>(d.names.size());
    w.Put<uint64_t>(d.byName.size()); w.Put<uint64_t>(d.frn.size());
    w.Raw(d.entries.data(), d.entries.size() * sizeof(Entry));
    w.Raw(d.names.data(), d.names.size() * 2);
    w.Raw(d.byName.data(), d.byName.size() * 4);
    w.Raw(d.frn.data(), d.frn.size() * 8);

    std::vector<uint8_t> packed;
    const std::vector<uint8_t>* payload = &w.b;
    uint32_t flags = 0;
    if (compress) {
        COMPRESSOR_HANDLE c = nullptr;
        if (CreateCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &c)) {
            SIZE_T need = 0;
            Compress(c, w.b.data(), w.b.size(), nullptr, 0, &need);
            packed.resize(need);
            SIZE_T got = 0;
            if (Compress(c, w.b.data(), w.b.size(), packed.data(), packed.size(), &got)) {
                packed.resize(got); payload = &packed; flags |= 1;
            }
            CloseCompressor(c);
        }
    }
    std::vector<uint8_t> vt = BuildVolTable(d.volumes);
    DbHeader h{};
    memcpy(h.magic, "FFDB", 4);
    h.version = kDbVersion; h.flags = flags; h.headerSize = static_cast<uint32_t>(sizeof(DbHeader) + vt.size());
    h.rawSize = w.b.size(); h.storedSize = payload->size(); h.configHash = d.configHash;
    h.checksum = Checksum(payload->data(), payload->size());

    std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) { Log(L"保存数据库失败：无法创建 %s", tmp.c_str()); return false; }
    DWORD wr = 0; bool ok = WriteFile(f, &h, sizeof(h), &wr, nullptr) && WriteFile(f, vt.data(), static_cast<DWORD>(vt.size()), &wr, nullptr);
    for (size_t off = 0; ok && off < payload->size(); off += wr) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(payload->size() - off, 64u << 20));
        ok = WriteFile(f, payload->data() + off, chunk, &wr, nullptr) && wr == chunk;
    }
    ok = ok && FlushFileBuffers(f);
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        Log(L"保存数据库失败（%lu）", GetLastError());
        return false;
    }
    if (bytesWritten) *bytesWritten = h.headerSize + payload->size();
    return true;
}

std::shared_ptr<IndexData> LoadDb(const std::wstring& path, std::wstring& err) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) { err = L"数据库不存在"; return nullptr; }
    LARGE_INTEGER sz{}; GetFileSizeEx(f, &sz);
    DbHeader h{}; DWORD rd = 0;
    if (sz.QuadPart < static_cast<LONGLONG>(sizeof(h)) || !ReadFile(f, &h, sizeof(h), &rd, nullptr) || rd != sizeof(h) ||
        memcmp(h.magic, "FFDB", 4) != 0) { CloseHandle(f); err = L"文件头损坏"; return nullptr; }
    if (h.version != kDbVersion && h.version != 2) { CloseHandle(f); err = L"版本不符"; return nullptr; }
    const bool v2 = h.version == 2;
    if (v2) h.headerSize = sizeof(DbHeader);
    if (h.headerSize < sizeof(DbHeader) || h.headerSize > 1 << 16 ||
        h.storedSize != static_cast<uint64_t>(sz.QuadPart) - h.headerSize || h.rawSize > (8ULL << 30)) { CloseHandle(f); err = L"长度不符（损坏）"; return nullptr; }
    std::vector<VolumeInfo> vols;
    if (!v2) {
        std::vector<uint8_t> vt(h.headerSize - sizeof(DbHeader));
        if (!ReadFile(f, vt.data(), static_cast<DWORD>(vt.size()), &rd, nullptr) || rd != vt.size() || !ParseVolTable(vt.data(), vt.size(), vols)) {
            CloseHandle(f); err = L"卷表损坏"; return nullptr;
        }
    }
    std::vector<uint8_t> stored(static_cast<size_t>(h.storedSize));
    bool ok = true;
    for (size_t off = 0; ok && off < stored.size(); off += rd) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(stored.size() - off, 64u << 20));
        ok = ReadFile(f, stored.data() + off, chunk, &rd, nullptr) && rd == chunk;
    }
    CloseHandle(f);
    if (!ok) { err = L"读取失败"; return nullptr; }
    if (Checksum(stored.data(), stored.size()) != h.checksum) { err = L"校验和不符（损坏）"; return nullptr; }

    std::vector<uint8_t> raw;
    const std::vector<uint8_t>* payload = &stored;
    if (h.flags & 1) {   // 压缩：选择解压加载器
        DECOMPRESSOR_HANDLE dc = nullptr;
        if (!CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &dc)) { err = L"无法创建解压器"; return nullptr; }
        raw.resize(static_cast<size_t>(h.rawSize));
        SIZE_T got = 0;
        ok = Decompress(dc, stored.data(), stored.size(), raw.data(), raw.size(), &got) && got == raw.size();
        CloseDecompressor(dc);
        if (!ok) { err = L"解压失败（损坏）"; return nullptr; }
        stored.clear(); stored.shrink_to_fit();
        payload = &raw;
    } else if (h.rawSize != h.storedSize) { err = L"长度不符（损坏）"; return nullptr; }

    auto d = std::make_shared<IndexData>();
    d->configHash = h.configHash;
    d->dbVersion = h.version;
    Reader r{payload->data(), payload->size()};
    if (v2) {   // v2：卷信息在载荷中，没有 Journal 信息
        uint32_t nv = r.Get<uint32_t>();
        if (nv > 64) { err = L"卷数量异常（损坏）"; return nullptr; }
        for (uint32_t i = 0; i < nv && r.ok; ++i) {
            VolumeInfo v;
            uint16_t len = r.Get<uint16_t>();
            std::vector<wchar_t> s2; r.Array(s2, len); v.root.assign(s2.begin(), s2.end());
            v.serial = r.Get<uint32_t>(); v.ntfs = r.Get<uint8_t>() != 0; r.Get<uint8_t>();
            r.Get<uint64_t>(); r.Get<int64_t>();
            v.mft = false; v.journalId = 0; v.nextUsn = 0;
            vols.push_back(std::move(v));
        }
    }
    d->volumes = std::move(vols);
    d->folders = r.Get<uint64_t>(); d->files = r.Get<uint64_t>();
    uint64_t ne = r.Get<uint64_t>(), nc = r.Get<uint64_t>(), nb = r.Get<uint64_t>(), nf = r.Get<uint64_t>();
    r.Array(d->entries, ne); r.Array(d->names, nc); r.Array(d->byName, nb); r.Array(d->frn, nf);
    if (!r.ok || r.pos != r.n || nb > ne || (nf && nf != ne)) { err = L"结构不符（损坏）"; return nullptr; }
    // 轻量一致性检查：越界即视为损坏
    for (auto& e : d->entries)
        if (static_cast<uint64_t>(e.nameOff) + e.nameLen > nc || (e.parent != kNoParent && e.parent >= ne)) { err = L"条目越界（损坏）"; return nullptr; }
    for (auto i : d->byName) if (i >= ne) { err = L"排序表越界（损坏）"; return nullptr; }
    return d;
}

} // namespace ff
