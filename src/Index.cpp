#include "Index.h"
#include "App.h"
#include "Db.h"
#include "Fold.h"
#include "Log.h"
#include "MftIndexer.h"
#include "Settings.h"
#include "Util.h"
#include <algorithm>
#include <chrono>
#include <execution>

namespace ff {

using Clock = std::chrono::steady_clock;
static double Sec(Clock::time_point a) { return std::chrono::duration<double>(Clock::now() - a).count(); }

// ---------------- 路径 ----------------
size_t IndexData::ParentPathInto(uint32_t i, wchar_t* buf, size_t cap) const {
    if (cap == 0) return 0;
    uint32_t chain[512]; int n = 0;
    for (uint32_t p = entries[i].parent; p != kNoParent && n < 512; p = entries[p].parent) chain[n++] = p;
    size_t len = 0;
    for (int k = n - 1; k >= 0; --k) {
        auto nm = Name(chain[k]);
        size_t c = std::min(nm.size(), cap - 1 - len);
        wmemcpy(buf + len, nm.data(), c); len += c;
        if (k > 0 && len < cap - 1) buf[len++] = L'\\';
    }
    if (len == 2 && buf[1] == L':' && len < cap - 1) buf[len++] = L'\\';
    buf[len] = 0;
    return len;
}

size_t IndexData::FullPathInto(uint32_t i, wchar_t* buf, size_t cap) const {
    size_t len = ParentPathInto(i, buf, cap);
    if (len && buf[len - 1] != L'\\' && len < cap - 1) buf[len++] = L'\\';
    auto nm = Name(i);
    size_t c = std::min(nm.size(), cap - 1 - len);
    wmemcpy(buf + len, nm.data(), c); len += c;
    if (len == 2 && buf[1] == L':' && len < cap - 1) buf[len++] = L'\\';
    buf[len] = 0;
    return len;
}

std::wstring IndexData::ParentPath(uint32_t i) const { wchar_t b[32768]; size_t n = ParentPathInto(i, b, 32768); return {b, n}; }
std::wstring IndexData::FullPath(uint32_t i) const { wchar_t b[32768]; size_t n = FullPathInto(i, b, 32768); return {b, n}; }

// ---------------- 卷数据 ----------------
uint32_t VolumeData::Add(const wchar_t* s, size_t len, uint32_t parent, uint16_t flags, uint64_t size, uint64_t mtime) {
    Entry e;
    e.nameOff = static_cast<uint32_t>(names.size());
    e.nameLen = static_cast<uint16_t>(std::min<size_t>(len, 0xFFFF));
    e.parent = parent; e.flags = flags; e.size = size; e.mtime = mtime;
    names.insert(names.end(), s, s + e.nameLen);
    entries.push_back(e);
    return static_cast<uint32_t>(entries.size() - 1);
}

std::shared_ptr<IndexData> Merge(std::vector<VolumeData>& vols, uint64_t configHash) {
    auto d = std::make_shared<IndexData>();
    d->configHash = configHash;
    bool anyFrn = false;
    size_t total = 0, chars = 0;
    std::vector<std::vector<uint32_t>> maps(vols.size());
    for (size_t vi = 0; vi < vols.size(); ++vi) {
        auto& v = vols[vi];
        const size_t n = v.entries.size();
        // 存活：未删除，且父链存活（孤儿丢弃；父下标可能大于子下标，迭代到稳定）
        std::vector<uint8_t> alive(n);
        for (size_t i = 0; i < n; ++i) alive[i] = !v.entries[i].IsDeleted();
        for (int pass = 0; pass < 64; ++pass) {
            bool changed = false;
            for (size_t i = 1; i < n; ++i) {
                if (!alive[i]) continue;
                uint32_t p = v.entries[i].parent;
                if (p == kNoParent || p >= n || !alive[p]) { alive[i] = 0; changed = true; }
            }
            if (!changed) break;
        }
        auto& m = maps[vi];
        m.assign(n, kNoParent);
        for (size_t i = 0; i < n; ++i) if (alive[i]) { m[i] = static_cast<uint32_t>(total++); chars += v.entries[i].nameLen; }
        if (!v.frn.empty()) anyFrn = true;
    }
    d->entries.resize(total);
    d->names.resize(chars);
    if (anyFrn) d->frn.assign(total, 0);
    size_t nameOff = 0;
    for (size_t vi = 0; vi < vols.size(); ++vi) {
        auto& v = vols[vi]; auto& m = maps[vi];
        for (size_t i = 0; i < v.entries.size(); ++i) {
            if (m[i] == kNoParent) continue;
            const Entry& s = v.entries[i];
            Entry& e = d->entries[m[i]];
            e = s;
            e.flags = static_cast<uint16_t>((s.flags & kFlagDir) | (vi << 8));
            e.parent = s.parent == kNoParent ? kNoParent : m[s.parent];
            e.nameOff = static_cast<uint32_t>(nameOff);
            wmemcpy(d->names.data() + nameOff, v.names.data() + s.nameOff, s.nameLen);
            nameOff += s.nameLen;
            if (anyFrn && !v.frn.empty()) d->frn[m[i]] = v.frn[i];
            if (s.IsDir()) ++d->folders; else ++d->files;
        }
        d->volumes.push_back(v.info);
    }
    d->byName.resize(total);
    for (uint32_t i = 0; i < total; ++i) d->byName[i] = i;
    const IndexData* dp = d.get();
    std::sort(std::execution::par, d->byName.begin(), d->byName.end(), [dp](uint32_t a, uint32_t b) {
        int c = CompareFold(dp->Name(a), dp->Name(b));
        return c ? c < 0 : a < b;
    });
    return d;
}

VolumeData ExtractVolume(const IndexData& d, uint32_t vol) {
    VolumeData v;
    v.info = d.volumes[vol];
    std::vector<uint32_t> m(d.entries.size(), kNoParent);
    size_t cnt = 0;
    for (size_t i = 0; i < d.entries.size(); ++i)
        if (d.entries[i].Vol() == vol) m[i] = static_cast<uint32_t>(cnt++);
    v.entries.reserve(cnt);
    if (!d.frn.empty()) v.frn.reserve(cnt);
    for (size_t i = 0; i < d.entries.size(); ++i) {
        if (m[i] == kNoParent) continue;
        Entry e = d.entries[i];
        auto nm = d.Name(static_cast<uint32_t>(i));
        e.nameOff = static_cast<uint32_t>(v.names.size());
        v.names.insert(v.names.end(), nm.begin(), nm.end());
        e.parent = e.parent == kNoParent ? kNoParent : m[e.parent];
        e.flags &= kFlagDir;
        v.entries.push_back(e);
        if (!d.frn.empty()) v.frn.push_back(d.frn[i]);
    }
    return v;
}

uint64_t ConfigHash(const std::vector<std::wstring>& roots, const std::vector<std::wstring>& excludes) {
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&](const std::wstring& s) {
        auto l = ToLower(s);
        for (wchar_t c : l) { h ^= c; h *= 1099511628211ULL; }
        h ^= 0xFF; h *= 1099511628211ULL;
    };
    h ^= kDbVersion; h *= 1099511628211ULL;
    auto r = roots; std::sort(r.begin(), r.end()); for (auto& s : r) mix(s);
    h ^= 0xAB; h *= 1099511628211ULL;
    auto e = excludes; std::sort(e.begin(), e.end()); for (auto& s : e) mix(s);
    return h;
}

