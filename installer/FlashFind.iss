; 闪寻安装脚本：由 scripts/package.ps1 调用 ISCC 编译
#define AppName "闪寻 FlashFind"
#ifndef AppVersion
  #define AppVersion "0.1.1"
#endif
#define AppExe "FlashFind.exe"
#ifndef AppArchitecture
  #define AppArchitecture "x64"
#endif
#ifndef AppSourceDir
  #error Use scripts/package.ps1 to prepare the release payload.
#endif
#ifndef OutputDirectory
  #define OutputDirectory "..\dist"
#endif

[Setup]
AppId={{8C3F5A1E-2D7B-4E96-B0A4-6F1D9E2C7B53}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=FlashFind contributors
AppPublisherURL=https://github.com/nullTerminatedCat/FlashFind
AppSupportURL=https://github.com/nullTerminatedCat/FlashFind/issues
DefaultDirName={autopf}\FlashFind
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
OutputDir={#OutputDirectory}
OutputBaseFilename=FlashFind-{#AppVersion}-windows-{#AppArchitecture}-setup
SetupIconFile=..\resources\FlashFind.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName} {#AppVersion}
LicenseFile=..\LICENSE
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; 默认仅为当前用户安装（无需管理员）；勾选“安装 FlashFind 服务”需改为所有用户（管理员）
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "chinesesimp"; MessagesFile: "ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加选项:"; Flags: unchecked
Name: "autostart"; Description: "开机时自动启动闪寻"; GroupDescription: "附加选项:"
Name: "service"; Description: "安装 FlashFind 索引服务（读取 NTFS 主文件表/USN，无需以管理员运行闪寻）"; GroupDescription: "索引服务:"; Flags: unchecked; Check: IsAdminInstallMode

[Files]
Source: "{#AppSourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{group}\卸载{#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "FlashFind"; ValueData: """{app}\{#AppExe}"" -startup"; Tasks: autostart

[Run]
Filename: "{app}\{#AppExe}"; Parameters: "-install-service"; StatusMsg: "正在安装 FlashFind 服务…"; Flags: runhidden waituntilterminated; Tasks: service
Filename: "{app}\{#AppExe}"; Description: "立即启动闪寻"; Flags: nowait postinstall skipifsilent runasoriginaluser

[UninstallRun]
; 卸载时停止并删除服务（未安装服务时该命令直接失败，不影响卸载）
Filename: "{app}\{#AppExe}"; Parameters: "-uninstall-service"; Flags: runhidden waituntilterminated; RunOnceId: "RemoveFlashFindService"; Check: IsAdminInstallMode

[Code]
procedure KillRunning();
var
  Code: Integer;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM {#AppExe}', '', SW_HIDE, ewWaitUntilTerminated, Code);
end;

procedure StopService();
var
  Code: Integer;
begin
  // 服务进程锁定 exe：覆盖安装前先停止（非管理员时会失败，忽略）
  Exec(ExpandConstant('{sys}\sc.exe'), 'stop FlashFind', '', SW_HIDE, ewWaitUntilTerminated, Code);
  Sleep(1000);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  StopService();
  KillRunning();
  Result := '';
end;

function InitializeUninstall(): Boolean;
begin
  KillRunning();
  Result := True;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Code: Integer;
begin
  // 覆盖安装且未勾选任务：若服务已存在则重新启动
  if (CurStep = ssPostInstall) and IsAdminInstallMode and not WizardIsTaskSelected('service') then
    Exec(ExpandConstant('{sys}\sc.exe'), 'start FlashFind', '', SW_HIDE, ewWaitUntilTerminated, Code);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    RegDeleteValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Run', 'FlashFind');
end;
