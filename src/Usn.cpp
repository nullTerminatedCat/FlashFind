#include "Usn.h"
#include "Fold.h"
#include "Log.h"
#include <winioctl.h>
#include <algorithm>
#include <chrono>

namespace ff {

using Clock = std::chrono::steady_clock;
static double Ms(Clock::time_point a) { return std::chrono::duration<double, std::milli>(Clock::now() - a).count(); }

bool ParseUsnRecords(const uint8_t* p, size_t n, std::vector<UsnEvent>& out, std::wstring& err, int& logBudget) {
    constexpr size_t kHdr = offsetof(USN_RECORD_V2, FileName);   // 60
    size_t off = 0;
    while (off + 8 <= n) {
        uint32_t len; memcpy(&len, p + off, 4);
        uint16_t major; memcpy(&major, p + off + 4, 2);
        if (len == 0) break;
        if (len < kHdr || off + len > n || (len & 7)) {
            wchar_t b[128]; swprintf_s(b, L"bad record length %u at offset %zu", len, off); err = b;
            Log(L"USN: %s", b); return false;
        }
        if (major != 2) { off += len; continue; }   // 只请求 V2；其他版本跳过
        USN_RECORD_V2 r; memcpy(&r, p + off, kHdr);
        if (r.FileNameOffset < kHdr || (r.FileNameLength & 1) || static_cast<size_t>(r.FileNameOffset) + r.FileNameLength > len) {
            wchar_t b[160]; swprintf_s(b, L"bad file name offset %u length %u record length %u", r.FileNameOffset, r.FileNameLength, len); err = b;
            Log(L"USN: %s", b); return false;
        }
        UsnEvent e;
        e.frn = r.FileReferenceNumber; e.parent = r.ParentFileReferenceNumber;
        e.reason = r.Reason; e.attrs = r.FileAttributes; e.usn = r.Usn; e.time = r.TimeStamp.QuadPart;
        e.name.assign(reinterpret_cast<const wchar_t*>(p + off + r.FileNameOffset), r.FileNameLength / 2);
        if (logBudget > 0) {   // 仅 -debug-log
            --logBudget;
            Log(L"USN_RECORD: parent %016I64x frn %016I64x reason %08x %s", e.parent, e.frn, e.reason, e.name.c_str());
        }
        out.push_back(std::move(e));
        off += len;
    }
    return true;
}

void EncodeUsnRecord(std::vector<uint8_t>& buf, const UsnEvent& e) {
    constexpr size_t kHdr = offsetof(USN_RECORD_V2, FileName);
    size_t len = (kHdr + e.name.size() * 2 + 7) & ~size_t(7);
    size_t at = buf.size();
    buf.resize(at + len, 0);
    USN_RECORD_V2 r{};
    r.RecordLength = static_cast<DWORD>(len); r.MajorVersion = 2;
    r.FileReferenceNumber = e.frn; r.ParentFileReferenceNumber = e.parent;
    r.Usn = e.usn; r.TimeStamp.QuadPart = e.time; r.Reason = e.reason; r.FileAttributes = e.attrs;
    r.FileNameLength = static_cast<WORD>(e.name.size() * 2); r.FileNameOffset = static_cast<WORD>(kHdr);
    memcpy(buf.data() + at, &r, kHdr);
    memcpy(buf.data() + at + kHdr, e.name.data(), e.name.size() * 2);
}

// ---------------- LiveUpdater ----------------
void LiveUpdater::SetFilter(uint32_t vol, std::vector<std::wstring> excludes, std::vector<std::wstring> includeOnly) {
    if (vols_.size() <= vol) vols_.resize(vol + 1);
    vols_[vol].excludes = std::move(excludes); vols_[vol].includeOnly = std::move(includeOnly);
}

void LiveUpdater::Rebase(std::shared_ptr<const IndexData> base) {
    base_ = std::move(base); work_.reset();
    for (auto& v : vols_) { v.built = false; v.sorted.clear(); v.added.clear(); }
    removed_.clear(); insert_.clear(); folderDelta_ = fileDelta_ = 0;
    firstChild_.clear(); nextSib_.clear(); childBuilt_ = false;
}

void LiveUpdater::EnsureWork() {
    if (work_) return;
    work_ = std::make_shared<IndexData>();
    work_->entries = base_->entries; work_->names = base_->names; work_->frn = base_->frn;
    work_->volumes = base_->volumes; work_->configHash = base_->configHash; work_->dbVersion = base_->dbVersion;
    work_->folders = base_->folders; work_->files = base_->files;
    work_->entries.ResetClones(); work_->frn.ResetClones();
    work_->names.SetLogGrowth(true);
}

void LiveUpdater::EnsureFrnIndex(uint32_t vol) {
    if (vols_.size() <= vol) vols_.resize(vol + 1);
    if (vols_[vol].built) return;
    auto t = Clock::now();
    const IndexData& d = work_ ? *work_ : *base_;
    // 一次扫描建立所有卷的 FRN 索引（排序数组：每项 12→16 字节，比哈希表省一半以上）
    uint32_t nv = static_cast<uint32_t>(std::max(vols_.size(), d.volumes.size()));
    vols_.resize(nv);
    if (d.frn.size() == d.entries.size()) {
        for (size_t i = 0; i < d.entries.size(); ++i) {
            const Entry& e = d.entries[i];
            if (e.IsDeleted()) continue;
            auto& v = vols_[e.Vol()];
            if (!v.built) v.sorted.emplace_back(d.frn[i], static_cast<uint32_t>(i));
        }
    }
    for (auto& v : vols_) if (!v.built) { std::sort(v.sorted.begin(), v.sorted.end()); v.built = true; }
    if (!childBuilt_) {
        const size_t n = d.entries.size();
        firstChild_.assign(n, kNoParent); nextSib_.assign(n, kNoParent);
        for (size_t i = n; i-- > 0;) {   // 逆序头插：子链表按下标升序
            const Entry& e = d.entries[i];
            if (e.IsDeleted() || e.parent == kNoParent) continue;
            nextSib_[i] = firstChild_[e.parent]; firstChild_[e.parent] = static_cast<uint32_t>(i);
        }
        childBuilt_ = true;
    }
    st_.frnIndexMs += Ms(t);
}

size_t LiveUpdater::Find(uint32_t vol, uint64_t frn) const {
    if (vol >= vols_.size()) return SIZE_MAX;
    const auto& v = vols_[vol];
    const IndexData& d = work_ ? *work_ : *base_;
    size_t idx = SIZE_MAX;
    if (auto it = v.added.find(frn); it != v.added.end()) idx = it->second;
    else {
        auto it2 = std::lower_bound(v.sorted.begin(), v.sorted.end(), std::make_pair(frn, uint32_t(0)));
        if (it2 != v.sorted.end() && it2->first == frn) idx = it2->second;
    }
    if (idx == SIZE_MAX || d.entries[idx].IsDeleted()) return SIZE_MAX;
    return idx;
}

void LiveUpdater::Link(uint32_t child, uint32_t parent) {
    if (firstChild_.size() <= child) { firstChild_.resize(child + 1, kNoParent); nextSib_.resize(child + 1, kNoParent); }
    if (parent == kNoParent) return;
    nextSib_[child] = firstChild_[parent]; firstChild_[parent] = child;
}

void LiveUpdater::Unlink(uint32_t child, uint32_t parent) {
    if (parent == kNoParent || parent >= firstChild_.size()) return;
    uint32_t* p = &firstChild_[parent];
    while (*p != kNoParent) { if (*p == child) { *p = nextSib_[child]; nextSib_[child] = kNoParent; return; } p = &nextSib_[*p]; }
}

// 文件夹删除：沿反向索引在内存中移除整棵子树（不做全量扫描）
void LiveUpdater::RemoveTree(uint32_t idx, bool logIt) {
    auto t = Clock::now();
    EnsureWork();
    auto& E = work_->entries;
    std::wstring path;
    if (logIt) { path = work_->FullPath(idx); Log(L"remove ntfs folder %s with children", path.c_str()); }
    size_t dirs = 0, files = 0;
    std::vector<uint32_t> stack;
    for (uint32_t c = idx < firstChild_.size() ? firstChild_[idx] : kNoParent; c != kNoParent; c = nextSib_[c]) stack.push_back(c);
    while (!stack.empty()) {
        uint32_t i = stack.back(); stack.pop_back();
        if (E[i].IsDeleted()) continue;
        if (E[i].IsDir()) for (uint32_t c = i < firstChild_.size() ? firstChild_[i] : kNoParent; c != kNoParent; c = nextSib_[c]) stack.push_back(c);
        Entry& x = E.Mut(i);
        x.flags |= kFlagDeleted;
        MarkRemoved(i);
        if (x.IsDir()) { ++dirs; --folderDelta_; } else { ++files; --fileDelta_; }
    }
    st_.removedDirs += dirs; st_.removedFiles += files;
    double ms = Ms(t); st_.subtreeMs += ms;
    if (logIt) Log(L"removed %zu subfolders and %zu files from %s in %f seconds", dirs, files, path.c_str(), ms / 1000.0);
}

// 移入索引范围的文件夹（来自排除目录/范围外）：立即扫描该目录并把子树插入现有索引
void LiveUpdater::ScanMovedIn(uint32_t vol, uint32_t dirIdx) {
    if (!scanner_) return;
    auto t = Clock::now();
    std::wstring path = work_->FullPath(dirIdx);
    Log(L"scanning %s", path.c_str());
    VolumeData vd;
    if (!scanner_(vol, path, vd) || vd.entries.empty()) { Log(L"scan failed: %s", path.c_str()); return; }
    std::vector<uint32_t> map(vd.entries.size(), kNoParent);
    map[0] = dirIdx;
    size_t files = 0, dirs = 0;
    for (size_t i = 1; i < vd.entries.size(); ++i) {
        const Entry& s = vd.entries[i];
        uint32_t p = s.parent < map.size() ? map[s.parent] : kNoParent;
        if (p == kNoParent) continue;
        uint64_t frn = i < vd.frn.size() ? vd.frn[i] : 0;
        size_t ex = frn ? Find(vol, frn) : SIZE_MAX;
        if (ex != SIZE_MAX) { map[i] = static_cast<uint32_t>(ex); continue; }   // 已在索引中（之后的 USN 记录先到）
        std::wstring nm(vd.names.data() + s.nameOff, s.nameLen);
        uint32_t ni = AddEntry(vol, nm, p, s.IsDir(), static_cast<int64_t>(s.mtime), frn);
        work_->entries.Mut(ni).size = s.size;
        map[i] = ni;
        (s.IsDir() ? dirs : files)++;
    }
    ++st_.scans; st_.readdFiles += files; st_.readdFolders += dirs;
    double ms = Ms(t); st_.scanMs += ms;
    Log(L"scan completed in %f seconds", ms / 1000.0);
    Log(L"readd files %zu", files);
    Log(L"readd folders %zu", dirs);
}

void LiveUpdater::MarkRemoved(uint32_t id) {
    if (id < base_->entries.size() && !base_->entries[id].IsDeleted()) removed_.push_back(id);
}

uint32_t LiveUpdater::AddEntry(uint32_t vol, const std::wstring& name, uint32_t parent, bool dir, int64_t time, uint64_t frn) {
    EnsureWork();
    auto& d = *work_;
    Entry e;
    size_t len = std::min<size_t>(name.size(), 0xFFFF);
    e.nameOff = d.names.Append(name.data(), len);
    e.nameLen = static_cast<uint16_t>(len);
    e.flags = static_cast<uint16_t>((dir ? kFlagDir : 0) | (vol << 8));
    e.parent = parent; e.mtime = static_cast<uint64_t>(time);
    uint32_t idx = static_cast<uint32_t>(d.entries.size());
    d.entries.push_back(e);
    if (d.frn.size() < idx) d.frn.resize(idx, 0);
    d.frn.push_back(frn);
    vols_[vol].added[frn] = idx;
    Link(idx, parent);
    insert_.push_back(idx);
    if (dir) ++folderDelta_; else ++fileDelta_;
    return idx;
}

bool LiveUpdater::Filtered(uint32_t vol, uint32_t parent, const std::wstring& name, bool dir) const {
    const auto& v = vols_[vol];
    if (v.excludes.empty() && v.includeOnly.empty()) return false;
    const IndexData& d = work_ ? *work_ : *base_;
    wchar_t buf[32768];
    size_t n = d.FullPathInto(parent, buf, 32768);
    if (n && buf[n - 1] != L'\\' && n < 32767) buf[n++] = L'\\';
    size_t c = std::min(name.size(), 32767 - n);
    wmemcpy(buf + n, name.data(), c); n += c;
    return PathFiltered({buf, n}, dir, v.excludes, v.includeOnly);
}

void LiveUpdater::Apply(uint32_t vol, const std::vector<UsnEvent>& ev, const Lookup& lookup) {
    auto t = Clock::now();
    EnsureFrnIndex(vol);
    std::vector<uint32_t> need;
    int logBudget = g_debugLog ? 200 : 0;
    auto removeOne = [&](size_t idx, const UsnEvent& e) {
        EnsureWork();
        Entry& x = work_->entries.Mut(idx);
        x.flags |= kFlagDeleted;
        MarkRemoved(static_cast<uint32_t>(idx));
        if (logBudget > 0) { --logBudget; Log(L"RESCAN DELETE %016I64x %s", e.frn, e.name.c_str()); }
        if (x.IsDir()) { --folderDelta_; RemoveTree(static_cast<uint32_t>(idx), true); } else --fileDelta_;
        ++st_.deletes;
    };
    for (const UsnEvent& e : ev) {
        ++st_.events;
        const uint32_t r = e.reason;
        const bool dir = (e.attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (r & USN_REASON_FILE_DELETE) {
            size_t idx = Find(vol, e.frn);
            if (idx == SIZE_MAX) {
                ++st_.notFound;
                if (dir) Log(L"unable to remove ntfs folder: %016I64x: file not found", e.frn);
                continue;
            }
            if ((work_ ? *work_ : *base_).entries[idx].parent == kNoParent) continue;   // 卷根
            removeOne(idx, e);
            continue;
        }
        if ((r & USN_REASON_RENAME_OLD_NAME) && !(r & USN_REASON_RENAME_NEW_NAME)) continue;   // 旧名记录：身份按 FRN，不需要处理
        size_t idx = Find(vol, e.frn);
        size_t pidx = Find(vol, e.parent);
        if (idx == SIZE_MAX) {
            // CREATE（或改名/硬链接到索引内的未知对象）
            if (pidx == SIZE_MAX || e.name.empty() || Filtered(vol, static_cast<uint32_t>(pidx), e.name, dir)) { ++st_.dropped; continue; }
            uint32_t ni = AddEntry(vol, e.name, static_cast<uint32_t>(pidx), dir, e.time, e.frn);
            need.push_back(ni);
            ++st_.creates;
            if (dir && (r & USN_REASON_RENAME_NEW_NAME)) ScanMovedIn(vol, ni);   // 从范围外移入：子项不会有 USN 记录
            continue;
        }
        const IndexData& d = *(work_ ? work_ : std::const_pointer_cast<IndexData>(base_));
        if (d.entries[idx].parent == kNoParent) continue;   // 卷根
        if (r & USN_REASON_RENAME_NEW_NAME) {
            if (pidx == SIZE_MAX || pidx == idx || Filtered(vol, static_cast<uint32_t>(pidx), e.name, dir)) { removeOne(idx, e); continue; }   // 移出索引范围
            EnsureWork();
            Entry& x = work_->entries.Mut(idx);
            std::wstring_view cur(work_->names.At(x.nameOff), x.nameLen);
            if (!e.name.empty() && cur != e.name) {
                MarkRemoved(static_cast<uint32_t>(idx));
                size_t len = std::min<size_t>(e.name.size(), 0xFFFF);
                x.nameOff = work_->names.Append(e.name.data(), len);
                x.nameLen = static_cast<uint16_t>(len);
                insert_.push_back(static_cast<uint32_t>(idx));
            }
            if (x.parent != pidx) { Unlink(static_cast<uint32_t>(idx), x.parent); Link(static_cast<uint32_t>(idx), static_cast<uint32_t>(pidx)); }
            x.parent = static_cast<uint32_t>(pidx);
            ++st_.renames;
            continue;
        }
        // MODIFY（含 USN_REASON_HARD_LINK_CHANGE：一个 FRN 一个对象，保留现有名称）
        ++st_.modifies;
        if (r & (USN_REASON_DATA_OVERWRITE | USN_REASON_DATA_EXTEND | USN_REASON_DATA_TRUNCATION)) {
            EnsureWork();
            work_->entries.Mut(idx).mtime = static_cast<uint64_t>(e.time);
            need.push_back(static_cast<uint32_t>(idx));
        } else if (r & USN_REASON_BASIC_INFO_CHANGE) need.push_back(static_cast<uint32_t>(idx));
    }
    if (lookup && !need.empty() && work_) {
        std::sort(need.begin(), need.end()); need.erase(std::unique(need.begin(), need.end()), need.end());
        need.erase(std::remove_if(need.begin(), need.end(), [&](uint32_t i) { return work_->entries[i].IsDeleted(); }), need.end());
        std::vector<uint64_t> frns; frns.reserve(need.size());
        for (uint32_t i : need) frns.push_back(work_->frn[i]);
        std::vector<LookupResult> res;
        lookup(vol, frns, res);
        st_.lookups += frns.size();
        for (size_t k = 0; k < need.size() && k < res.size(); ++k) {
            if (!res[k].sizeOk && !res[k].timeOk) continue;   // 全部失败：保留旧值
            Entry& x = work_->entries.Mut(need[k]);
            if (res[k].sizeOk && !x.IsDir()) x.size = res[k].size;
            if (res[k].timeOk && res[k].mtime) x.mtime = res[k].mtime;
        }
    }
    st_.applyMs += Ms(t);
}

std::shared_ptr<IndexData> LiveUpdater::Commit() {
    if (!work_) return nullptr;
    auto t = Clock::now();
    IndexData& wd = *work_;
    const IndexData& bd = *base_;
    // 2) byName：二分定位删除位置与插入位置，单次线性合并（不整体排序）
    const auto& B = bd.byName;
    auto lessB = [&](uint32_t a, uint32_t b) { int c = CompareFold(bd.Name(a), bd.Name(b)); return c ? c < 0 : a < b; };
    std::sort(removed_.begin(), removed_.end()); removed_.erase(std::unique(removed_.begin(), removed_.end()), removed_.end());
    std::vector<size_t> rpos; rpos.reserve(removed_.size());
    for (uint32_t id : removed_) {
        auto it = std::lower_bound(B.begin(), B.end(), id, lessB);
        if (it != B.end() && *it == id) rpos.push_back(static_cast<size_t>(it - B.begin()));
    }
    std::sort(rpos.begin(), rpos.end());
    std::sort(insert_.begin(), insert_.end()); insert_.erase(std::unique(insert_.begin(), insert_.end()), insert_.end());
    insert_.erase(std::remove_if(insert_.begin(), insert_.end(), [&](uint32_t i) { return wd.entries[i].IsDeleted(); }), insert_.end());
    std::sort(insert_.begin(), insert_.end(), [&](uint32_t a, uint32_t b) { int c = CompareFold(wd.Name(a), wd.Name(b)); return c ? c < 0 : a < b; });
    std::vector<uint32_t> out;
    out.reserve(B.size() - rpos.size() + insert_.size());
    size_t bi = 0, ri = 0;
    auto copyTo = [&](size_t to) {
        while (ri < rpos.size() && rpos[ri] < to) {
            out.insert(out.end(), B.begin() + bi, B.begin() + rpos[ri]);
            bi = rpos[ri] + 1; ++ri;
        }
        if (bi < to) { out.insert(out.end(), B.begin() + bi, B.begin() + to); bi = to; }
    };
    for (uint32_t ins : insert_) {
        auto it = std::lower_bound(B.begin() + bi, B.end(), ins, [&](uint32_t old, uint32_t x) {
            int c = CompareFold(bd.Name(old), wd.Name(x)); return c ? c < 0 : old < x; });
        copyTo(static_cast<size_t>(it - B.begin()));
        out.push_back(ins);
    }
    copyTo(B.size());
    st_.inserted += insert_.size(); st_.removed += rpos.size();
    wd.byName = std::move(out);
    wd.folders = static_cast<size_t>(static_cast<int64_t>(wd.folders) + folderDelta_);
    wd.files = static_cast<size_t>(static_cast<int64_t>(wd.files) + fileDelta_);
    st_.chunkClones += wd.entries.Clones() + wd.frn.Clones();
    auto res = work_;
    base_ = work_; work_.reset();
    removed_.clear(); insert_.clear(); folderDelta_ = fileDelta_ = 0;
    st_.commitMs += Ms(t);
    return res;
}

} // namespace ff
