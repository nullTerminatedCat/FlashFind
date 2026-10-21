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
#include <unordered_map>

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

uint64_t ConfigHash(const std::vector<std::wstring>& roots, const std::vector<std::wstring>& excludes, uint32_t version) {
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&](const std::wstring& s) {
        auto l = ToLower(s);
        for (wchar_t c : l) { h ^= c; h *= 1099511628211ULL; }
        h ^= 0xFF; h *= 1099511628211ULL;
    };
    h ^= version; h *= 1099511628211ULL;
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

bool Indexer::BuildVolume(VolumeData& v, const Options& opt, std::wstring& note) {
    uint32_t serial = 0;
    v.info.ntfs = MftIndexer::IsNtfs(v.info.root, &serial);
    v.info.serial = serial;
    auto t = Clock::now();
    if (!v.info.ntfs) {   // 非 NTFS：文件夹索引（之后按其重扫计划，默认从不）
        MftIndexer::EnumerateWithIds(v, opt.excludes, stop_, progress_);
        v.info.mft = false; v.info.journalId = 0;
        Log(L"%s: 文件夹索引 %zu 个对象，用时 %.2f 秒", v.info.root.c_str(), v.entries.size(), Sec(t));
        return !stop_;
    }
    if (std::find(opt.seedTraverse.begin(), opt.seedTraverse.end(), v.info.root) != opt.seedTraverse.end()) {
        // 仅自检：中完整性下用带 FileId 的枚举建立，游标取枚举开始前的 NextUsn，之后由 USN 重放
        std::wstring err; uint64_t jid = 0; int64_t first = 0, next = 0;
        if (!MftIndexer::QueryJournal(v.info.root, false, jid, first, next, err)) { note += v.info.root + L" " + err + L"；"; return false; }
        MftIndexer::EnumerateWithIds(v, opt.excludes, stop_, progress_);
        v.info.mft = true; v.info.journalId = jid; v.info.nextUsn = next;
        Log(L"%s: [自检] FileId 枚举 %zu 个对象，Journal %016I64X 游标 %I64d", v.info.root.c_str(), v.entries.size(), jid, next);
        return !stop_;
    }
    if (!MftIndexer::IsElevated()) {
        adminVols_ += (adminVols_.empty() ? L"" : L" ") + v.info.root;
        Log(L"%s: 无法读取 MFT（未提升权限），卷需要重建", v.info.root.c_str());
        return false;
    }
    std::wstring err;
    if (MftIndexer::Build(v, opt.enableUsnJournal, stop_, progress_, err)) {
        ApplyExcludes(v, opt.excludes);
        Log(L"%s: MFT 建立 %zu 个对象，用时 %.2f 秒", v.info.root.c_str(), v.entries.size(), Sec(t));
        return true;
    }
    note += v.info.root + L" " + err + L"；";
    Log(L"%s: MFT 建立失败：%s", v.info.root.c_str(), err.c_str());
    return false;
}

