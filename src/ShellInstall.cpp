#include "ShellInstall.h"
#include <string>

namespace ff {

namespace {
std::wstring ExeQuoted() {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return L"\"" + std::wstring(exe) + L"\"";
}
bool RegWrite(HKEY root, const wchar_t* sub, const wchar_t* name, const std::wstring& val) {
    HKEY k; if (RegCreateKeyExW(root, sub, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return false;
    LONG r = RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(val.c_str()), static_cast<DWORD>((val.size() + 1) * 2));
    RegCloseKey(k); return r == ERROR_SUCCESS;
}
bool RegExists(HKEY root, const wchar_t* sub) {
    HKEY k; if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    RegCloseKey(k); return true;
}
void RegDelTree(HKEY root, const wchar_t* sub) {
    // Win10+：RegDeleteTreeW
    RegDeleteTreeW(root, sub);
}
}

bool UrlProtocolInstalled() { return RegExists(HKEY_CURRENT_USER, L"Software\\Classes\\es"); }

bool FolderContextMenuInstalled() {
    return RegExists(HKEY_CURRENT_USER, L"Software\\Classes\\Directory\\shell\\FlashFindSearch")
        || RegExists(HKEY_CURRENT_USER, L"Software\\Classes\\Directory\\Background\\shell\\FlashFindSearch");
}

bool InstallUrlProtocol(bool install, std::wstring* err) {
    if (!install) { RegDelTree(HKEY_CURRENT_USER, L"Software\\Classes\\es"); return true; }
    std::wstring cmd = ExeQuoted() + L" -url \"%1\"";
    bool ok = RegWrite(HKEY_CURRENT_USER, L"Software\\Classes\\es", nullptr, L"URL:FlashFind Search")
           && RegWrite(HKEY_CURRENT_USER, L"Software\\Classes\\es", L"URL Protocol", L"")
           && RegWrite(HKEY_CURRENT_USER, L"Software\\Classes\\es\\shell\\open\\command", nullptr, cmd);
    if (!ok && err) *err = L"写入注册表失败";
    return ok;
}

bool InstallFolderContextMenu(bool install, std::wstring* err) {
    const wchar_t* keys[] = {
        L"Software\\Classes\\Directory\\shell\\FlashFindSearch",
        L"Software\\Classes\\Directory\\Background\\shell\\FlashFindSearch",
    };
    if (!install) {
        for (auto k : keys) RegDelTree(HKEY_CURRENT_USER, k);
        return true;
    }
    std::wstring cmdDir = ExeQuoted() + L" -p \"%1\"";
    std::wstring cmdBg = ExeQuoted() + L" -p \"%V\"";
    bool ok = RegWrite(HKEY_CURRENT_USER, keys[0], nullptr, L"用闪寻搜索")
           && RegWrite(HKEY_CURRENT_USER, (std::wstring(keys[0]) + L"\\command").c_str(), nullptr, cmdDir)
           && RegWrite(HKEY_CURRENT_USER, keys[1], nullptr, L"用闪寻搜索")
           && RegWrite(HKEY_CURRENT_USER, (std::wstring(keys[1]) + L"\\command").c_str(), nullptr, cmdBg);
    if (!ok && err) *err = L"写入注册表失败";
    return ok;
}

} // namespace ff
