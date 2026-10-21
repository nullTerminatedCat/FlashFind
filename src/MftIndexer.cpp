#include "MftIndexer.h"
#include "Log.h"
#include <windows.h>
#include <winioctl.h>
#include <cstring>
#include <unordered_map>

namespace ff {

static constexpr uint64_t kFrnMask = 0x0000FFFFFFFFFFFFULL;   // 48 位 MFT 记录号（去掉序列号）

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

static HANDLE OpenVolume(const std::wstring& root) {
    std::wstring p = L"\\\\.\\" + root;
    return CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
}

static std::wstring ErrText(const wchar_t* what) {
    wchar_t b[128]; swprintf_s(b, L"%s 失败，错误 %lu", what, GetLastError()); return b;
}

// 读取一个文件记录，取 $STANDARD_INFORMATION 修改时间与无名 $DATA 大小
static bool ReadRecord(HANDLE vol, uint64_t frn, std::vector<uint8_t>& buf, uint64_t& size, uint64_t& mtime) {
    NTFS_FILE_RECORD_INPUT_BUFFER in{};
    in.FileReferenceNumber.QuadPart = static_cast<LONGLONG>(frn & kFrnMask);
    DWORD got = 0;
    if (!DeviceIoControl(vol, FSCTL_GET_NTFS_FILE_RECORD, &in, sizeof(in), buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) return false;
    auto* o = reinterpret_cast<NTFS_FILE_RECORD_OUTPUT_BUFFER*>(buf.data());
    if ((static_cast<uint64_t>(o->FileReferenceNumber.QuadPart) & kFrnMask) != (frn & kFrnMask)) return false;
    uint8_t* r = o->FileRecordBuffer;
    const uint32_t len = o->FileRecordLength;
    if (len < 0x30 || memcmp(r, "FILE", 4) != 0) return false;
    auto u16 = [&](uint32_t off) { uint16_t v; memcpy(&v, r + off, 2); return v; };
    auto u32 = [&](uint32_t off) { uint32_t v; memcpy(&v, r + off, 4); return v; };
    auto u64 = [&](uint32_t off) { uint64_t v; memcpy(&v, r + off, 8); return v; };
    // 更新序列修正（usa 中存的是原始字节，重复写入是幂等的）
    uint16_t usaOff = u16(4), usaCnt = u16(6);
    for (uint32_t k = 1; k < usaCnt; ++k) {
        uint32_t pos = k * 512 - 2;
        if (pos + 2 > len || usaOff + 2 * k + 2 > len) break;
        memcpy(r + pos, r + usaOff + 2 * k, 2);
    }
    uint32_t a = u16(0x14);
    size = 0; mtime = 0;
    while (a + 16 <= len) {
        uint32_t type = u32(a);
        if (type == 0xFFFFFFFF) break;
        uint32_t alen = u32(a + 4);
        if (alen < 16 || a + alen > len) break;
        uint8_t nonres = r[a + 8], nameLen = r[a + 9];
        if (type == 0x10 && !nonres) {
            uint16_t vo = u16(a + 0x14);
            if (a + vo + 16 <= len) mtime = u64(a + vo + 8);
        } else if (type == 0x80 && nameLen == 0) {
            size = nonres ? (a + 0x38 <= len ? u64(a + 0x30) : 0) : u32(a + 0x10);
        }
        a += alen;
    }
    return true;
}

bool MftIndexer::Build(VolumeData& v, std::atomic<bool>& stop, std::atomic<size_t>& progress, std::wstring& err) {
    HANDLE vol = OpenVolume(v.info.root);
    if (vol == INVALID_HANDLE_VALUE) { err = ErrText(L"打开卷"); return false; }
    NTFS_VOLUME_DATA_BUFFER nv{}; DWORD got = 0;
    if (!DeviceIoControl(vol, FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0, &nv, sizeof(nv), &got, nullptr)) { err = ErrText(L"FSCTL_GET_NTFS_VOLUME_DATA"); CloseHandle(vol); return false; }
    USN_JOURNAL_DATA_V0 jd{};
    if (!DeviceIoControl(vol, FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &jd, sizeof(jd), &got, nullptr)) {
        // 不自动创建 USN Journal（会修改系统卷），交给用户在设置中决定
        err = ErrText(L"FSCTL_QUERY_USN_JOURNAL（Journal 未启用？）"); CloseHandle(vol); return false;
    }
    std::wstring rootPath = v.info.root + L"\\";
    HANDLE rh = CreateFileW(rootPath.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    BY_HANDLE_FILE_INFORMATION bi{};
    if (rh == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(rh, &bi)) { err = ErrText(L"读取根目录 FRN"); if (rh != INVALID_HANDLE_VALUE) CloseHandle(rh); CloseHandle(vol); return false; }
    CloseHandle(rh);
    const uint64_t rootFrn = (static_cast<uint64_t>(bi.nFileIndexHigh) << 32) | bi.nFileIndexLow;

    v.entries.clear(); v.names.clear(); v.frn.clear();
    size_t estimate = static_cast<size_t>(nv.MftValidDataLength.QuadPart / nv.BytesPerFileRecordSegment);
    v.entries.reserve(estimate); v.frn.reserve(estimate); v.names.reserve(estimate * 20);
    std::vector<uint64_t> parentFrn; parentFrn.reserve(estimate);
    v.Add(v.info.root.c_str(), v.info.root.size(), kNoParent, kFlagDir, 0, 0);
    v.frn.push_back(rootFrn); parentFrn.push_back(0);

    // 1) 枚举 MFT（名称 / 父 FRN / 属性）
    MFT_ENUM_DATA_V0 med{0, 0, jd.NextUsn};
    std::vector<uint8_t> buf(1 << 20);
    while (!stop) {
        if (!DeviceIoControl(vol, FSCTL_ENUM_USN_DATA, &med, sizeof(med), buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) {
            if (GetLastError() == ERROR_HANDLE_EOF) break;
            err = ErrText(L"FSCTL_ENUM_USN_DATA"); CloseHandle(vol); return false;
        }
        if (got <= sizeof(USN)) break;
        uint32_t off = sizeof(USN);
        while (off < got) {
            auto* rec = reinterpret_cast<USN_RECORD_V2*>(buf.data() + off);
            if (rec->RecordLength == 0) break;
            if (rec->MajorVersion == 2 && (rec->FileReferenceNumber & kFrnMask) >= 24 && (rec->FileReferenceNumber & kFrnMask) != (rootFrn & kFrnMask)) {
                const wchar_t* nm = reinterpret_cast<const wchar_t*>(reinterpret_cast<uint8_t*>(rec) + rec->FileNameOffset);
                bool dir = rec->FileAttributes & FILE_ATTRIBUTE_DIRECTORY;
                v.Add(nm, rec->FileNameLength / 2, kNoParent, dir ? kFlagDir : 0, 0, 0);
                v.frn.push_back(rec->FileReferenceNumber);
                parentFrn.push_back(rec->ParentFileReferenceNumber);
            }
            off += rec->RecordLength;
        }
        med.StartFileReferenceNumber = *reinterpret_cast<USN*>(buf.data());
        progress += 1;
    }
    if (stop) { CloseHandle(vol); err = L"已取消"; return false; }

    // 2) 父 FRN -> 下标
    std::unordered_map<uint64_t, uint32_t> map;
    map.reserve(v.entries.size() * 2);
    for (uint32_t i = 0; i < v.entries.size(); ++i) map.emplace(v.frn[i] & kFrnMask, i);
    for (uint32_t i = 1; i < v.entries.size(); ++i) {
        auto it = map.find(parentFrn[i] & kFrnMask);
        if (it == map.end() || it->second == i) v.entries[i].flags |= kFlagDeleted;   // 孤儿（系统元文件等）
        else v.entries[i].parent = it->second;
    }

    // 3) 大小 / 修改时间：逐记录 FSCTL_GET_NTFS_FILE_RECORD
    std::vector<uint8_t> rb(sizeof(NTFS_FILE_RECORD_OUTPUT_BUFFER) + nv.BytesPerFileRecordSegment + 64);
    for (uint32_t i = 1; i < v.entries.size() && !stop; ++i) {
        auto& e = v.entries[i];
        if (e.IsDeleted()) continue;
        uint64_t sz = 0, mt = 0;
        if (ReadRecord(vol, v.frn[i], rb, sz, mt)) { e.size = e.IsDir() ? 0 : sz; e.mtime = mt; }
        if ((i & 0xFFF) == 0) progress += 1;
    }
    CloseHandle(vol);
    if (stop) { err = L"已取消"; return false; }
    v.info.mft = true;
    v.info.journalId = jd.UsnJournalID;
    v.info.nextUsn = jd.NextUsn;
    return true;
}

bool MftIndexer::ApplyUsn(VolumeData& v, std::atomic<bool>& stop, size_t* changes, std::wstring& err) {
    *changes = 0;
    if (v.frn.size() != v.entries.size()) { err = L"缺少 FRN"; return false; }
    HANDLE vol = OpenVolume(v.info.root);
    if (vol == INVALID_HANDLE_VALUE) { err = ErrText(L"打开卷"); return false; }
    USN_JOURNAL_DATA_V0 jd{}; DWORD got = 0;
    if (!DeviceIoControl(vol, FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &jd, sizeof(jd), &got, nullptr)) { err = ErrText(L"FSCTL_QUERY_USN_JOURNAL"); CloseHandle(vol); return false; }
    if (jd.UsnJournalID != v.info.journalId) { err = L"Journal ID 已变化"; CloseHandle(vol); return false; }
    if (v.info.nextUsn < jd.FirstUsn) { err = L"保存的 USN 已被截断"; CloseHandle(vol); return false; }

    struct Change { uint64_t frn = 0, parent = 0; uint32_t attrs = 0; bool deleted = false; std::wstring name; };
    std::unordered_map<uint64_t, Change> ch;   // 每个 FRN 只保留最终状态
    READ_USN_JOURNAL_DATA_V0 rd{};
    rd.StartUsn = v.info.nextUsn; rd.ReasonMask = 0xFFFFFFFF; rd.UsnJournalID = jd.UsnJournalID;
    std::vector<uint8_t> buf(1 << 20);
    while (!stop && rd.StartUsn < jd.NextUsn) {
        if (!DeviceIoControl(vol, FSCTL_READ_USN_JOURNAL, &rd, sizeof(rd), buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) {
            err = ErrText(L"FSCTL_READ_USN_JOURNAL"); CloseHandle(vol); return false;
        }
        if (got <= sizeof(USN)) break;
        for (uint32_t off = sizeof(USN); off < got;) {
            auto* rec = reinterpret_cast<USN_RECORD_V2*>(buf.data() + off);
            if (rec->RecordLength == 0) break;
            if (rec->MajorVersion == 2 && rec->Usn < jd.NextUsn) {
                auto& c = ch[rec->FileReferenceNumber & kFrnMask];
                c.frn = rec->FileReferenceNumber; c.parent = rec->ParentFileReferenceNumber; c.attrs = rec->FileAttributes;
                c.name.assign(reinterpret_cast<const wchar_t*>(reinterpret_cast<uint8_t*>(rec) + rec->FileNameOffset), rec->FileNameLength / 2);
                c.deleted = (rec->Reason & USN_REASON_FILE_DELETE) != 0;
            }
            off += rec->RecordLength;
        }
        USN next = *reinterpret_cast<USN*>(buf.data());
        if (next <= rd.StartUsn) break;
        rd.StartUsn = next;
    }
    if (stop) { CloseHandle(vol); err = L"已取消"; return false; }

    std::unordered_map<uint64_t, uint32_t> map;
    map.reserve(v.entries.size() + ch.size());
    for (uint32_t i = 0; i < v.entries.size(); ++i) map.emplace(v.frn[i] & kFrnMask, i);
    std::vector<std::pair<uint32_t, uint64_t>> fixParent;
    NTFS_VOLUME_DATA_BUFFER nv{};
    DeviceIoControl(vol, FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0, &nv, sizeof(nv), &got, nullptr);
    std::vector<uint8_t> rb(sizeof(NTFS_FILE_RECORD_OUTPUT_BUFFER) + (nv.BytesPerFileRecordSegment ? nv.BytesPerFileRecordSegment : 4096) + 64);
    for (auto& [key, c] : ch) {
        if (key < 24) continue;
        auto it = map.find(key);
        if (c.deleted) {
            if (it != map.end() && it->second != 0) { v.entries[it->second].flags |= kFlagDeleted; ++*changes; }
            continue;
        }
        uint64_t sz = 0, mt = 0;
        ReadRecord(vol, c.frn, rb, sz, mt);
        bool dir = c.attrs & FILE_ATTRIBUTE_DIRECTORY;
        uint32_t idx;
        if (it != map.end()) {
            idx = it->second;
            Entry& e = v.entries[idx];
            std::wstring_view cur(v.names.data() + e.nameOff, e.nameLen);
            if (cur != c.name) {
                e.nameOff = static_cast<uint32_t>(v.names.size());
                e.nameLen = static_cast<uint16_t>(c.name.size());
                v.names.insert(v.names.end(), c.name.begin(), c.name.end());
            }
            e.flags = static_cast<uint16_t>((e.flags & ~(kFlagDir | kFlagDeleted)) | (dir ? kFlagDir : 0));
            e.size = dir ? 0 : sz; e.mtime = mt;
        } else {
            idx = v.Add(c.name.c_str(), c.name.size(), kNoParent, dir ? kFlagDir : 0, dir ? 0 : sz, mt);
            v.frn.push_back(c.frn);
            map.emplace(key, idx);
        }
        fixParent.emplace_back(idx, c.parent);
        ++*changes;
    }
    for (auto& [idx, pf] : fixParent) {
        auto it = map.find(pf & kFrnMask);
        if (it == map.end() || it->second == idx) v.entries[idx].flags |= kFlagDeleted;
        else v.entries[idx].parent = it->second;
    }
    CloseHandle(vol);
    v.info.nextUsn = jd.NextUsn;
    return true;
}

} // namespace ff