bool Indexer::PollUsn(std::shared_ptr<IndexData>& cur, const Options& opt, bool& cursorMoved, std::vector<uint32_t>* staleVols) {
    std::vector<VolumeData> modified(volState_.size());
    std::vector<uint8_t> isMod(volState_.size(), 0);
    bool any = false;
    for (uint32_t vi = 0; vi < volState_.size() && !stop_; ++vi) {
        auto& vs = volState_[vi];
        if (!vs.ntfs || stale_[vi]) continue;
        if (!vs.mft || vs.journalId == 0) {   // v2 数据库或文件夹枚举建立：没有记录 Journal
            Log(L"%s: Journal ID changed（未记录 -> 需要 MFT 重建），卷标记为过期，继续显示旧数据", vs.root.c_str());
            stale_[vi] = 1; if (staleVols) staleVols->push_back(vi); continue;
        }
        uint64_t jid = 0; int64_t first = 0, next = 0; std::wstring err;
        if (!MftIndexer::QueryJournal(vs.root, opt.enableUsnJournal, jid, first, next, err)) {
            Log(L"%s: %s，卷需要重建", vs.root.c_str(), err.c_str());
            stale_[vi] = 1; if (staleVols) staleVols->push_back(vi); continue;
        }
        if (jid != vs.journalId) {
            Log(L"%s: Journal ID changed（%016I64X -> %016I64X），卷需要重建", vs.root.c_str(), vs.journalId, jid);
            stale_[vi] = 1; if (staleVols) staleVols->push_back(vi); continue;
        }
        if (vs.nextUsn < first) {
            Log(L"%s: 保存的 USN %I64d 早于 Journal 起点 %I64d（已截断），卷需要重建", vs.root.c_str(), vs.nextUsn, first);
            stale_[vi] = 1; if (staleVols) staleVols->push_back(vi); continue;
        }
        if (next <= vs.nextUsn) continue;
        std::unordered_map<uint64_t, UsnChange> ch;
        int64_t newNext = vs.nextUsn;
        if (!MftIndexer::ReadChanges(vs.root, vs.journalId, vs.nextUsn, newNext, ch, err)) {
            Log(L"%s: 读取 USN 失败：%s，卷需要重建", vs.root.c_str(), err.c_str());
            stale_[vi] = 1; if (staleVols) staleVols->push_back(vi); continue;
        }
        cursorMoved = cursorMoved || newNext != vs.nextUsn;
        if (!ch.empty()) {
            VolumeData v = ExtractVolume(*cur, vi);
            size_t unresolved = 0;
            size_t n = MftIndexer::Apply(v, ch, &unresolved);
            v.info = vs; v.info.nextUsn = newNext;
            {
                std::lock_guard lk(mu_);
                stats_.usnChanges += n; stats_.usnUnresolved += unresolved;
            }
            if (n) {
                Log(L"%s: USN %I64d -> %I64d，%zu 条记录，应用 %zu 项变化（%zu 项新建无法取得名称）", vs.root.c_str(), vs.nextUsn, newNext, ch.size(), n, unresolved);
                modified[vi] = std::move(v); isMod[vi] = 1; any = true;
            }
        }
        vs.nextUsn = newNext;
    }
    if (!any) return false;
    std::vector<VolumeData> vols;
    for (uint32_t vi = 0; vi < volState_.size(); ++vi) {
        if (isMod[vi]) vols.push_back(std::move(modified[vi]));
        else { vols.push_back(ExtractVolume(*cur, vi)); vols.back().info = volState_[vi]; }
    }
    auto t = Clock::now();
    cur = Merge(vols, cur->configHash);
    cur->dbVersion = kDbVersion;
    Log(L"增量合并 %zu 个对象，用时 %.2f 秒", cur->entries.size(), Sec(t));
    return true;
}