// ---------------- Indexer ----------------
std::vector<std::wstring> Indexer::DefaultRoots() {
    std::vector<std::wstring> r;
    DWORD mask = GetLogicalDrives();
    for (int d = 0; d < 26; ++d) {
        if (!(mask & (1u << d))) continue;
        wchar_t root[] = {static_cast<wchar_t>(L'A' + d), L':', L'\\', 0};
        if (GetDriveTypeW(root) == DRIVE_FIXED) r.push_back(std::wstring(root, 2));
    }
    return r;
}

std::wstring Indexer::DefaultDbPath() {
    auto p = GetSettings().IniPath();
    return p.substr(0, p.size() - 4) + L".db";
}

void Indexer::Start(HWND hwnd, Options opt) {
    Stop();
    stop_ = false;
    busy_ = true;
    hwnd_ = hwnd;
    thread_ = std::thread([this, hwnd, opt = std::move(opt)]() mutable { Run(hwnd, std::move(opt)); });
}

void Indexer::Stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    busy_ = false;
    phase_ = IndexPhase::Idle;
}

std::shared_ptr<const IndexData> Indexer::Snapshot() const {
    std::lock_guard lk(mu_);
    return ready_ ? data_ : nullptr;
}

std::wstring Indexer::CurrentFolder() const { std::lock_guard lk(mu_); return current_; }

void Indexer::Publish(HWND hwnd, std::shared_ptr<IndexData> d) {
    {
        std::lock_guard lk(mu_);
        data_ = std::move(d);
        ready_ = true;          // 与快照在同一把锁下发布
        current_.clear();
    }
    if (hwnd) PostMessageW(hwnd, WM_APP_INDEX_DONE, 0, 0);
}

