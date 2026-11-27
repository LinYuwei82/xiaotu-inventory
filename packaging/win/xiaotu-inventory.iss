; 小兔进销存 Windows 安装包（Inno Setup 6）
;
; 本地编译（在 Windows + Inno Setup 6 环境）：
;   ISCC.exe /DAppVersion=<版本> packaging\win\xiaotu-inventory.iss
; 壳路径解析（与 make-zip.ps1 同一套规则）：
;   1. 显式 /DShellExe=<路径> 优先，指定了就必须存在（建议传绝对路径）；
;   2. 未指定时依次查找 build\windows\shell\win\、build\windows\shell\win\Release\、
;      旧的 shell\win\build\、shell\win\build\Release\；
;   3. 都找不到时编译报错，不会静默产出无壳安装包；确实需要请显式 /DXIAOTU_NO_SHELL=1。
; 产物默认输出到仓库根 dist\ 目录（可用 /O 覆盖）。
; CI 由 .github/workflows/windows-build.yml 调用，版本号取自仓库根 VERSION。
;
; 签名：只有传入 /DSign=1 且 /Ssigntool=<命令> 时才会签名；
; 未传时产物为未签名安装包，CI 会在 SIGNING.txt 里写明，不会假装已签名。

#ifndef AppVersion
  #error 请传入 /DAppVersion=<版本号>（CI 从仓库根 VERSION 读取）
#endif

; 注意：预处理器宏用 XIAOTU_NO_SHELL，不能叫 NoShell——
; 脚本里已有同名 Pascal 函数 NoShell()，宏替换会把函数声明和 Check 参数一起改坏。
#ifndef XIAOTU_NO_SHELL
  #define XIAOTU_NO_SHELL 0
#endif

#define RepoRoot AddBackslash(AddBackslash(SourcePath) + "..\..")
#define ShellCandidate1 AddBackslash(RepoRoot) + "build\windows\shell\win\xiaotu-shell.exe"
#define ShellCandidate2 AddBackslash(RepoRoot) + "build\windows\shell\win\Release\xiaotu-shell.exe"
#define ShellCandidate3 AddBackslash(RepoRoot) + "shell\win\build\xiaotu-shell.exe"
#define ShellCandidate4 AddBackslash(RepoRoot) + "shell\win\build\Release\xiaotu-shell.exe"

#if !XIAOTU_NO_SHELL
  ; 只在用户未传 /DShellExe 时定义 ShellExe，避免 ISPP 重复定义宏。
  #ifndef ShellExe
    #if FileExists(ShellCandidate1)
      #define ShellExe ShellCandidate1
    #elif FileExists(ShellCandidate2)
      #define ShellExe ShellCandidate2
    #elif FileExists(ShellCandidate3)
      #define ShellExe ShellCandidate3
    #elif FileExists(ShellCandidate4)
      #define ShellExe ShellCandidate4
    #else
      #error 未找到 Windows 桌面壳 xiaotu-shell.exe。已查找 build\windows\shell\win\、build\windows\shell\win\Release\、shell\win\build\、shell\win\build\Release\。构建：cmake --preset windows-release -DXIAOTU_BUILD_WIN_SHELL=ON; cmake --build --preset windows-release。如确实要无壳包，请加 /DXIAOTU_NO_SHELL=1。
    #endif
  #elif !FileExists(ShellExe)
    #error 指定的 /DShellExe 指向的文件不存在；显式指定时不会回退到其他路径。
  #endif
  ; 壳与它依赖的 OpenSSL 运行时 DLL 同在壳产物目录（CMake POST_BUILD 拷贝）。
  #define ShellDir ExtractFilePath(ShellExe)
#endif

#ifndef OutputDir
  #define OutputDir RepoRoot + "dist"
#endif

#define AppName "小兔进销存"
#define AppPublisher "si_mem"
#define AppCopyright "Copyright © 2026 si_mem"
#define AppExeName "xiaotu-shell.exe"
#define ServerExeName "xiaotu-server.exe"
#define WebView2Url "https://go.microsoft.com/fwlink/p/?LinkId=2124703"
#define WebView2InstallerName "MicrosoftEdgeWebview2Setup.exe"

