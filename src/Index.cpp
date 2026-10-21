#include "Index.h"
#include "App.h"
#include "Db.h"
#include "Fold.h"
#include "Log.h"
#include "MftIndexer.h"
#include "NtfsSource.h"
#include "Settings.h"
#include "Usn.h"
#include "Util.h"
#include <algorithm>
#include <chrono>
#include <execution>
#include <unordered_map>

namespace ff {

using Clock = std::chrono::steady_clock;
static double Sec(Clock::time_point a) { return std::chrono::duration<double>(Clock::now() - a).count(); }

// ---------------- 名称池 ----------------
uint32_t NamePool::Append(const wchar_t* s, size_t len) {
    size_t inChunk = used_ & kMask;
    if (ch_.empty() || (used_ >> kShift) >= ch_.size() || inChunk + len > kChunk) {
        size_t oldChunks = ch_.size();
        if (!ch_.empty() && (used_ >> kShift) < ch_.size()) used_ = (used_ | kMask) + 1;   // 块尾填充，名称不跨块
        if ((used_ >> kShift) >= ch_.size()) {
            AddChunk();
            if (oldChunks && logGrowth_) Log(L"name array %zu/%zu o %zu n %zu", used_, ch_.size() * kChunk, oldChunks, ch_.size());
        }
    }
    uint32_t off = static_cast<uint32_t>(used_);
    wmemcpy(p_[used_ >> kShift] + (used_ & kMask), s, len);
    used_ += len;
    return off;
}

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

static std::wstring_view TrimSlash(std::wstring_view s) { while (!s.empty() && s.back() == L'\\') s.remove_suffix(1); return s; }

bool PathUnder(std::wstring_view path, std::wstring_view dir) {
    path = TrimSlash(path); dir = TrimSlash(dir);
    if (dir.empty() || path.size() < dir.size()) return false;
    if (CompareStringOrdinal(path.data(), static_cast<int>(dir.size()), dir.data(), static_cast<int>(dir.size()), TRUE) != CSTR_EQUAL) return false;
    return path.size() == dir.size() || path[dir.size()] == L'\\';
}

bool PathFiltered(std::wstring_view path, bool dir, const std::vector<std::wstring>& excludes, const std::vector<std::wstring>& includeOnly) {
    path = TrimSlash(path);
    for (auto& x : excludes) {
        auto xs = TrimSlash(x);
        if (path.size() > xs.size() && PathUnder(path, xs)) return true;   // 排除目录本身可见，其内容不索引
    }
    if (includeOnly.empty()) return false;
    for (auto& inc : includeOnly) {
        if (PathUnder(path, inc)) return false;
        if (dir && PathUnder(inc, path)) return false;   // 仅包含目录的祖先
    }
    return true;
}

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

// 建立时使用：合并各卷并整体并行排序（增量更新走 LiveUpdater，不调用这里）
std::shared_ptr<IndexData> Merge(std::vector<VolumeData>& vols, uint64_t configHash) {
    auto d = std::make_shared<IndexData>();
    d->configHash = configHash;
    bool anyFrn = false;
    size_t total = 0;
    std::vector<std::vector<uint32_t>> maps(vols.size());
    for (size_t vi = 0; vi < vols.size(); ++vi) {
        auto& v = vols[vi];
        const size_t n = v.entries.size();
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
        for (size_t i = 0; i < n; ++i) if (alive[i]) m[i] = static_cast<uint32_t>(total++);
        if (!v.frn.empty()) anyFrn = true;
    }
    d->entries.resize(total);
    if (anyFrn) d->frn.resize(total, 0);
    for (size_t vi = 0; vi < vols.size(); ++vi) {
        auto& v = vols[vi]; auto& m = maps[vi];
        for (size_t i = 0; i < v.entries.size(); ++i) {
            if (m[i] == kNoParent) continue;
            const Entry& s = v.entries[i];
            Entry& e = d->entries.Mut(m[i]);
            e = s;
            e.flags = static_cast<uint16_t>((s.flags & kFlagDir) | (vi << 8));
            e.parent = s.parent == kNoParent ? kNoParent : m[s.parent];
            e.nameOff = d->names.Append(v.names.data() + s.nameOff, s.nameLen);
            if (anyFrn && !v.frn.empty()) d->frn.Mut(m[i]) = v.frn[i];
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

std::shared_ptr<IndexData> ClearGhosts(const IndexData& d, size_t* cleared) {
    auto t = Clock::now();
    const size_t n = d.entries.size();
    std::vector<uint32_t> map(n, kNoParent);
    // 存活 = 未删除且父链存活（子树删除已保证这一点；这里再防御一次）
    std::vector<uint8_t> alive(n);
    for (size_t i = 0; i < n; ++i) alive[i] = !d.entries[i].IsDeleted();
    for (int pass = 0; pass < 64; ++pass) {
        bool ch = false;
        for (size_t i = 0; i < n; ++i) {
            if (!alive[i]) continue;
            uint32_t p = d.entries[i].parent;
            if (p != kNoParent && (p >= n || !alive[p])) { alive[i] = 0; ch = true; }
        }
        if (!ch) break;
    }
    uint32_t m = 0;
    for (size_t i = 0; i < n; ++i) if (alive[i]) map[i] = m++;
    auto o = std::make_shared<IndexData>();
    o->volumes = d.volumes; o->configHash = d.configHash; o->dbVersion = d.dbVersion;
    o->entries.resize(m);
    const bool hasFrn = d.frn.size() == n;
    if (hasFrn) o->frn.resize(m);
    for (size_t i = 0; i < n; ++i) {
        if (map[i] == kNoParent) continue;
        Entry e = d.entries[i];
        e.parent = e.parent == kNoParent ? kNoParent : map[e.parent];
        e.nameOff = o->names.Append(d.names.At(d.entries[i].nameOff), e.nameLen);
        o->entries.Mut(map[i]) = e;
        if (hasFrn) o->frn.Mut(map[i]) = d.frn[i];
        if (e.IsDir()) ++o->folders; else ++o->files;
    }
    // byName：下标单调重映射，顺序（名称，下标）保持不变，无需重新排序
    o->byName.reserve(d.byName.size());
    for (uint32_t id : d.byName) if (map[id] != kNoParent) o->byName.push_back(map[id]);
    if (cleared) *cleared = n - m;
    Log(L"deleteme: cleared %zu ghosts of %zu entries in %f seconds", n - m, n, Sec(t));
    return o;
}

static size_t Ghosts(const IndexData& d) { return d.entries.size() - d.folders - d.files; }

VolumeData ExtractVolume(const IndexData& d, uint32_t vol) {
    VolumeData v;
    v.info = d.volumes[vol];
    std::vector<uint32_t> m(d.entries.size(), kNoParent);
    size_t cnt = 0;
    for (size_t i = 0; i < d.entries.size(); ++i)
        if (d.entries[i].Vol() == vol && !d.entries[i].IsDeleted()) m[i] = static_cast<uint32_t>(cnt++);
    v.entries.reserve(cnt);
    const bool hasFrn = d.frn.size() == d.entries.size();
    if (hasFrn) v.frn.reserve(cnt);
    for (size_t i = 0; i < d.entries.size(); ++i) {
        if (m[i] == kNoParent) continue;
        Entry e = d.entries[i];
        auto nm = d.Name(static_cast<uint32_t>(i));
        e.nameOff = static_cast<uint32_t>(v.names.size());
        v.names.insert(v.names.end(), nm.begin(), nm.end());
        e.parent = e.parent == kNoParent ? kNoParent : m[e.parent];
        e.flags &= kFlagDir;
        v.entries.push_back(e);
        if (hasFrn) v.frn.push_back(d.frn[i]);
    }
    return v;
}

// 只有排除列表（含各卷仅包含列表）变化才导致重建；卷的增减按卷处理
uint64_t ConfigHash(const std::vector<std::wstring>& excludes, const std::vector<VolumeConfig>& vols) {
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&](const std::wstring& s) {
        auto l = ToLower(TrimSlash(s));
        for (wchar_t c : l) { h ^= c; h *= 1099511628211ULL; }
        h ^= 0xFF; h *= 1099511628211ULL;
    };
    auto e = excludes; std::sort(e.begin(), e.end()); for (auto& s : e) mix(s);
    std::vector<std::wstring> inc;
    for (auto& v : vols) if (v.include) for (auto& s : v.includeOnly) inc.push_back(s);
    std::sort(inc.begin(), inc.end());
    h ^= 0xAB; h *= 1099511628211ULL;
    for (auto& s : inc) mix(s);
    return h;
}

const VolumeConfig* Indexer::Options::Vol(const std::wstring& root) const {
    for (auto& v : volumes) if (CompareStringOrdinal(TrimSlash(v.path).data(), static_cast<int>(TrimSlash(v.path).size()), root.c_str(), static_cast<int>(root.size()), TRUE) == CSTR_EQUAL) return &v;
    return nullptr;
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
        ready_ = true;
        current_.clear();
    }
    if (hwnd) PostMessageW(hwnd, WM_APP_INDEX_DONE, 0, 0);
}

static std::vector<std::wstring> ExcludesOn(const std::vector<std::wstring>& ex, const std::wstring& root) {
    std::vector<std::wstring> r;
    for (auto& e : ex) if (PathUnder(e, root)) r.push_back(e);
    return r;
}

// 建立后的过滤：排除目录的内容、以及仅包含列表之外的对象标记为已删除
static void ApplyFilters(VolumeData& v, const std::vector<std::wstring>& excludes, const std::vector<std::wstring>& includeOnly) {
    if ((excludes.empty() && includeOnly.empty()) || v.entries.empty()) return;
    const size_t n = v.entries.size();
    // 按名称逐级解析目录路径（子表：父 -> 子；只在需要时建立）
    std::unordered_multimap<uint32_t, uint32_t> kids;
    kids.reserve(n);
    for (uint32_t i = 1; i < n; ++i) if (v.entries[i].IsDir() && !v.entries[i].IsDeleted()) kids.emplace(v.entries[i].parent, i);
    auto resolve = [&](const std::wstring& path) -> uint32_t {
        auto p = TrimSlash(path);
        if (p.size() < 2 || CompareStringOrdinal(p.data(), 2, v.info.root.c_str(), 2, TRUE) != CSTR_EQUAL) return kNoParent;
        uint32_t cur = 0; size_t pos = 3;
        while (pos <= p.size() && pos > 2 && cur != kNoParent) {
            if (pos >= p.size()) break;
            size_t e = p.find(L'\\', pos); if (e == std::wstring_view::npos) e = p.size();
            std::wstring_view seg = p.substr(pos, e - pos);
            uint32_t found = kNoParent;
            auto [a, b] = kids.equal_range(cur);
            for (auto it = a; it != b; ++it) {
                auto& en = v.entries[it->second];
                if (en.nameLen == seg.size() && CompareStringOrdinal(v.names.data() + en.nameOff, en.nameLen, seg.data(), static_cast<int>(seg.size()), TRUE) == CSTR_EQUAL) { found = it->second; break; }
            }
            cur = found; pos = e + 1;
        }
        return cur;
    };
    std::vector<uint8_t> mark(n, 0);   // 1 = 排除目录，2 = 仅包含目录，4 = 仅包含目录的祖先
    for (auto& ex : excludes) { uint32_t i = resolve(ex); if (i != kNoParent && i != 0) mark[i] |= 1; }
    bool incl = false;
    for (auto& in : includeOnly) {
        uint32_t i = resolve(in); if (i == kNoParent) continue;
        incl = true; mark[i] |= 2;
        for (uint32_t p = v.entries[i].parent; p != kNoParent; p = v.entries[p].parent) mark[p] |= 4;
    }
    if (!includeOnly.empty() && !incl) {   // 仅包含目录都不存在：只留卷根
        for (uint32_t i = 1; i < n; ++i) v.entries[i].flags |= kFlagDeleted;
        return;
    }
    for (uint32_t i = 1; i < n; ++i) {
        bool underInc = false, underEx = false;
        for (uint32_t p = v.entries[i].parent, depth = 0; p != kNoParent && depth < 512; p = v.entries[p].parent, ++depth) {
            if (mark[p] & 1) { underEx = true; break; }
            if (mark[p] & 2) underInc = true;
        }
        bool keep = !underEx && (includeOnly.empty() || underInc || (mark[i] & 6));
        if (!keep) v.entries[i].flags |= kFlagDeleted;
    }
}

bool Indexer::BuildVolume(VolumeData& v, const Options& opt, std::wstring& note) {
    uint32_t serial = 0;
    v.info.ntfs = MftIndexer::IsNtfs(v.info.root, &serial);
    v.info.serial = serial;
    auto t = Clock::now();
    const VolumeConfig* vc = opt.Vol(v.info.root);
    auto ex = ExcludesOn(opt.excludes, v.info.root);
    if (!v.info.ntfs) {   // 非 NTFS：文件夹索引（重扫计划由文件夹设置决定，默认从不）
        MftIndexer::EnumerateWithIds(v, ex, stop_, progress_);
        v.info.mft = false; v.info.journalId = 0;
        Log(L"%s: 文件夹索引 %zu 个对象，用时 %.2f 秒", v.info.root.c_str(), v.entries.size(), Sec(t));
        return !stop_;
    }
    if (!src_) {
        if (adminVols_.find(v.info.root) == std::wstring::npos) adminVols_ += (adminVols_.empty() ? L"" : L" ") + v.info.root;
        Log(L"%s: 无法读取 MFT（没有 FlashFind 服务且未提升权限），卷需要重建", v.info.root.c_str());
        return false;
    }
    std::wstring err;
    Log(L"%s: 通过%s读取 MFT", v.info.root.c_str(), src_->Kind());
    if (src_->Build(v, vc ? vc->enableUsnJournal : true, stop_, progress_, err)) {
        ApplyFilters(v, ex, vc ? vc->includeOnly : std::vector<std::wstring>{});
        Log(L"%s: MFT 建立 %zu 个对象，用时 %.2f 秒", v.info.root.c_str(), v.entries.size(), Sec(t));
        return true;
    }
    note += v.info.root + L" " + err + L"；";
    Log(L"%s: MFT 建立失败：%s", v.info.root.c_str(), err.c_str());
    return false;
}

static bool JournalLost(DWORD e) {
    return e == ERROR_JOURNAL_ENTRY_DELETED || e == ERROR_JOURNAL_NOT_ACTIVE || e == ERROR_JOURNAL_DELETE_IN_PROGRESS;
}

void Indexer::Run(HWND hwnd, Options opt) {
    InitFold();
    for (auto& r : opt.roots) while (!r.empty() && r.back() == L'\\') r.pop_back();
    const uint64_t cfg = ConfigHash(opt.excludes, opt.volumes);
    // 特权来源：已提升 -> 本进程；否则连接 FlashFind 服务；都没有 -> 只加载旧数据并提示
    std::unique_ptr<NtfsSource> srcHolder;
    std::wstring why;
    if (MftIndexer::IsElevated()) srcHolder = MakeLocalSource();
    else srcHolder = ConnectService(opt.allowConsoleService, &why);
    if (!srcHolder && !why.empty()) Log(L"FlashFind 服务不可用：%s", why.c_str());
    src_ = srcHolder.get();
    struct ClearSrc { NtfsSource*& p; ~ClearSrc() { p = nullptr; } } clearSrc{src_};
    IndexStats st;
    progress_ = 0;
    auto setStats = [&] { std::lock_guard lk(mu_); stats_ = st; };
    auto finish = [&] { busy_ = false; phase_ = IndexPhase::Idle; if (hwnd) PostMessageW(hwnd, WM_APP_INDEX_PROGRESS, 0, 0); };
    { std::lock_guard lk(mu_); stats_ = {}; }
    adminVols_.clear();
    st.source = src_ ? src_->Kind() : L"";
    auto volCfg = [&](const std::wstring& root) { const VolumeConfig* c = opt.Vol(root); static const VolumeConfig def; return c ? *c : def; };
    auto rootOnly = [&](const std::wstring& r) {
        VolumeData v; v.info.root = r; uint32_t s = 0; v.info.ntfs = MftIndexer::IsNtfs(r, &s); v.info.serial = s;
        v.Add(r.c_str(), r.size(), kNoParent, kFlagDir, 0, 0); v.frn.push_back(0);
        return v;
    };

    // 1) 加载上次的数据库（启动时绝不全量重扫）
    std::shared_ptr<IndexData> cur;
    std::wstring err;
    if (!opt.forceRebuild && !opt.dbPath.empty()) {
        phase_ = IndexPhase::Loading;
        if (hwnd) PostMessageW(hwnd, WM_APP_INDEX_PROGRESS, 0, 0);
        auto t = Clock::now();
        cur = LoadDb(opt.dbPath, err);
        if (cur) {
            st.loadSec = Sec(t); st.loadedFromDb = true;
            Log(L"loaded %zu folders, %zu files, in %f seconds", cur->folders, cur->files, st.loadSec);
            setStats();
            Publish(hwnd, cur);
        } else {
            Log(L"数据库不可用（%s），需要重建", err.c_str());
        }
    }
    if (stop_) { finish(); return; }

    std::wstring note;
    bool recordsChanged = false, cursorMoved = false;
    std::vector<VolumeInfo> vs;
    LiveUpdater::Lookup lookup = [&](uint32_t vi, const std::vector<uint64_t>& frns, std::vector<LookupResult>& out) {
        if (src_) src_->Lookup(vs[vi].root, frns, out);
    };
    std::unique_ptr<LiveUpdater> upd;
    auto setFilters = [&] {
        for (uint32_t vi = 0; vi < vs.size(); ++vi) upd->SetFilter(vi, ExcludesOn(opt.excludes, vs[vi].root), volCfg(vs[vi].root).includeOnly);
        upd->SetScanner([&](uint32_t vi, const std::wstring& path, VolumeData& out) {
            std::wstring e2;
            return src_ && src_->ScanDir(vs[vi].root, path, ExcludesOn(opt.excludes, vs[vi].root), out, e2);
        });
    };
    // 读一卷的 USN（只用带名称的特权记录）；返回 0 或 Win32 错误
    auto readVol = [&](uint32_t vi, std::vector<UsnEvent>& ev, int64_t& next) -> DWORD {
        if (!src_) return ERROR_ACCESS_DENIED;
        std::wstring e2; std::vector<uint8_t> raw;
        next = vs[vi].nextUsn;
        DWORD e = src_->ReadUsn(vs[vi].root, vs[vi].journalId, next, raw, e2);
        if (e) return e;
        int budget = g_debugLog ? 1000 : 0;
        if (!ParseUsnRecords(raw.data(), raw.size(), ev, e2, budget)) { next = vs[vi].nextUsn; return ERROR_INVALID_DATA; }
        return 0;
    };
    auto closeVols = [] {};
    auto queryJournal = [&](const std::wstring& root, bool create, uint64_t& jid, int64_t& first, int64_t& next, std::wstring& e2) {
        return src_ ? src_->QueryJournal(root, create, jid, first, next, e2) : MftIndexer::QueryJournal(root, false, jid, first, next, e2);
    };

    if (cur) {
        vs = cur->volumes;
        if (!opt.updateAfterLoad) { finish(); return; }
        phase_ = IndexPhase::Updating;
        auto t = Clock::now();
        const bool excludesChanged = cur->configHash != cfg;
        if (excludesChanged) Log(L"rebuild: excludes changed");
        std::vector<uint8_t> stale(vs.size(), 0), keep(vs.size(), 0), needAdmin(vs.size(), 0);
        bool structural = false;
        for (uint32_t vi = 0; vi < vs.size(); ++vi) {
            keep[vi] = std::find_if(opt.roots.begin(), opt.roots.end(), [&](auto& r) { return CompareStringOrdinal(r.c_str(), -1, vs[vi].root.c_str(), -1, TRUE) == CSTR_EQUAL; }) != opt.roots.end();
            if (!keep[vi]) { structural = true; Log(L"%s: 已不在索引卷列表中，移除", vs[vi].root.c_str()); }
        }
        std::vector<std::wstring> added;
        for (auto& r : opt.roots)
            if (std::find_if(vs.begin(), vs.end(), [&](auto& v) { return CompareStringOrdinal(r.c_str(), -1, v.root.c_str(), -1, TRUE) == CSTR_EQUAL; }) == vs.end()) { added.push_back(r); structural = true; }
        upd = std::make_unique<LiveUpdater>(cur);
        setFilters();
        for (uint32_t vi = 0; vi < vs.size() && !stop_; ++vi) {
            auto& v = vs[vi];
            if (!keep[vi] || !v.ntfs) continue;
            if (excludesChanged) { stale[vi] = 1; continue; }
            if (!v.mft || v.journalId == 0) { Log(L"%s: Journal ID changed（未记录 -> 需要 MFT 重建），卷标记为过期，继续显示旧数据", v.root.c_str()); stale[vi] = 1; continue; }
            uint64_t jid = 0; int64_t first = 0, next = 0;
            if (!queryJournal(v.root, volCfg(v.root).enableUsnJournal, jid, first, next, err)) { Log(L"%s: %s，卷需要重建", v.root.c_str(), err.c_str()); stale[vi] = 1; continue; }
            if (jid != v.journalId) { Log(L"%s: Journal ID changed（%016I64X -> %016I64X），卷需要重建", v.root.c_str(), v.journalId, jid); stale[vi] = 1; continue; }
            if (v.nextUsn < first) { Log(L"%s: 保存的 USN %I64d 早于 Journal 起点 %I64d（已截断），卷需要重建", v.root.c_str(), v.nextUsn, first); stale[vi] = 1; continue; }
            if (next <= v.nextUsn) continue;
            if (!src_) {   // 非特权记录没有文件名：不用于修补；游标不动
                needAdmin[vi] = 1;
                adminVols_ += (adminVols_.empty() ? L"" : L" ") + v.root;
                Log(L"%s: 有 %I64d 字节的新 USN 记录，但只能读取无文件名的非特权记录，不用于修补；需要 FlashFind 服务或管理员", v.root.c_str(), next - v.nextUsn);
                continue;
            }
            std::vector<UsnEvent> ev; int64_t nx = 0;
            DWORD e = readVol(vi, ev, nx);
            if (e) {
                if (JournalLost(e)) { Log(L"%s: USN Journal 丢失/过期（%lu），整卷重建", v.root.c_str(), e); stale[vi] = 1; }
                else Log(L"ntfs monitor read error %lu retrying in 30 seconds", e);
                continue;
            }
            {
                LogBatch lb;
                auto tp = Clock::now();
                upd->Apply(vi, ev, lookup);
                if (!ev.empty()) Log(L"%s: processed ntfs usn records in %f seconds（%zu 条）", v.root.c_str(), Sec(tp), ev.size());
            }
            if (nx != v.nextUsn) { v.nextUsn = nx; cursorMoved = true; }
        }
        // 过期卷 / 新卷：只重建该卷（NTFS 用 MFT，需要管理员）；做不到就保留旧数据并提示
        std::vector<VolumeData> rebuilt;
        std::vector<int> rebuiltOf(vs.size(), -1);
        bool anyRebuilt = false;
        for (uint32_t vi = 0; vi < vs.size() && !stop_; ++vi) {
            if (!keep[vi] || !stale[vi]) continue;
            VolumeData v; v.info.root = vs[vi].root;
            phase_ = IndexPhase::Creating;
            if (BuildVolume(v, opt, note)) { rebuiltOf[vi] = static_cast<int>(rebuilt.size()); rebuilt.push_back(std::move(v)); anyRebuilt = true; }
        }
        std::vector<VolumeData> newVols;
        for (auto& r : added) {
            VolumeData v; v.info.root = r;
            if (!BuildVolume(v, opt, note)) v = rootOnly(r);
            newVols.push_back(std::move(v));
        }
        if (structural || anyRebuilt || excludesChanged) {
            auto snap = upd->Dirty() ? upd->Commit() : cur;
            std::vector<VolumeData> vols;
            for (uint32_t vi = 0; vi < vs.size(); ++vi) {
                if (!keep[vi]) continue;
                if (rebuiltOf[vi] >= 0) { vols.push_back(std::move(rebuilt[rebuiltOf[vi]])); continue; }
                VolumeData v = ExtractVolume(*snap, vi);
                v.info = vs[vi];
                if (excludesChanged) {   // 无法重建：旧数据按新的排除列表过滤；移除的排除项需要以后重建
                    ApplyFilters(v, ExcludesOn(opt.excludes, v.info.root), volCfg(v.info.root).includeOnly);
                    if (v.info.ntfs) { v.info.mft = false; v.info.journalId = 0; }
                }
                vols.push_back(std::move(v));
            }
            for (auto& v : newVols) vols.push_back(std::move(v));
            auto t2 = Clock::now();
            cur = Merge(vols, cfg); cur->dbVersion = kDbVersion;
            st.sortSec = Sec(t2);
            recordsChanged = true;
        } else if (upd->Dirty()) {
            cur = upd->Commit();
            cur->volumes = vs;
            auto& s2 = upd->LastStats();
            Log(L"rescan update db: %zu 条记录，新建 %zu 删除 %zu 改名 %zu 修改 %zu，插入 %zu 移除 %zu，用时 %.1f+%.1f ms",
                s2.events, s2.creates, s2.deletes, s2.renames, s2.modifies, s2.inserted, s2.removed, s2.applyMs, s2.commitMs);
            st.usnChanges = s2.events;
            recordsChanged = true;
        }
        if (!recordsChanged)
            for (uint32_t vi = 0; vi < vs.size(); ++vi)
                if (keep[vi] && vs[vi].ntfs && !stale[vi] && !needAdmin[vi]) Log(L"%s: using existing NTFS index", vs[vi].root.c_str());
        if (cur->dbVersion != kDbVersion) recordsChanged = true;   // 旧版本数据库升级需要整库保存一次
        st.updateSec = Sec(t);
        if (structural || anyRebuilt || excludesChanged) vs = cur->volumes;
    } else {
        // 2) 没有可用数据库：建立（NTFS 卷需要 MFT 权限；做不到则该卷只有根并提示）
        phase_ = IndexPhase::Creating;
        if (hwnd) PostMessageW(hwnd, WM_APP_INDEX_PROGRESS, 0, 0);
        auto t = Clock::now();
        std::vector<VolumeData> vols;
        for (auto& r : opt.roots) {
            if (stop_) break;
            VolumeData v; v.info.root = r;
            if (!BuildVolume(v, opt, note)) v = rootOnly(r);
            vols.push_back(std::move(v));
        }
        st.rebuildSec = Sec(t);
        if (stop_) { closeVols(); finish(); return; }
        auto t2 = Clock::now();
        cur = Merge(vols, cfg); cur->dbVersion = kDbVersion;
        st.sortSec = Sec(t2);
        vs = cur->volumes;
        recordsChanged = true;
    }
    if (!adminVols_.empty()) { note = adminVols_ + L": install FlashFind service or run as admin to index this volume" + (note.empty() ? L"" : L"；" + note); st.needService = true; }
    st.note = note;
    if (!note.empty()) Log(L"%s", note.c_str());

    auto save = [&](bool full) {
        auto t = Clock::now();
        phase_ = IndexPhase::Saving;
        if (full) {
            if (Ghosts(*cur)) {   // 保存前清除幽灵条目（不重读 MFT、不重新排序）
                auto c2 = ClearGhosts(*cur, &st.ghostsCleared);
                cur = c2;
                if (upd) upd->Rebase(cur);
                Publish(hwnd, cur);
            }
            cur->volumes = vs; cur->configHash = cfg; cur->dbVersion = kDbVersion;
            if (SaveDb(opt.dbPath, *cur, opt.compress, &st.dbBytes)) { st.saveSec = Sec(t); st.fullSave = true; Log(L"保存数据库 %llu 字节，用时 %.2f 秒", st.dbBytes, st.saveSec); }
        } else if (UpdateDbVolumes(opt.dbPath, vs)) {
            st.headerOnlyUpdate = true; st.saveSec = Sec(t);
            Log(L"无记录变化：只更新卷表（Journal ID + Next USN），用时 %.3f 秒", st.saveSec);
        }
    };
    if (recordsChanged) { cur->volumes = vs; Publish(hwnd, cur); }
    if (!opt.dbPath.empty()) { if (recordsChanged) save(true); else if (cursorMoved) save(false); }
    setStats();
    finish();

    // 3) 监视：每 monitor_update_delay 毫秒读取一次各卷 USN，逐条处理后批量增量合并；退出时保存
    std::vector<uint8_t> mon(vs.size(), 0);
    bool anyMon = false;
    for (uint32_t vi = 0; vi < vs.size(); ++vi) {
        mon[vi] = opt.monitor && src_ && vs[vi].ntfs && vs[vi].mft && vs[vi].journalId && volCfg(vs[vi].root).monitor;
        anyMon = anyMon || mon[vi];
    }
    if (!anyMon) { closeVols(); return; }
    upd = std::make_unique<LiveUpdater>(cur);
    setFilters();
    bool dbDirty = false; cursorMoved = false;
    std::vector<Clock::time_point> retryAt(vs.size(), Clock::now());
    while (!stop_) {
        for (int k = 0; k < std::max(1, opt.monitorDelayMs / 50) && !stop_; ++k) Sleep(50);
        if (stop_) break;
        bool rebuiltAny = false;
        LogBatch lb;   // 每批一次写日志
        if (src_ && !src_->Alive()) {   // 服务连接断开：重连
            std::wstring w2;
            srcHolder = MftIndexer::IsElevated() ? MakeLocalSource() : ConnectService(opt.allowConsoleService, &w2);
            src_ = srcHolder.get();
            if (!src_) { Log(L"ntfs monitor read error %lu retrying in 30 seconds", ERROR_BROKEN_PIPE); for (int k = 0; k < 600 && !stop_; ++k) Sleep(50); continue; }
        }
        if (!src_) break;
        auto tBatch = Clock::now(); size_t nEv = 0;
        for (uint32_t vi = 0; vi < vs.size() && !stop_; ++vi) {
            if (!mon[vi] || Clock::now() < retryAt[vi]) continue;
            std::vector<UsnEvent> ev; int64_t nx = 0;
            DWORD e = readVol(vi, ev, nx);
            if (e) {
                if (JournalLost(e)) {
                    Log(L"%s: USN Journal 丢失/过期（%lu），整卷重建", vs[vi].root.c_str(), e);
                    std::wstring n2;
                    VolumeData v; v.info.root = vs[vi].root;
                    if (BuildVolume(v, opt, n2)) {
                        auto snap = upd->Dirty() ? upd->Commit() : cur;
                        std::vector<VolumeData> vols;
                        for (uint32_t k = 0; k < vs.size(); ++k) {
                            if (k == vi) { vols.push_back(std::move(v)); continue; }
                            VolumeData x = ExtractVolume(*snap, k); x.info = vs[k]; vols.push_back(std::move(x));
                        }
                        cur = Merge(vols, cfg); cur->dbVersion = kDbVersion; vs = cur->volumes;
                        upd->Rebase(cur); setFilters();
                        rebuiltAny = true;
                    } else { mon[vi] = 0; }
                } else {
                    Log(L"ntfs monitor read error %lu retrying in 30 seconds", e);
                    retryAt[vi] = Clock::now() + std::chrono::seconds(30);
                }
                continue;
            }
            if (!ev.empty()) { upd->Apply(vi, ev, lookup); nEv += ev.size(); }
            if (nx != vs[vi].nextUsn) { vs[vi].nextUsn = nx; cursorMoved = true; }
        }
        if (nEv) Log(L"processed ntfs usn records in %f seconds（%zu 条）", Sec(tBatch), nEv);
        if (upd->Dirty()) {
            cur = upd->Commit();
            cur->volumes = vs;
            Publish(hwnd, cur);
            dbDirty = true;
            auto& s2 = upd->LastStats();
            Log(L"rescan update db: %zu 条记录，新建 %zu 删除 %zu 改名 %zu 修改 %zu，插入 %zu 移除 %zu，用时 %.1f+%.1f ms",
                s2.events, s2.creates, s2.deletes, s2.renames, s2.modifies, s2.inserted, s2.removed, s2.applyMs, s2.commitMs);
            upd->ResetStats();
            if (Ghosts(*cur) * 10 > cur->entries.size()) {   // 幽灵超过 10%：压缩
                cur = ClearGhosts(*cur, nullptr);
                cur->volumes = vs;
                upd->Rebase(cur); setFilters();
                Publish(hwnd, cur);
            }
        } else if (rebuiltAny) { cur->volumes = vs; Publish(hwnd, cur); dbDirty = true; }
    }
    closeVols();
    if (!opt.dbPath.empty()) { if (dbDirty) save(true); else if (cursorMoved) save(false); }
}

} // namespace ff