void Indexer::Traverse(VolumeData& v, const std::vector<std::wstring>& excludes) {
    struct Pending { uint32_t idx; std::wstring path; };
    std::vector<Pending> stack;
    v.entries.clear(); v.names.clear(); v.frn.clear();
    v.entries.reserve(1 << 18); v.names.reserve(1 << 22);
    v.info.mft = false; v.info.journalId = 0; v.info.nextUsn = 0;
    v.Add(v.info.root.c_str(), v.info.root.size(), kNoParent, kFlagDir, 0, 0);
    stack.push_back({0, v.info.root});
    DWORD lastPost = GetTickCount();
    while (!stack.empty() && !stop_) {
        Pending cur = std::move(stack.back());
        stack.pop_back();
        std::wstring pattern = cur.path + L"\\*";
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            const wchar_t* n = fd.cFileName;
            if (n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0))) continue;
            bool dir = fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
            uint64_t size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            uint64_t mt = (static_cast<uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
            uint32_t idx = v.Add(n, wcslen(n), cur.idx, dir ? kFlagDir : 0, dir ? 0 : size, mt);
            if (dir && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                std::wstring child = cur.path + L"\\" + n;
                bool ex = false;
                for (auto& e : excludes) if (CompareStringOrdinal(child.c_str(), -1, e.c_str(), -1, TRUE) == CSTR_EQUAL) { ex = true; break; }
                if (!ex) stack.push_back({idx, std::move(child)});
            }
        } while (FindNextFileW(h, &fd) && !stop_);
        FindClose(h);
        ++progress_;  // 近似：已扫描文件夹数
        if (GetTickCount() - lastPost > 200) {
            lastPost = GetTickCount();
            { std::lock_guard lk(mu_); current_ = cur.path; }
            if (hwnd_) PostMessageW(hwnd_, WM_APP_INDEX_PROGRESS, 0, 0);
        }
    }
}

// 排除目录：标记其整棵子树为已删除（MFT 路径用；遍历路径直接跳过）
static void ApplyExcludes(VolumeData& v, const std::vector<std::wstring>& excludes) {
    if (excludes.empty() || v.entries.empty()) return;
    std::vector<uint32_t> exIdx;
    for (auto& ex : excludes) {
        if (ex.size() < 3 || CompareStringOrdinal(ex.c_str(), 2, v.info.root.c_str(), 2, TRUE) != CSTR_EQUAL) continue;
        uint32_t cur = 0; size_t pos = 3;
        while (pos <= ex.size() && cur != kNoParent) {
            size_t e = ex.find(L'\\', pos); if (e == std::wstring::npos) e = ex.size();
            std::wstring_view seg(ex.data() + pos, e - pos);
            uint32_t found = kNoParent;
            for (uint32_t i = 1; i < v.entries.size(); ++i) {
                auto& en = v.entries[i];
                if (en.parent == cur && en.nameLen == seg.size() &&
                    CompareStringOrdinal(v.names.data() + en.nameOff, en.nameLen, seg.data(), static_cast<int>(seg.size()), TRUE) == CSTR_EQUAL) { found = i; break; }
            }
            cur = found; pos = e + 1;
        }
        if (cur != kNoParent && cur != 0) exIdx.push_back(cur);
    }
    if (exIdx.empty()) return;
    // 只保留排除目录本身（与遍历一致：目录项可见，内容不索引）
    for (uint32_t i = 1; i < v.entries.size(); ++i) {
        for (uint32_t p = v.entries[i].parent, depth = 0; p != kNoParent && depth < 512; p = v.entries[p].parent, ++depth)
            if (std::find(exIdx.begin(), exIdx.end(), p) != exIdx.end()) { v.entries[i].flags |= kFlagDeleted; break; }
    }
}

bool Indexer::BuildVolume(VolumeData& v, const std::vector<std::wstring>& excludes, std::wstring& note) {
    uint32_t serial = 0;
    v.info.ntfs = MftIndexer::IsNtfs(v.info.root, &serial);
    v.info.serial = serial;
    if (v.info.ntfs && MftIndexer::IsElevated()) {
        std::wstring err;
        auto t = Clock::now();
        if (MftIndexer::Build(v, stop_, progress_, err)) {
            ApplyExcludes(v, excludes);
            Log(L"%s: MFT 枚举 %zu 个对象，用时 %.2f 秒", v.info.root.c_str(), v.entries.size(), Sec(t));
            return true;
        }
        note += v.info.root + L" MFT 失败(" + err + L")，回退遍历；";
    } else if (v.info.ntfs) {
        note += v.info.root + L" 未提升权限，使用文件夹遍历；";
    }
    auto t = Clock::now();
    Traverse(v, excludes);
    Log(L"%s: 文件夹遍历 %zu 个对象，用时 %.2f 秒", v.info.root.c_str(), v.entries.size(), Sec(t));
    return !stop_;
}