[Setup]
AppId={{6E2A9B4D-5C1F-4F3B-9D2E-7A8B1C3D5E6F}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
AppCopyright={#AppCopyright}
; 当前用户安装：不请求管理员权限，数据目录与提权账户无关。
PrivilegesRequired=lowest
DefaultDirName={localappdata}\Programs\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
OutputDir={#OutputDir}
OutputBaseFilename=xiaotu-inventory-{#AppVersion}-win64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayName={#AppName} {#AppVersion}
; 安装包自身在资源管理器里的图标：与 macOS/两个 exe 同为候选 A。
; 安装向导图（WizardImageFile / WizardSmallImageFile）未自定义，仍是 Inno 默认占位图。
SetupIconFile=xiaotu.ico
; 卸载项指向始终存在的服务端 exe（无论是否带壳），避免指向可能不存在的壳。
UninstallDisplayIcon={app}\{#ServerExeName}
; RestartIfRunning 不是 [Setup] 指令！静默覆盖场景下壳占用处理靠 [Code] 里的 KillOldShell：
;   ssPreInstall 阶段 taskkill /F /IM xiaotu-shell.exe → 文件释放 → 安装继续 → 启新壳
; 见本脚本 [Code] 段 CurStepChanged。
#ifdef Sign
SignTool=signtool
SignedUninstaller=yes
#endif

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加任务："; Flags: checkedonce
#if !XIAOTU_NO_SHELL
; 仅在本机检测不到 WebView2 Runtime 时出现；勾选后安装期下载官方 bootstrapper 并静默安装。
Name: "webview2"; Description: "下载并安装 WebView2 运行环境（桌面窗口必需，需要联网）"; GroupDescription: "附加任务："; Check: not WebView2Installed
#endif

[Files]
; 服务端必须存在；壳缺失时编译期直接失败，确实要出无壳包请显式 /DXIAOTU_NO_SHELL=1。
Source: "..\..\build\windows\server\{#ServerExeName}"; DestDir: "{app}"; Flags: ignoreversion
#if !XIAOTU_NO_SHELL
Source: "{#ShellExe}"; DestDir: "{app}"; Flags: ignoreversion
; 壳的 OpenSSL 运行时依赖（libcrypto-3-x64.dll / libssl-3-x64.dll）：必须与壳
; 同级，Windows 只在 exe 目录、System32 与 PATH 里找 DLL。文件名是 OpenSSL 3
; 在 Windows x64 上的固定命名；用 FileExists 包住，缺 DLL 时不至于让编译直接失败
; （绿色包脚本 make-zip.ps1 会在缺 DLL 时报错，不会静默出坏包）。
#if FileExists(ShellDir + "libcrypto-3-x64.dll")
Source: "{#ShellDir}libcrypto-3-x64.dll"; DestDir: "{app}"; Flags: ignoreversion
#endif
#if FileExists(ShellDir + "libssl-3-x64.dll")
Source: "{#ShellDir}libssl-3-x64.dll"; DestDir: "{app}"; Flags: ignoreversion
#endif
#endif
Source: "..\..\deploy\config.toml.example"; DestDir: "{app}"; Flags: ignoreversion; DestName: "config.toml.example"
Source: "package-readme.md"; DestDir: "{app}"; Flags: ignoreversion; DestName: "README.md"

[Icons]
; 未设置 IconFilename：快捷方式继承 exe 资源图标；shell 与 server 两个 exe 都已嵌入候选 A，
; 因此带壳与无壳（浏览器访问）两种快捷方式都会显示新图标。
; 当前用户安装：开始菜单与桌面快捷方式都写当前用户配置，不涉及公共目录。
Name: "{userprograms}\{#AppName}"; Filename: "{app}\{#AppExeName}"; WorkingDir: "{app}"; Check: HasShell
Name: "{userprograms}\{#AppName}（浏览器访问）"; Filename: "{app}\{#ServerExeName}"; Parameters: "--server"; WorkingDir: "{app}"; Check: NoShell
Name: "{userdesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; WorkingDir: "{app}"; Tasks: desktopicon; Check: HasShell

[Run]
#if !XIAOTU_NO_SHELL
; 勾选 webview2 任务时：PrepareToInstall 已把官方 bootstrapper 下载到 {tmp}，这里静默安装。
; 必须排在启动应用之前，否则壳可能在 Runtime 装好前就起来了。
; 安装失败时下面的「下载并安装 WebView2」入口仍然可见（NeedWebView2 会重新查注册表）。
Filename: "{tmp}\{#WebView2InstallerName}"; Parameters: "/silent /install"; StatusMsg: "正在安装 WebView2 运行环境…"; Flags: runhidden waituntilterminated; Check: ShouldRunWebView2Setup
#endif
; 静默覆盖更新：安装完成后把新壳拉起来，否则窗口消失后不会自动恢复。
Filename: "{app}\{#AppExeName}"; Flags: nowait runasoriginaluser; Check: RestartAfterSilent
Filename: "{app}\{#AppExeName}"; Description: "启动{#AppName}"; Flags: postinstall nowait skipifsilent; Check: HasShell
; 壳依赖 WebView2 Runtime：缺失时在完成页提供官方安装入口（见 [Code] 的注册表检查）。
Filename: "{#WebView2Url}"; Description: "下载并安装 WebView2 Runtime（桌面窗口必需）"; Flags: postinstall shellexec nowait skipifsilent runasoriginaluser; Check: NeedWebView2

[Code]
{ 仅下载 WebView2 Evergreen 官方引导程序；安装交给 [Run] 的 /silent /install。 }
function URLDownloadToFile(pCaller: LongWord; szURL, szFileName: String;
  dwReserved: LongWord; lpfnCB: LongWord): Integer;
  external 'URLDownloadToFileW@urlmon.dll stdcall';

{ 等待用：Inno 脚本自带的 Sleep 要 6.3+，CI 的 Inno 版本不保证，直接导入 kernel32。 }
procedure KernelSleep(dwMilliseconds: LongWord);
  external 'Sleep@kernel32.dll stdcall';

var
  DataDirPage: TInputDirWizardPage;
  WebView2SetupPath: String;
  WebView2SetupReady: Boolean;
  WasInstalled: Boolean;

function DefaultDataDir: String;
begin
  { 与应用默认数据目录一致：%LOCALAPPDATA%\com.simem.xiaotu }
  Result := ExpandConstant('{localappdata}\com.simem.xiaotu');
end;

function ConfigFilePath: String;
begin
  Result := DefaultDataDir + '\config.toml';
end;

function HasShell: Boolean;
begin
  Result := FileExists(ExpandConstant('{app}\{#AppExeName}'));
end;

function NoShell: Boolean;
begin
  Result := not HasShell;
end;

// 静默覆盖更新（updater_win 调 /VERYSILENT）装完要把新壳拉起来：
// 壳是安装脚本 taskkill 掉的，不在 Restart Manager 管理内，/RESTARTAPPLICATIONS
// 不会替我们重启；而下面带 skipifsilent 的「启动应用」项在静默模式下会被跳过，
// 结果就是窗口消失后再也不回来（用户看到的是「点安装更新就闪退」）。
//
// 注意 WasInstalled 单靠不住：用户跑「绿色解压版」时壳不在 app 安装目录，
// WasInstalled=False，但 taskkill 按映像名照样杀掉它——装完不重启就是必现闪退。
// updater 拉起安装时永远带 /RESTARTAPPLICATIONS，据此识别「更新触发的静默安装」，
// 首次静默部署（不带该参数）仍然不会莫名弹窗起应用。
// （[Code] 段只能用 // 或 { } 注释；{ } 不能嵌套，注释里出现 {app} 会提前闭合。）
// 协助器模式（1.0.19+）：安装由 apply-helper 同步执行并负责重启，环境变量
// XIAOTU_APPLY_HELPER=1 时 Inno 不再自己重启，避免双重启抢单实例锁。
// 旧壳不带该变量，仍走 /RESTARTAPPLICATIONS 识别路径，行为不变。
function RestartAfterSilent: Boolean;
begin
  Result := WizardSilent and HasShell and
    (WasInstalled or (Pos('/RESTARTAPPLICATIONS', GetCmdTail) > 0)) and
    (GetEnv('XIAOTU_APPLY_HELPER') = '');
end;

{ WebView2 Runtime 检测：机器级 x64/x86 与用户级注册表任一存在且版本非空即视为已安装。 }
function WebView2Installed: Boolean;
var
  Version: String;
begin
  Result :=
    RegQueryStringValue(HKLM, 'SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}', 'pv', Version) or
    RegQueryStringValue(HKLM32, 'SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}', 'pv', Version) or
    RegQueryStringValue(HKCU, 'Software\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}', 'pv', Version);
  if Result then
    Result := (Version <> '') and (Version <> '0.0.0.0');
end;

function NeedWebView2: Boolean;
begin
  { 仅桌面壳需要 WebView2；纯浏览器模式不拦。 }
  Result := HasShell and (not WebView2Installed);
end;

{ 下载 WebView2 Evergreen bootstrapper 到临时目录；失败不中断安装，留给完成页手动入口。 }
function DownloadWebView2Bootstrapper: Boolean;
var
  Res: Integer;
begin
  WebView2SetupPath := ExpandConstant('{tmp}\{#WebView2InstallerName}');
  Res := URLDownloadToFile(0, '{#WebView2Url}', WebView2SetupPath, 0, 0);
  Result := (Res = 0) and FileExists(WebView2SetupPath);
end;

{ 在复制文件前完成下载，[Run] 才有安装包可执行。 }
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  WebView2SetupReady := False;
  if not (WizardIsTaskSelected('webview2') or WizardSilent) then
    Exit;
  if WebView2Installed then
    Exit;
  WizardForm.StatusLabel.Caption := '正在下载 WebView2 运行环境…';
  WizardForm.Refresh;
  WebView2SetupReady := DownloadWebView2Bootstrapper;
  if not WebView2SetupReady then
    SuppressibleMsgBox('未能下载 WebView2 运行环境（可能未联网）。' + #13#10#13#10 +
           '安装完成后可在完成页手动安装，或直接使用「浏览器访问」入口。',
           mbInformation, MB_OK, IDOK);
  WizardForm.StatusLabel.Caption := '';
end;

{ [Run] 静默安装条件：下载成功，且此刻注册表里仍没有 Runtime。 }
function ShouldRunWebView2Setup: Boolean;
begin
  Result := WebView2SetupReady and (not WebView2Installed);
end;

procedure InitializeWizard;
var
  PageCaption: String;
begin
  { 已有 config.toml 时不再提供修改，避免覆盖用户已有配置或让选择静默失效。
    说明文字只能通过 CreateInputDirPage 的第 4 个参数传入（向导页没有
    SubCaption 属性），所以先判断再建页。 }
  if FileExists(ConfigFilePath) then
    PageCaption := '检测到已有 config.toml，安装器不会改动它；' +
      '如需更换数据目录，请直接编辑 ' + ConfigFilePath + '。'
  else
    PageCaption := '数据库、日志与备份都放在这个目录。默认放在当前用户的应用数据目录；' + #13#10 +
      '卸载时会保留该目录并提示，不会自动删除数据。';
  DataDirPage := CreateInputDirPage(wpSelectDir,
    '数据目录', '选择小兔进销存的数据保存位置', PageCaption, False, '');
  DataDirPage.Add('数据目录：');
  DataDirPage.Values[0] := DefaultDataDir;

  if FileExists(ConfigFilePath) then
  begin
    DataDirPage.Values[0] := DefaultDataDir;
    DataDirPage.Edits[0].Enabled := False;
  end;
end;

procedure WriteDataConfig;
var
  Chosen: String;
  Content: String;
  TargetDir: String;
begin
  if DataDirPage = nil then
    Exit;
  Chosen := Trim(DataDirPage.Values[0]);
  if Chosen = '' then
    Exit;
  { 默认目录无需写配置；已有配置文件时保持原样（InitializeWizard 已禁用编辑）。 }
  if CompareText(Chosen, DefaultDataDir) = 0 then
    Exit;
  if FileExists(ConfigFilePath) then
    Exit;

  ForceDirectories(DefaultDataDir);
  ForceDirectories(Chosen);

  TargetDir := Chosen;
  StringChangeEx(TargetDir, '\', '/', True);
  Content := '# 由安装程序生成：数据目录选择' + #13#10 +
             '[storage]' + #13#10 +
             'data_dir = "' + TargetDir + '"' + #13#10;
  if not SaveStringToFile(ConfigFilePath, Content, False) then
    SuppressibleMsgBox('写入配置文件失败：' + ConfigFilePath + #13#10 +
           '应用将使用默认数据目录 ' + DefaultDataDir, mbError, MB_OK, IDOK);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
begin
  if CurStep = ssPostInstall then
  begin
    WriteDataConfig;
    if NeedWebView2 and (not WebView2SetupReady) then
      SuppressibleMsgBox('本机未检测到 WebView2 Runtime。' + #13#10#13#10 +
             '桌面窗口依赖该运行时；本次未自动安装，完成页提供了官方安装入口，装好后重新打开即可。' + #13#10 +
             '也可以直接用开始菜单里的「浏览器访问」入口。', mbInformation, MB_OK, IDOK);
  end;

  { 自动安装跑完后再查一次注册表；静默安装失败时给出明确提示，不静默放过。 }
  if (CurStep = ssDone) and (WizardIsTaskSelected('webview2') or WizardSilent) and (not WebView2Installed) then
    SuppressibleMsgBox('WebView2 运行环境未安装成功，桌面窗口可能无法打开。' + #13#10#13#10 +
           '请手动安装：{#WebView2Url}', mbError, MB_OK, IDOK);

  // 静默覆盖更新（updater_win 调 /VERYSILENT）时，壳文件会被占用导致安装失败。
  // 这里在安装开始前强制关闭旧壳，释放文件锁；安装完后 [Run] 段的
  // RestartAfterSilent 会拉起新壳（不能依赖 skipifsilent 那一项）。
  // WasInstalled 必须在覆盖前取，用来区分「真升级」与「首次静默安装」。
  //
  // 必须先发不带 /F 的温和关闭（对 GUI 进程即 WM_CLOSE）：壳收到 WM_CLOSE 会走
  // 正常退出流程，WebView2 运行时随之干净退出。直接 /F 强杀会把
  // msedgewebview2.exe（不是壳的子进程，/T 也杀不到）留成僵尸，继续锁住
  // 数据目录下的 webview2 用户目录，新壳启动创建环境失败 → 又是「闪退」。
  // 温和关闭等 2 秒后仍存活才 /F 兜底。
  if CurStep = ssInstall then
  begin
    WasInstalled := FileExists(ExpandConstant('{app}\{#AppExeName}'));
    Exec('taskkill.exe', '/IM xiaotu-shell.exe /T', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    KernelSleep(2000);
    Exec('taskkill.exe', '/F /IM xiaotu-shell.exe /T', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    KernelSleep(500);
  end;
end;

function ReadConfiguredDataDir: String;
var
  Lines: TArrayOfString;
  I, P: Integer;
  Line: String;
begin
  Result := '';
  if not LoadStringsFromFile(ConfigFilePath, Lines) then
    Exit;
  for I := 0 to GetArrayLength(Lines) - 1 do
  begin
    Line := Trim(Lines[I]);
    if Pos('data_dir', Line) = 1 then
    begin
      P := Pos('"', Line);
      if P > 0 then
      begin
        Delete(Line, 1, P);
        P := Pos('"', Line);
        if P > 0 then
          Result := Copy(Line, 1, P - 1);
      end;
    end;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  DataDir: String;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    DataDir := ReadConfiguredDataDir;
    if DataDir = '' then
      DataDir := DefaultDataDir;
    if DirExists(DataDir) then
      SuppressibleMsgBox('数据目录已保留：' + DataDir + #13#10#13#10 +
             '卸载不会删除数据库、日志与备份。确认不再需要时，请手动删除该目录。',
             mbInformation, MB_OK, IDOK);
  end;
end;
