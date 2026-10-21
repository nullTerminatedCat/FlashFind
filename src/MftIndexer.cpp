#include "MftIndexer.h"
#include "Log.h"
#include <windows.h>
#include <winioctl.h>
#include <algorithm>
#include <cstring>

namespace ff {

static constexpr uint64_t kRecMask = 0x0000FFFFFFFFFFFFULL;   // 仅用于把 FRN 换算成记录号做数组下标；身份比较始终用完整 64 位

static std::wstring ErrText(const wchar_t* what) {
    wchar_t b[256]; swprintf_s(b, L"%s 失败，错误 %lu", what, GetLastError()); return b;
}

bool MftIndexer::IsElevated() {
    static int cached = -1;
    if (cached >= 0) return cached != 0;
    HANDLE tok = nullptr; TOKEN_ELEVATION te{}; DWORD n = 0;
    BOOL ok = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok) && GetTokenInformation(tok, TokenElevation, &te, sizeof(te), &n);
    if (tok) CloseHandle(tok);
    cached = ok && te.TokenIsElevated ? 1 : 0;
    return cached != 0;
}

bool MftIndexer::IsNtfs(const std::wstring& root, uint32_t* serial) {
    std::wstring r = root + L"\\";
    wchar_t fs[32] = {}; DWORD ser = 0;
    if (!GetVolumeInformationW(r.c_str(), nullptr, 0, &ser, nullptr, nullptr, fs, 32)) { if (serial) *serial = 0; return false; }
    if (serial) *serial = ser;
    return wcscmp(fs, L"NTFS") == 0;
}