void Indexer::Run(HWND hwnd, Options opt) {
    InitFold();
    for (auto& r : opt.roots) while (!r.empty() && r.back() == L'\\') r.pop_back();
    const uint64_t cfg = ConfigHash(opt.roots, opt.excludes);
    IndexStats st;
    progress_ = 0;

    // 1) 加载数据库
    std::shared_ptr<IndexData> loaded;
    std::wstring err;
    if (!opt.forceRebuild && !opt.dbPath.empty()) {
        phase_ = IndexPhase::Loading;
        if (hwnd) PostMessageW(hwnd, WM_APP_INDEX_PROGRESS, 0, 0);
        auto t = Clock::now();
        loaded = LoadDb(opt.dbPath, err);
        if (loaded && loaded->configHash != cfg) { err = L"索引卷或排除设置已变化"; loaded.reset(); }
        if (loaded) {
            st.loadSec = Sec(t);
            st.loadedFromDb = true;
            Log(L"loaded %zu folders, %zu files, in %f seconds", loaded->folders, loaded->files, st.loadSec);
            { std::lock_guard lk(mu_); stats_ = st; }
            Publish(hwnd, loaded);
        } else {
            Log(L"数据库不可用（%s），重建", err.c_str());
        }
    }
    if (stop_) { busy_ = false; phase_ = IndexPhase::Idle; return; }

    std::vector<VolumeData> vols;
    bool changed = false;
    std::wstring note;

    if (loaded) {
        if (!opt.updateAfterLoad) { busy_ = false; phase_ = IndexPhase::Idle; return; }
        // 2) 只更新过期卷
        phase_ = IndexPhase::Updating;
        auto t = Clock::now();
        bool elevated = MftIndexer::IsElevated();
        for (uint32_t vi = 0; vi < loaded->volumes.size() && !stop_; ++vi) {
            const auto& info = loaded->volumes[vi];
            uint32_t serial = 0; MftIndexer::IsNtfs(info.root, &serial);
            bool stale = serial != info.serial;
            VolumeData v;
            if (!stale && info.mft && elevated) {
                v = ExtractVolume(*loaded, vi);
                size_t n = 0; std::wstring uerr;
                if (MftIndexer::ApplyUsn(v, stop_, &n, uerr)) {
                    Log(L"%s: USN 增量 %zu 条变化", info.root.c_str(), n);
                    if (n) changed = true;
                } else {
                    Log(L"%s: USN 不可用（%s），标记过期", info.root.c_str(), uerr.c_str());
                    stale = true;
                }
            } else if (!stale) {
                // 无法验证（未提升权限 / 非 MFT 建立）：视为过期
                stale = true;
                note += info.root + (elevated ? L" 非 MFT 建立，重扫；" : L" 未提升权限无法读取 USN，重扫；");
            }
            if (stale) {
                v = VolumeData{};
                v.info.root = info.root;
                if (!BuildVolume(v, opt.excludes, note)) break;
                changed = true;
            }
            vols.push_back(std::move(v));
        }
        st.updateSec = Sec(t);
        if (!changed && !stop_) {
            // 仍需保存新的 nextUsn
            if (!vols.empty()) changed = true;
        }
    } else {
        // 3) 重建
        phase_ = IndexPhase::Creating;
        if (hwnd) PostMessageW(hwnd, WM_APP_INDEX_PROGRESS, 0, 0);
        auto t = Clock::now();
        for (auto& r : opt.roots) {
            if (stop_) break;
            VolumeData v; v.info.root = r;
            BuildVolume(v, opt.excludes, note);
            vols.push_back(std::move(v));
        }
        st.rebuildSec = Sec(t);
        changed = true;
    }
    if (stop_) { busy_ = false; phase_ = IndexPhase::Idle; return; }

    if (changed) {
        auto t = Clock::now();
        auto d = Merge(vols, cfg);
        vols.clear(); vols.shrink_to_fit();
        st.sortSec = Sec(t);
        Log(L"合并排序 %zu 个对象，用时 %.2f 秒", d->entries.size(), st.sortSec);
        Publish(hwnd, d);
        phase_ = IndexPhase::Saving;
        t = Clock::now();
        if (!opt.dbPath.empty() && SaveDb(opt.dbPath, *d, opt.compress, &st.dbBytes)) {
            st.saveSec = Sec(t);
            Log(L"保存数据库 %llu 字节，用时 %.2f 秒", st.dbBytes, st.saveSec);
        }
    }
    st.note = note;
    if (!note.empty()) Log(L"%s", note.c_str());
    { std::lock_guard lk(mu_); stats_ = st; }
    busy_ = false;
    phase_ = IndexPhase::Idle;
    if (hwnd) PostMessageW(hwnd, WM_APP_INDEX_PROGRESS, 0, 0);
}

} // namespace ff
