; 小兔进销存 Windows 安装包（Inno Setup 6）
;
; 本地编译（在 Windows + Inno Setup 6 环境）：
;   ISCC.exe /DAppVersion=<版本> packaging\win\xiaotu-inventory.iss
; 产物默认输出到脚本同级的 out\ 目录（可用 /O 覆盖）。
; CI 由 .github/workflows/windows-build.yml 调用，版本号取自仓库根 VERSION。
;
; 签名：只有传入 /DSign=1 且 /Ssigntool=<命令> 时才会签名；
; 未传时产物为未签名安装包，CI 会在 SIGNING.txt 里写明，不会假装已签名。

#ifndef AppVersion
  #error 请传入 /DAppVersion=<版本号>（CI 从仓库根 VERSION 读取）
#endif

#ifndef OutputDir
  #define OutputDir AddBackslash(SourcePath) + "out"
#endif

#define AppName "小兔进销存"
#define AppPublisher "si_mem"
#define AppCopyright "Copyright © 2026 si_mem"
#define AppExeName "xiaotu-shell.exe"
#define ServerExeName "xiaotu-server.exe"
#define WebView2Url "https://go.microsoft.com/fwlink/p/?LinkId=2124703"

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
UninstallDisplayIcon={app}\{#AppExeName}
#ifdef Sign
SignTool=signtool
SignedUninstaller=yes
#endif

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加任务："; Flags: checkedonce

[Files]
; 服务端必须存在；壳缺失时安装包仍可编出「仅浏览器访问」的版本。
Source: "..\..\build\windows\server\{#ServerExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\shell\win\build\{#AppExeName}"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "..\..\deploy\config.toml.example"; DestDir: "{app}"; Flags: ignoreversion; DestName: "config.toml.example"
Source: "package-readme.md"; DestDir: "{app}"; Flags: ignoreversion; DestName: "README.md"

[Icons]
; 当前用户安装：开始菜单与桌面快捷方式都写当前用户配置，不涉及公共目录。
Name: "{userprograms}\{#AppName}"; Filename: "{app}\{#AppExeName}"; WorkingDir: "{app}"; Check: HasShell
Name: "{userprograms}\{#AppName}（浏览器访问）"; Filename: "{app}\{#ServerExeName}"; Parameters: "--server"; WorkingDir: "{app}"; Check: NoShell
Name: "{userdesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; WorkingDir: "{app}"; Tasks: desktopicon; Check: HasShell

[Run]
Filename: "{app}\{#AppExeName}"; Description: "启动{#AppName}"; Flags: postinstall nowait skipifsilent; Check: HasShell
; 壳依赖 WebView2 Runtime：缺失时在完成页提供官方安装入口（见 [Code] 的注册表检查）。
Filename: "{#WebView2Url}"; Description: "下载并安装 WebView2 Runtime（桌面窗口必需）"; Flags: postinstall shellexec nowait skipifsilent runasoriginaluser; Check: NeedWebView2

[Code]
var
  DataDirPage: TInputDirWizardPage;

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
    MsgBox('写入配置文件失败：' + ConfigFilePath + #13#10 +
           '应用将使用默认数据目录 ' + DefaultDataDir, mbError, MB_OK);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    WriteDataConfig;
    if NeedWebView2 then
      MsgBox('本机未检测到 WebView2 Runtime。' + #13#10#13#10 +
             '桌面窗口依赖该运行时；完成页提供了官方安装入口，装好后重新打开即可。' + #13#10 +
             '也可以直接用开始菜单里的「浏览器访问」入口。', mbInformation, MB_OK);
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
      MsgBox('数据目录已保留：' + DataDir + #13#10#13#10 +
             '卸载不会删除数据库、日志与备份。确认不再需要时，请手动删除该目录。',
             mbInformation, MB_OK);
  end;
end;