void Indexer::Run(HWND hwnd, Options opt) {
    InitFold();
    for (auto& r : opt.roots) while (!r.empty() && r.back() == L'\\') r.pop_back();
    const uint64_t cfg = ConfigHash(opt.roots, opt.excludes, kDbVersion);
    IndexStats st;
    progress_ = 0;
    auto setStats = [&] { std::lock_guard lk(mu_); auto c = stats_.usnChanges, u = stats_.usnUnresolved; stats_ = st; stats_.usnChanges = c; stats_.usnUnresolved = u; };
    auto finish = [&] { busy_ = false; phase_ = IndexPhase::Idle; if (hwnd) PostMessageW(hwnd, WM_APP_INDEX_PROGRESS, 0, 0); };
    { std::lock_guard lk(mu_); stats_ = {}; }
    adminVols_.clear();

    // 1) 加载上次的数据库（启动时绝不全量重扫）
    std::shared_ptr<IndexData> cur;
    std::wstring err;
    if (!opt.forceRebuild && !opt.dbPath.empty()) {
        phase_ = IndexPhase::Loading;
        if (hwnd) PostMessageW(hwnd, WM_APP_INDEX_PROGRESS, 0, 0);
        auto t = Clock::now();
        cur = LoadDb(opt.dbPath, err);
        if (cur && cur->configHash != ConfigHash(opt.roots, opt.excludes, cur->dbVersion)) { err = L"索引卷或排除设置已变化"; cur.reset(); }
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
    bool dirty = false, cursorMoved = false;
    if (cur) {
        volState_ = cur->volumes;
        stale_.assign(volState_.size(), 0);
        if (!opt.updateAfterLoad) { finish(); return; }
        phase_ = IndexPhase::Updating;
        auto t = Clock::now();
        std::vector<uint32_t> staleVols;
        dirty = PollUsn(cur, opt, cursorMoved, &staleVols);
        // 过期卷：只重建该卷（MFT，需要管理员）；做不到就保留旧数据并提示
        bool rebuilt = false;
        std::vector<VolumeData> rebuiltVols(volState_.size());
        for (auto vi : staleVols) {
            if (stop_) break;
            VolumeData v; v.info.root = volState_[vi].root;
            phase_ = IndexPhase::Creating;
            if (BuildVolume(v, opt, note)) { volState_[vi] = v.info; stale_[vi] = 0; rebuiltVols[vi] = std::move(v); rebuilt = true; }
        }
        if (rebuilt) {
            std::vector<VolumeData> vols;
            for (uint32_t vi = 0; vi < volState_.size(); ++vi) {
                if (!rebuiltVols[vi].entries.empty()) vols.push_back(std::move(rebuiltVols[vi]));
                else { vols.push_back(ExtractVolume(*cur, vi)); vols.back().info = volState_[vi]; }
            }
            cur = Merge(vols, cfg); cur->dbVersion = kDbVersion;
            dirty = true;
        }
        for (uint32_t vi = 0; vi < volState_.size(); ++vi)
            if (volState_[vi].ntfs && !stale_[vi] && !dirty) Log(L"%s: using existing NTFS index", volState_[vi].root.c_str());
        st.updateSec = Sec(t);
        if (cur->dbVersion != kDbVersion) dirty = true;   // v2 -> v3 升级需要整库保存一次
    } else {
        // 2) 没有可用数据库：重建（NTFS 卷需要 MFT 权限；做不到则该卷为空并提示）
        phase_ = IndexPhase::Creating;
        if (hwnd) PostMessageW(hwnd, WM_APP_INDEX_PROGRESS, 0, 0);
        auto t = Clock::now();
        std::vector<VolumeData> vols;
        for (auto& r : opt.roots) {
            if (stop_) break;
            VolumeData v; v.info.root = r;
            if (!BuildVolume(v, opt, note)) {   // 保留一个只有根的卷，下次启动仍按过期处理
                v.entries.clear(); v.names.clear(); v.frn.clear();
                v.Add(r.c_str(), r.size(), kNoParent, kFlagDir, 0, 0);
                v.info.mft = false; v.info.journalId = 0;
            }
            vols.push_back(std::move(v));
        }
        st.rebuildSec = Sec(t);
        if (stop_) { finish(); return; }
        auto t2 = Clock::now();
        cur = Merge(vols, cfg); cur->dbVersion = kDbVersion;
        st.sortSec = Sec(t2);
        volState_ = cur->volumes;
        stale_.assign(volState_.size(), 0);
        dirty = true;
    }
    if (!adminVols_.empty()) note = adminVols_ + L" 需要读取 NTFS 主文件表才能更新：请以管理员身份运行闪寻，或安装 FlashFind 服务。" + note;
    st.note = note;
    if (!note.empty()) Log(L"%s", note.c_str());

    auto save = [&](bool full) {
        auto t = Clock::now();
        phase_ = IndexPhase::Saving;
        if (full) {
            cur->volumes = volState_; cur->configHash = cfg; cur->dbVersion = kDbVersion;
            if (SaveDb(opt.dbPath, *cur, opt.compress, &st.dbBytes)) { st.saveSec = Sec(t); st.fullSave = true; Log(L"保存数据库 %llu 字节，用时 %.2f 秒", st.dbBytes, st.saveSec); }
        } else if (UpdateDbVolumes(opt.dbPath, volState_)) {
            st.headerOnlyUpdate = true; st.saveSec = Sec(t);
            Log(L"无记录变化：只更新卷表（Journal ID + Next USN），用时 %.3f 秒", st.saveSec);
        }
    };
    if (dirty) Publish(hwnd, cur);
    if (!opt.dbPath.empty()) { if (dirty) save(true); else if (cursorMoved) save(false); }
    setStats();
    finish();

    // 3) 监视：持续读 USN（ntfs_volume_monitors），批量合并；退出时保存
    if (!opt.monitor) return;
    dirty = false; cursorMoved = false;
    auto lastApply = Clock::now();
    while (!stop_) {
        for (int k = 0; k < 10 && !stop_; ++k) Sleep(100);
        if (stop_ || Sec(lastApply) < 3.0) continue;
        bool moved = false;
        if (PollUsn(cur, opt, moved, nullptr)) { Publish(hwnd, cur); dirty = true; }
        cursorMoved = cursorMoved || moved;
        lastApply = Clock::now();
    }
    if (!opt.dbPath.empty()) { if (dirty) save(true); else if (cursorMoved) save(false); }
}

} // namespace ff