HANDLE MftIndexer::OpenRoot(const std::wstring& root) {
    std::wstring r = root + L"\\";
    return CreateFileW(r.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
}

static HANDLE OpenVolume(const std::wstring& root, DWORD access) {
    std::wstring p = L"\\\\.\\" + root;
    return CreateFileW(p.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
}

bool MftIndexer::QueryJournal(const std::wstring& root, bool create, uint64_t& journalId, int64_t& firstUsn, int64_t& nextUsn, std::wstring& err) {
    HANDLE h = OpenRoot(root);
    if (h == INVALID_HANDLE_VALUE) { err = ErrText(L"打开根目录"); return false; }
    USN_JOURNAL_DATA_V0 jd{}; DWORD got = 0;
    BOOL ok = DeviceIoControl(h, FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &jd, sizeof(jd), &got, nullptr);
    DWORD e = GetLastError();
    CloseHandle(h);
    if (!ok && (e == ERROR_JOURNAL_NOT_ACTIVE || e == ERROR_JOURNAL_DELETE_IN_PROGRESS) && create) {
        HANDLE v = OpenVolume(root, GENERIC_READ | GENERIC_WRITE);
        CREATE_USN_JOURNAL_DATA cj{32ULL << 20, 8ULL << 20};
        if (v == INVALID_HANDLE_VALUE || !DeviceIoControl(v, FSCTL_CREATE_USN_JOURNAL, &cj, sizeof(cj), nullptr, 0, &got, nullptr)) {
            err = ErrText(L"Failed to create USN Journal");
            Log(L"%s: Failed to create USN Journal（错误 %lu）", root.c_str(), GetLastError());
            if (v != INVALID_HANDLE_VALUE) CloseHandle(v);
            return false;
        }
        CloseHandle(v);
        Log(L"%s: 已创建 USN Journal", root.c_str());
        return QueryJournal(root, false, journalId, firstUsn, nextUsn, err);
    }
    if (!ok) { SetLastError(e); err = ErrText(L"FSCTL_QUERY_USN_JOURNAL"); return false; }
    journalId = jd.UsnJournalID; firstUsn = jd.FirstUsn; nextUsn = jd.NextUsn;
    return true;
}

bool MftIndexer::ReadChanges(const std::wstring& root, uint64_t journalId, int64_t from, int64_t& next,
                             std::unordered_map<uint64_t, UsnChange>& out, std::wstring& err) {
    uint64_t jid = 0; int64_t first = 0, end = 0;
    if (!QueryJournal(root, false, jid, first, end, err)) return false;
    if (jid != journalId) { err = L"Journal ID changed"; return false; }
    if (from < first) { err = L"游标已被 Journal 截断"; return false; }
    next = from;
    if (from >= end) return true;
    const bool priv = IsElevated();
    HANDLE h = priv ? OpenVolume(root, GENERIC_READ) : OpenRoot(root);
    if (h == INVALID_HANDLE_VALUE) { err = ErrText(L"打开卷/根目录"); return false; }
    READ_USN_JOURNAL_DATA_V1 rd{};
    rd.StartUsn = from; rd.ReasonMask = 0xFFFFFFFF; rd.UsnJournalID = journalId;
    rd.MinMajorVersion = 2; rd.MaxMajorVersion = 2;
    const DWORD code = priv ? FSCTL_READ_USN_JOURNAL : FSCTL_READ_UNPRIVILEGED_USN_JOURNAL;
    std::vector<uint8_t> buf(1 << 20);
    DWORD got = 0;
    while (rd.StartUsn < end) {
        if (!DeviceIoControl(h, code, &rd, sizeof(rd), buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) {
            err = ErrText(priv ? L"FSCTL_READ_USN_JOURNAL" : L"FSCTL_READ_UNPRIVILEGED_USN_JOURNAL");
            CloseHandle(h); return false;
        }
        if (got < sizeof(USN)) break;
        for (DWORD off = sizeof(USN); off + sizeof(USN_RECORD_V2) <= got;) {
            auto* rec = reinterpret_cast<USN_RECORD_V2*>(buf.data() + off);
            if (rec->RecordLength == 0) break;
            if (rec->MajorVersion == 2 && rec->Usn < end) {
                auto& c = out[rec->FileReferenceNumber];
                c.parent = rec->ParentFileReferenceNumber;
                c.attrs = rec->FileAttributes;
                c.reasons |= rec->Reason;
                c.deleted = (rec->Reason & USN_REASON_FILE_DELETE) != 0;
                if (rec->FileNameLength)
                    c.name.assign(reinterpret_cast<const wchar_t*>(reinterpret_cast<uint8_t*>(rec) + rec->FileNameOffset), rec->FileNameLength / 2);
            }
            off += rec->RecordLength;
        }
        USN n = *reinterpret_cast<USN*>(buf.data());
        if (n <= rd.StartUsn) break;
        rd.StartUsn = n;
    }
    CloseHandle(h);
    next = std::max<int64_t>(rd.StartUsn, end);
    return true;
}

// OpenFileById：取最后一段名称、大小、修改时间、属性（中完整性下对可访问的文件有效）
static bool InfoById(HANDLE hint, uint64_t frn, std::wstring& name, uint64_t& size, uint64_t& mtime, uint32_t& attrs) {
    FILE_ID_DESCRIPTOR d{};
    d.dwSize = sizeof(d); d.Type = FileIdType; d.FileId.QuadPart = static_cast<LONGLONG>(frn);
    HANDLE h = OpenFileById(hint, &d, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, FILE_FLAG_BACKUP_SEMANTICS);
    if (h == INVALID_HANDLE_VALUE) return false;
    alignas(8) uint8_t nb[sizeof(FILE_NAME_INFO) + 65536];
    bool ok = GetFileInformationByHandleEx(h, FileNameInfo, nb, sizeof(nb)) != FALSE;
    if (ok) {
        auto* fi = reinterpret_cast<FILE_NAME_INFO*>(nb);
        std::wstring_view p(fi->FileName, fi->FileNameLength / 2);
        auto s = p.rfind(L'\\');
        name.assign(s == std::wstring_view::npos ? p : p.substr(s + 1));
    }
    FILE_BASIC_INFO bi{}; FILE_STANDARD_INFO si{};
    if (GetFileInformationByHandleEx(h, FileBasicInfo, &bi, sizeof(bi))) { mtime = static_cast<uint64_t>(bi.LastWriteTime.QuadPart); attrs = bi.FileAttributes; }
    if (GetFileInformationByHandleEx(h, FileStandardInfo, &si, sizeof(si))) size = static_cast<uint64_t>(si.EndOfFile.QuadPart);
    CloseHandle(h);
    return ok && !name.empty();
}

size_t MftIndexer::Apply(VolumeData& v, const std::unordered_map<uint64_t, UsnChange>& changes, size_t* unresolved) {
    size_t n = 0; if (unresolved) *unresolved = 0;
    if (v.frn.size() != v.entries.size()) return 0;
    std::unordered_map<uint64_t, uint32_t> map;
    map.reserve(v.entries.size() + changes.size());
    for (uint32_t i = 0; i < v.entries.size(); ++i) if (!v.entries[i].IsDeleted()) map.emplace(v.frn[i], i);
    HANDLE hint = OpenRoot(v.info.root);
    std::vector<std::pair<uint32_t, uint64_t>> fix;
    for (auto& [frn, c] : changes) {
        auto it = map.find(frn);
        if (c.deleted) {
            if (it != map.end() && it->second != 0) { v.entries[it->second].flags |= kFlagDeleted; map.erase(it); ++n; }
            continue;
        }
        std::wstring name; uint64_t size = 0, mtime = 0; uint32_t attrs = c.attrs;
        bool got = hint != INVALID_HANDLE_VALUE && InfoById(hint, frn, name, size, mtime, attrs);
        if (!got) name = c.name;
        bool dir = attrs & FILE_ATTRIBUTE_DIRECTORY;
        uint32_t idx;
        if (it != map.end()) {
            idx = it->second;
            if (idx == 0) continue;   // 卷根
            Entry& e = v.entries[idx];
            if (!name.empty() && std::wstring_view(v.names.data() + e.nameOff, e.nameLen) != name) {
                e.nameOff = static_cast<uint32_t>(v.names.size());
                e.nameLen = static_cast<uint16_t>(std::min<size_t>(name.size(), 0xFFFF));
                v.names.insert(v.names.end(), name.begin(), name.begin() + e.nameLen);
            }
            e.flags = static_cast<uint16_t>((e.flags & ~kFlagDir) | (dir ? kFlagDir : 0));
            if (got) { e.size = dir ? 0 : size; e.mtime = mtime; }
        } else {
            if (name.empty()) { if (unresolved) ++*unresolved; continue; }
            idx = v.Add(name.c_str(), name.size(), kNoParent, dir ? kFlagDir : 0, dir ? 0 : size, mtime);
            v.frn.push_back(frn);
            map.emplace(frn, idx);
        }
        fix.emplace_back(idx, c.parent);
        ++n;
    }
    if (hint != INVALID_HANDLE_VALUE) CloseHandle(hint);
    for (auto& [idx, parent] : fix) {
        auto it = map.find(parent);   // 完整 64 位比较
        if (it == map.end() || it->second == idx) v.entries[idx].flags |= kFlagDeleted;
        else v.entries[idx].parent = it->second;
    }
    return n;
}

// ---------------- $MFT 顺序读取 ----------------
namespace {
struct Run { int64_t vcn, lcn, len; bool sparse; };

bool Fixup(uint8_t* r, uint32_t recSize, uint32_t sector) {
    uint16_t usaOff, usaCnt; memcpy(&usaOff, r + 4, 2); memcpy(&usaCnt, r + 6, 2);
    if (usaOff + 2u * usaCnt > recSize || usaCnt == 0) return false;
    uint16_t check; memcpy(&check, r + usaOff, 2);
    for (uint32_t k = 1; k < usaCnt; ++k) {
        uint32_t pos = k * sector - 2;
        if (pos + 2 > recSize) break;
        uint16_t v; memcpy(&v, r + pos, 2);
        if (v != check) return false;          // 撕裂写/损坏
        memcpy(r + pos, r + usaOff + 2 * k, 2);
    }
    return true;
}

bool ParseRuns(const uint8_t* p, const uint8_t* end, std::vector<Run>& runs) {
    int64_t vcn = 0, lcn = 0;
    while (p < end && *p) {
        uint8_t ls = *p & 0xF, os = *p >> 4; ++p;
        if (ls == 0 || ls > 8 || os > 8 || p + ls + os > end) return false;
        int64_t len = 0; for (int i = 0; i < ls; ++i) len |= static_cast<int64_t>(p[i]) << (8 * i);
        p += ls;
        int64_t delta = 0;
        if (os) { for (int i = 0; i < os; ++i) delta |= static_cast<int64_t>(p[i]) << (8 * i); if (p[os - 1] & 0x80) delta |= -1LL << (8 * os); }
        p += os;
        lcn += delta;
        runs.push_back({vcn, lcn, len, os == 0});
        vcn += len;
    }
    return true;
}

struct Rec {
    uint64_t parent = 0, size = 0, mtime = 0;
    uint32_t nameOff = 0; uint16_t nameLen = 0; uint16_t seq = 0;
    uint8_t inUse = 0, dir = 0, nameRank = 0, hasSize = 0;
};
} // namespace

bool MftIndexer::Build(VolumeData& v, bool enableUsn, std::atomic<bool>& stop, std::atomic<size_t>& progress, std::wstring& err) {
    HANDLE vol = OpenVolume(v.info.root, GENERIC_READ);
    if (vol == INVALID_HANDLE_VALUE) { err = ErrText(L"打开卷（需要管理员或 FlashFind 服务）"); return false; }
    NTFS_VOLUME_DATA_BUFFER nv{}; DWORD got = 0;
    if (!DeviceIoControl(vol, FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0, &nv, sizeof(nv), &got, nullptr)) { err = ErrText(L"FSCTL_GET_NTFS_VOLUME_DATA"); CloseHandle(vol); return false; }
    // 先取 Journal 游标：读 MFT 期间的变化会在之后被 USN 重放
    uint64_t jid = 0; int64_t first = 0, next = 0;
    if (!QueryJournal(v.info.root, enableUsn, jid, first, next, err)) { CloseHandle(vol); return false; }

    const uint32_t recSize = nv.BytesPerFileRecordSegment, cluster = nv.BytesPerCluster, sector = nv.BytesPerSector;
    const uint64_t total = static_cast<uint64_t>(nv.MftValidDataLength.QuadPart) / recSize;
    auto readAt = [&](int64_t off, uint8_t* dst, DWORD n) {
        OVERLAPPED ov{}; ov.Offset = static_cast<DWORD>(off); ov.OffsetHigh = static_cast<DWORD>(off >> 32);
        DWORD r = 0; return ReadFile(vol, dst, n, &r, &ov) && r == n;
    };
    // 记录 0：$MFT 自身，取 $DATA 运行列表
    std::vector<uint8_t> r0(std::max(cluster, recSize));
    if (!readAt(nv.MftStartLcn.QuadPart * cluster, r0.data(), static_cast<DWORD>(r0.size())) || memcmp(r0.data(), "FILE", 4) || !Fixup(r0.data(), recSize, sector)) {
        err = L"读取 $MFT 记录 0 失败"; CloseHandle(vol); return false;
    }
    std::vector<Run> runs;
    bool hasAttrList = false;
    {
        uint16_t ao; memcpy(&ao, r0.data() + 0x14, 2);
        for (uint32_t a = ao; a + 16 <= recSize;) {
            uint32_t type, alen; memcpy(&type, r0.data() + a, 4); if (type == 0xFFFFFFFF) break;
            memcpy(&alen, r0.data() + a + 4, 4); if (alen < 16 || a + alen > recSize) break;
            if (type == 0x20) hasAttrList = true;
            if (type == 0x80 && r0[a + 8] && r0[a + 9] == 0) {
                uint16_t ro; memcpy(&ro, r0.data() + a + 0x20, 2);
                ParseRuns(r0.data() + a + ro, r0.data() + a + alen, runs);
            }
            a += alen;
        }
    }
    int64_t covered = 0; for (auto& r : runs) covered += r.len;
    if (runs.empty() || static_cast<uint64_t>(covered) * cluster < total * recSize) {
        err = hasAttrList ? L"$MFT 碎片过多（运行列表在属性列表扩展记录中），暂不支持" : L"$MFT 运行列表无效";
        CloseHandle(vol); return false;
    }

    std::vector<Rec> recs(static_cast<size_t>(total));
    std::vector<wchar_t> pool; pool.reserve(static_cast<size_t>(total) * 16);
    const uint32_t chunk = std::max<uint32_t>(cluster, 4u << 20) / cluster * cluster;
    std::vector<uint8_t> buf(chunk);
    uint64_t processed = 0, nextLog = total / 10 + 1;
    for (auto& run : runs) {
        if (stop) break;
        if (run.sparse) continue;
        int64_t bytes = run.len * cluster, base = run.vcn * cluster;
        for (int64_t off = 0; off < bytes && !stop; off += chunk) {
            uint64_t firstRec = static_cast<uint64_t>(base + off) / recSize;
            if (firstRec >= total) break;
            DWORD n = static_cast<DWORD>(std::min<int64_t>(chunk, bytes - off));
            if (!readAt(run.lcn * cluster + off, buf.data(), n)) { err = ErrText(L"读取 $MFT"); CloseHandle(vol); return false; }
            for (uint32_t ro = 0; ro + recSize <= n; ro += recSize) {
                uint64_t recNo = static_cast<uint64_t>(base + off + ro) / recSize;
                if (recNo >= total) break;
                ++processed;
                uint8_t* r = buf.data() + ro;
                if (memcmp(r, "FILE", 4) || !Fixup(r, recSize, sector)) continue;
                uint16_t flags, seq; uint64_t baseRef;
                memcpy(&flags, r + 0x16, 2); memcpy(&seq, r + 0x10, 2); memcpy(&baseRef, r + 0x20, 8);
                if (!(flags & 1)) continue;
                uint64_t baseNo = baseRef ? (baseRef & kRecMask) : recNo;   // 扩展记录：属性归入基记录
                if (baseNo >= total) continue;
                Rec& R = recs[baseNo];
                if (!baseRef) { R.inUse = 1; R.seq = seq; R.dir = (flags & 2) ? 1 : 0; }
                uint16_t ao; memcpy(&ao, r + 0x14, 2);
                for (uint32_t a = ao; a + 16 <= recSize;) {
                    uint32_t type, alen; memcpy(&type, r + a, 4); if (type == 0xFFFFFFFF) break;
                    memcpy(&alen, r + a + 4, 4); if (alen < 16 || a + alen > recSize) break;
                    uint8_t nonres = r[a + 8], anl = r[a + 9];
                    if (!nonres) {
                        uint32_t vl; uint16_t vo; memcpy(&vl, r + a + 0x10, 4); memcpy(&vo, r + a + 0x14, 2);
                        const uint8_t* c = r + a + vo;
                        if (a + vo + vl <= recSize) {
                            if (type == 0x10 && vl >= 16) memcpy(&R.mtime, c + 8, 8);
                            else if (type == 0x30 && vl >= 0x42) {
                                uint8_t nl = c[0x40], ns = c[0x41];
                                uint8_t rank = (ns == 1 || ns == 3) ? 3 : (ns == 0 ? 2 : 1);   // Win32 > POSIX > DOS
                                if (rank > R.nameRank && 0x42u + nl * 2u <= vl) {
                                    memcpy(&R.parent, c, 8);
                                    R.nameOff = static_cast<uint32_t>(pool.size()); R.nameLen = nl; R.nameRank = rank;
                                    const wchar_t* nm = reinterpret_cast<const wchar_t*>(c + 0x42);
                                    pool.insert(pool.end(), nm, nm + nl);
                                }
                            } else if (type == 0x80 && anl == 0 && !R.hasSize) { R.size = vl; R.hasSize = 1; }
                        }
                    } else if (type == 0x80 && anl == 0) {
                        uint64_t lowVcn; memcpy(&lowVcn, r + a + 0x10, 8);
                        if (lowVcn == 0 && a + 0x38 <= recSize) { memcpy(&R.size, r + a + 0x30, 8); R.hasSize = 1; }
                    }
                    a += alen;
                }
            }
            progress += 1;
            if (processed >= nextLog) { Log(L"%s: processed %I64u / %I64u file records", v.info.root.c_str(), processed, total); nextLog += total / 10 + 1; }
        }
    }
    CloseHandle(vol);
    if (stop) { err = L"已取消"; return false; }
    Log(L"%s: processed %I64u / %I64u file records", v.info.root.c_str(), processed, total);

    // 组装：记录 5 = 根；0-11 其余为元文件；无 $FILE_NAME 的记录跳过；$Extend(11) 及元文件的子项因父不存在成为孤儿被丢弃
    auto fullFrn = [&](uint64_t no) { return (static_cast<uint64_t>(recs[no].seq) << 48) | no; };
    if (total <= 5 || !recs[5].inUse) { err = L"根目录记录无效"; return false; }
    v.entries.clear(); v.names.clear(); v.frn.clear();
    std::vector<uint32_t> idxOf(static_cast<size_t>(total), kNoParent);
    std::vector<uint64_t> parentRef;
    v.Add(v.info.root.c_str(), v.info.root.size(), kNoParent, kFlagDir, 0, recs[5].mtime);
    v.frn.push_back(fullFrn(5)); parentRef.push_back(0);
    idxOf[5] = 0;
    Log(L"%s: 根目录 FRN %016I64X", v.info.root.c_str(), fullFrn(5));
    for (uint64_t no = 12; no < total; ++no) {
        const Rec& R = recs[no];
        if (!R.inUse || !R.nameRank) continue;
        uint32_t i = v.Add(pool.data() + R.nameOff, R.nameLen, kNoParent, R.dir ? kFlagDir : 0, R.dir ? 0 : R.size, R.mtime);
        v.frn.push_back(fullFrn(no)); parentRef.push_back(R.parent);
        idxOf[no] = i;
    }
    size_t orphans = 0;
    for (uint32_t i = 1; i < v.entries.size(); ++i) {
        uint64_t p = parentRef[i], pn = p & kRecMask;
        if (pn < total && idxOf[pn] != kNoParent && fullFrn(pn) == p && idxOf[pn] != i) v.entries[i].parent = idxOf[pn];
        else { v.entries[i].flags |= kFlagDeleted; ++orphans; }
    }
    Log(L"%s: %zu 个对象，%zu 个孤儿/元文件子项被排除", v.info.root.c_str(), v.entries.size(), orphans);
    v.info.mft = true; v.info.journalId = jid; v.info.nextUsn = next;
    return true;
}

void MftIndexer::EnumerateWithIds(VolumeData& v, const std::vector<std::wstring>& excludes, std::atomic<bool>& stop, std::atomic<size_t>& progress) {
    v.entries.clear(); v.names.clear(); v.frn.clear();
    HANDLE rh = OpenRoot(v.info.root);
    BY_HANDLE_FILE_INFORMATION bi{};
    uint64_t rootFrn = 0;
    if (rh != INVALID_HANDLE_VALUE) { if (GetFileInformationByHandle(rh, &bi)) rootFrn = (static_cast<uint64_t>(bi.nFileIndexHigh) << 32) | bi.nFileIndexLow; CloseHandle(rh); }
    v.Add(v.info.root.c_str(), v.info.root.size(), kNoParent, kFlagDir, 0, 0);
    v.frn.push_back(rootFrn);
    struct P { uint32_t idx; std::wstring path; };
    std::vector<P> stack{{0, v.info.root}};
    std::vector<uint8_t> buf(256 << 10);
    while (!stack.empty() && !stop) {
        P cur = std::move(stack.back()); stack.pop_back();
        std::wstring dirPath = cur.path + L"\\";
        HANDLE h = CreateFileW(dirPath.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        bool firstCall = true;
        while (!stop && GetFileInformationByHandleEx(h, firstCall ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo, buf.data(), static_cast<DWORD>(buf.size()))) {
            firstCall = false;
            for (uint8_t* p = buf.data();;) {
                auto* fi = reinterpret_cast<FILE_ID_BOTH_DIR_INFO*>(p);
                size_t nl = fi->FileNameLength / 2;
                const wchar_t* n = fi->FileName;
                if (!(n[0] == L'.' && (nl == 1 || (nl == 2 && n[1] == L'.')))) {
                    bool dir = fi->FileAttributes & FILE_ATTRIBUTE_DIRECTORY;
                    uint32_t idx = v.Add(n, nl, cur.idx, dir ? kFlagDir : 0, dir ? 0 : static_cast<uint64_t>(fi->EndOfFile.QuadPart),
                                         static_cast<uint64_t>(fi->LastWriteTime.QuadPart));
                    v.frn.push_back(static_cast<uint64_t>(fi->FileId.QuadPart));
                    if (dir && !(fi->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                        std::wstring child = cur.path + L"\\" + std::wstring(n, nl);
                        bool ex = false;
                        for (auto& e : excludes) if (CompareStringOrdinal(child.c_str(), -1, e.c_str(), -1, TRUE) == CSTR_EQUAL) { ex = true; break; }
                        if (!ex) stack.push_back({idx, std::move(child)});
                    }
                }
                if (!fi->NextEntryOffset) break;
                p += fi->NextEntryOffset;
            }
        }
        CloseHandle(h);
        ++progress;
    }
}

} // namespace ff
