// FlashFind 服务：-svc（SCM 启动，LocalSystem）、-svc-console（控制台测试）、-install-service / -uninstall-service。
#include "NtfsSource.h"
#include "Log.h"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <string>

namespace ff {

static SERVICE_STATUS_HANDLE g_ssh = nullptr;
static SERVICE_STATUS g_ss{};
static HANDLE g_stop = nullptr;

static void SetState(DWORD state, DWORD exitCode = NO_ERROR) {
    g_ss.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ss.dwCurrentState = state;
    g_ss.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    g_ss.dwWin32ExitCode = exitCode;
    g_ss.dwWaitHint = state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING ? 5000 : 0;
    SetServiceStatus(g_ssh, &g_ss);
}

static DWORD WINAPI Handler(DWORD ctl, DWORD, LPVOID, LPVOID) {
    if (ctl == SERVICE_CONTROL_STOP || ctl == SERVICE_CONTROL_SHUTDOWN) { SetState(SERVICE_STOP_PENDING); SetEvent(g_stop); return NO_ERROR; }
    return ctl == SERVICE_CONTROL_INTERROGATE ? NO_ERROR : ERROR_CALL_NOT_IMPLEMENTED;
}

static void ServiceLogPath() {
    PWSTR p = nullptr; std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &p))) { dir = p; CoTaskMemFree(p); }
    dir += L"\\FlashFind";
    CreateDirectoryW(dir.c_str(), nullptr);
    SetLogPath((dir + L"\\FlashFind-service.log").c_str());
}

static void WINAPI SvcMain(DWORD, LPWSTR*) {
    g_ssh = RegisterServiceCtrlHandlerExW(kServiceName, Handler, nullptr);
    if (!g_ssh) return;
    SetState(SERVICE_START_PENDING);
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetState(SERVICE_RUNNING);
    Log(L"FlashFind 服务启动（pid %lu）", GetCurrentProcessId());
    int r = RunPipeServer(g_stop);
    Log(L"FlashFind 服务停止（%d）", r);
    SetState(SERVICE_STOPPED, r ? ERROR_SERVICE_SPECIFIC_ERROR : NO_ERROR);
}

int ServiceMain() {
    ServiceLogPath();
    SERVICE_TABLE_ENTRYW t[] = {{const_cast<LPWSTR>(kServiceName), SvcMain}, {nullptr, nullptr}};
    if (!StartServiceCtrlDispatcherW(t)) { Log(L"StartServiceCtrlDispatcher 失败 %lu（-svc 只能由服务管理器启动；测试请用 -svc-console）", GetLastError()); return 1; }
    return 0;
}

static std::wstring ExePath() { wchar_t p[MAX_PATH * 4]; GetModuleFileNameW(nullptr, p, MAX_PATH * 4); return p; }

int InstallService() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) return static_cast<int>(GetLastError());
    std::wstring bin = L"\"" + ExePath() + L"\" -svc";
    SC_HANDLE s = CreateServiceW(scm, kServiceName, L"FlashFind 闪寻索引服务", SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                                 SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, bin.c_str(), nullptr, nullptr, nullptr, nullptr /*LocalSystem*/, nullptr);
    DWORD e = s ? 0 : GetLastError();
    if (!s && e == ERROR_SERVICE_EXISTS) {
        s = OpenServiceW(scm, kServiceName, SERVICE_ALL_ACCESS);
        if (s && !ChangeServiceConfigW(s, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE, bin.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr)) e = GetLastError();
        else e = s ? 0 : GetLastError();
    }
    if (s) {
        SERVICE_DESCRIPTIONW d{const_cast<LPWSTR>(L"为闪寻读取 NTFS 主文件表与 USN 日志（只读），通过本机命名管道提供给未提升的界面。")};
        ChangeServiceConfig2W(s, SERVICE_CONFIG_DESCRIPTION, &d);
        if (!StartServiceW(s, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING && !e) e = GetLastError();
        // 等待进入运行状态（最多 10 秒）
        for (int k = 0; k < 100; ++k) {
            SERVICE_STATUS st{}; if (QueryServiceStatus(s, &st) && st.dwCurrentState == SERVICE_RUNNING) break;
            Sleep(100);
        }
        CloseServiceHandle(s);
    }
    CloseServiceHandle(scm);
    return static_cast<int>(e);
}

int UninstallService() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return static_cast<int>(GetLastError());
    SC_HANDLE s = OpenServiceW(scm, kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    DWORD e = s ? 0 : GetLastError();
    if (s) {
        SERVICE_STATUS st{};
        if (ControlService(s, SERVICE_CONTROL_STOP, &st))
            for (int k = 0; k < 100 && QueryServiceStatus(s, &st) && st.dwCurrentState != SERVICE_STOPPED; ++k) Sleep(100);
        if (!DeleteService(s)) e = GetLastError();
        CloseServiceHandle(s);
    }
    CloseServiceHandle(scm);
    return static_cast<int>(e);
}

bool ServiceInstalled() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE s = OpenServiceW(scm, kServiceName, SERVICE_QUERY_STATUS);
    if (s) CloseServiceHandle(s);
    CloseServiceHandle(scm);
    return s != nullptr;
}

DWORD RunElevated(HWND owner, const wchar_t* args) {
    std::wstring exe = ExePath();
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.hwnd = owner; sei.lpVerb = L"runas"; sei.lpFile = exe.c_str(); sei.lpParameters = args; sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) return GetLastError();   // 用户取消 UAC = ERROR_CANCELLED
    DWORD code = ERROR_GEN_FAILURE;
    if (sei.hProcess) {
        // 等待时继续处理消息，界面不假死
        for (;;) {
            DWORD r = MsgWaitForMultipleObjects(1, &sei.hProcess, FALSE, INFINITE, QS_ALLINPUT);
            if (r == WAIT_OBJECT_0) break;
            MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        }
        GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
    }
    return code;
}

} // namespace ff
