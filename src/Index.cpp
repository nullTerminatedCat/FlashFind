#include "Index.h"
#include "App.h"
#include "Util.h"
#include <algorithm>

namespace ff {

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
    if (len == 2 && buf[1] == L':' && len < cap - 1) buf[len++] = L'\\';  // "C:" -> "C:\"
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

std::wstring IndexData::ParentPath(uint32_t i) const {
    wchar_t b[32768]; size_t n = ParentPathInto(i, b, 32768); return {b, n};
}
std::wstring IndexData::FullPath(uint32_t i) const {
    wchar_t b[32768]; size_t n = FullPathInto(i, b, 32768); return {b, n};
}

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

void Indexer::Start(HWND hwnd, std::vector<std::wstring> roots, std::vector<std::wstring> excludes) {
    Stop();
    stop_ = false;
    busy_ = true;
    thread_ = std::thread([this, hwnd, roots = std::move(roots), excludes = std::move(excludes)]() mutable {
        Run(hwnd, std::move(roots), std::move(excludes));
    });
}

void Indexer::Stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    busy_ = false;
}

std::shared_ptr<const IndexData> Indexer::Snapshot() const {
    std::lock_guard lk(mu_);
    return data_;
}

std::wstring Indexer::CurrentFolder() const {
    std::lock_guard lk(mu_);
    return current_;
}

void Indexer::Run(HWND hwnd, std::vector<std::wstring> roots, std::vector<std::wstring> excludes) {
    auto d = std::make_shared<IndexData>();
    d->entries.reserve(1 << 20);
    d->names.reserve(1 << 24);

    for (auto& ex : excludes) ex = ToLower(ex);

    auto addName = [&](const wchar_t* s, size_t len, uint32_t parent, uint32_t flags, uint64_t size, uint64_t mtime) -> uint32_t {
        Entry e;
        e.nameOff = static_cast<uint32_t>(d->names.size());
        e.nameLen = static_cast<uint32_t>(len);
        e.parent = parent; e.flags = flags; e.size = size; e.mtime = mtime;
        d->names.insert(d->names.end(), s, s + len);
        d->entries.push_back(e);
        return static_cast<uint32_t>(d->entries.size() - 1);
    };

    struct Pending { uint32_t idx; std::wstring path; };
    std::vector<Pending> stack;
    DWORD lastPost = GetTickCount();

    for (auto& root : roots) {
        std::wstring r = root;
        while (!r.empty() && r.back() == L'\\') r.pop_back();
        if (r.empty()) continue;
        uint32_t ri = addName(r.c_str(), r.size(), kNoParent, 1, 0, 0);
        stack.push_back({ri, r});

        while (!stack.empty() && !stop_) {
            Pending cur = std::move(stack.back());
            stack.pop_back();
            {
                std::lock_guard lk(mu_);
                current_ = cur.path;
            }
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
                uint32_t idx = addName(n, wcslen(n), cur.idx, dir ? 1u : 0u, dir ? 0 : size, mt);
                // 不跟随联接点/符号链接，避免环路
                if (dir && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                    std::wstring child = cur.path + L"\\" + n;
                    std::wstring lc = ToLower(child);
                    if (std::find(excludes.begin(), excludes.end(), lc) == excludes.end())
                        stack.push_back({idx, std::move(child)});
                }
            } while (FindNextFileW(h, &fd) && !stop_);
            FindClose(h);

            if (GetTickCount() - lastPost > 200) {
                lastPost = GetTickCount();
                PostMessageW(hwnd, WM_APP_INDEX_PROGRESS, d->entries.size(), 0);
            }
        }
        stack.clear();
    }
    if (stop_) { busy_ = false; return; }

    // 小写副本 + 名称排序
    d->lowerNames = d->names;
    if (!d->lowerNames.empty()) CharLowerBuffW(d->lowerNames.data(), static_cast<DWORD>(d->lowerNames.size()));
    d->byName.resize(d->entries.size());
    for (uint32_t i = 0; i < d->byName.size(); ++i) d->byName[i] = i;
    const IndexData* dp = d.get();
    std::sort(d->byName.begin(), d->byName.end(), [dp](uint32_t a, uint32_t b) {
        auto x = dp->LowerName(a), y = dp->LowerName(b);
        if (x != y) return x < y;
        return a < b;
    });

    {
        std::lock_guard lk(mu_);
        data_ = std::move(d);
        current_.clear();
    }
    busy_ = false;
    PostMessageW(hwnd, WM_APP_INDEX_DONE, 0, 0);
}

} // namespace ff
