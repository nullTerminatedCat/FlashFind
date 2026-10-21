#include "Db.h"
#include "Log.h"
#include <windows.h>
#include <compressapi.h>
#include <algorithm>
#include <cstring>

namespace ff {

#pragma pack(push, 1)
struct DbHeader {
    char magic[4];          // "FFDB"
    uint32_t version;
    uint32_t flags;         // bit0 = 载荷 XPRESS_HUFF 压缩
    uint32_t headerSize;
    uint64_t rawSize;       // 解压后载荷大小
    uint64_t storedSize;    // 文件中载荷大小
    uint64_t configHash;
    uint64_t checksum;      // 存储载荷的 FNV-1a(64 位字)
};
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

bool SaveDb(const std::wstring& path, const IndexData& d, bool compress, uint64_t* bytesWritten) {
    Writer w;
    w.b.reserve(d.entries.size() * sizeof(Entry) + d.names.size() * 2 + d.byName.size() * 4 + d.frn.size() * 8 + 4096);
    w.Put<uint32_t>(static_cast<uint32_t>(d.volumes.size()));
    for (auto& v : d.volumes) {
        w.Put<uint16_t>(static_cast<uint16_t>(v.root.size()));
        w.Raw(v.root.data(), v.root.size() * 2);
        w.Put<uint32_t>(v.serial);
        w.Put<uint8_t>(v.ntfs); w.Put<uint8_t>(v.mft);
        w.Put<uint64_t>(v.journalId); w.Put<int64_t>(v.nextUsn);
    }
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
    DbHeader h{};
    memcpy(h.magic, "FFDB", 4);
    h.version = kDbVersion; h.flags = flags; h.headerSize = sizeof(DbHeader);
    h.rawSize = w.b.size(); h.storedSize = payload->size(); h.configHash = d.configHash;
    h.checksum = Checksum(payload->data(), payload->size());

    std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) { Log(L"保存数据库失败：无法创建 %s", tmp.c_str()); return false; }
    DWORD wr = 0; bool ok = WriteFile(f, &h, sizeof(h), &wr, nullptr) != FALSE;
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
    if (bytesWritten) *bytesWritten = sizeof(h) + payload->size();
    return true;
}

std::shared_ptr<IndexData> LoadDb(const std::wstring& path, std::wstring& err) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) { err = L"数据库不存在"; return nullptr; }
    LARGE_INTEGER sz{}; GetFileSizeEx(f, &sz);
    DbHeader h{}; DWORD rd = 0;
    if (sz.QuadPart < static_cast<LONGLONG>(sizeof(h)) || !ReadFile(f, &h, sizeof(h), &rd, nullptr) || rd != sizeof(h) ||
        memcmp(h.magic, "FFDB", 4) != 0) { CloseHandle(f); err = L"文件头损坏"; return nullptr; }
    if (h.version != kDbVersion) { CloseHandle(f); err = L"版本不符"; return nullptr; }
    if (h.storedSize != static_cast<uint64_t>(sz.QuadPart) - sizeof(h) || h.rawSize > (8ULL << 30)) { CloseHandle(f); err = L"长度不符（损坏）"; return nullptr; }
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
    Reader r{payload->data(), payload->size()};
    uint32_t nv = r.Get<uint32_t>();
    if (nv > 64) { err = L"卷数量异常（损坏）"; return nullptr; }
    for (uint32_t i = 0; i < nv && r.ok; ++i) {
        VolumeInfo v;
        uint16_t len = r.Get<uint16_t>();
        std::vector<wchar_t> s; r.Array(s, len); v.root.assign(s.begin(), s.end());
        v.serial = r.Get<uint32_t>(); v.ntfs = r.Get<uint8_t>() != 0; v.mft = r.Get<uint8_t>() != 0;
        v.journalId = r.Get<uint64_t>(); v.nextUsn = r.Get<int64_t>();
        d->volumes.push_back(std::move(v));
    }
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
