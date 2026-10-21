#include "MftIndexer.h"
#include <windows.h>

namespace ff {
bool MftIndexer::Available() { return false; /* TODO(M3) */ }

std::vector<std::wstring> MftIndexer::NtfsVolumes() {
    std::vector<std::wstring> r;
    DWORD mask = GetLogicalDrives();
    for (int d = 0; d < 26; ++d) {
        if (!(mask & (1u << d))) continue;
        wchar_t root[] = {static_cast<wchar_t>(L'A' + d), L':', L'\\', 0};
        wchar_t fs[32] = {};
        if (GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, nullptr, fs, 32) && wcscmp(fs, L"NTFS") == 0)
            r.push_back(std::wstring(root, 2));
    }
    return r;
}
}
