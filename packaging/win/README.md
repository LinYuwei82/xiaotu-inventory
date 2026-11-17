# 小兔进销存 Windows 打包说明

发布者：个人开发者 `si_mem`；版权：`Copyright © 2026 si_mem`。

本目录放 Windows 侧的分发产物构建脚本，不参与顶层 CMake 构建：

- `xiaotu-inventory.iss`：Inno Setup 6 安装包脚本（安装、快捷方式、卸载、数据目录、WebView2 检查）。
- `make-zip.ps1`：从现有 `windows-release` 产物（`build\windows\server\xiaotu-server.exe`）生成绿色 zip。

两者都读取同一批产物：服务端 exe 为必需，桌面壳按同一套顺序解析：显式指定 → `build\windows\shell\win\xiaotu-shell.exe` → `build\windows\shell\win\Release\xiaotu-shell.exe` → 旧的 `shell\win\build\xiaotu-shell.exe` → `shell\win\build\Release\xiaotu-shell.exe`。**都找不到时脚本报错退出/编译失败**，不会静默产出无壳包；确实需要无壳包时显式加 `-NoShell` / `/DXIAOTU_NO_SHELL=1`。

`tools\win\verify.ps1` 的「安装包含桌面壳」与 `/MT` 检查使用**同一套候选顺序**（也可用 `-ShellExe` 显式指定，语义同 `/DShellExe`），因此旧目录布局与顶层构建布局都能验到壳，不需要改包或放宽断言。

## 前置

```powershell
# 服务端（必需）
cmake --preset windows-release
cmake --build --preset windows-release

# 桌面壳（可选，需要时再构建；见 shell/win/README.md）
.\shell\win\fetch-webview2.ps1
cmake -S shell\win -B shell\win\build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build shell\win\build
```

两条 `cmake --build` 都需在 Developer PowerShell（`cl.exe` 在 PATH）里执行。

## Inno Setup 安装包

### 本地编译

在 Windows + Inno Setup 6 环境，先构建服务端与壳（见上文「前置」），然后：

```powershell
$version = (Get-Content VERSION -Raw).Trim()
& "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe" `
  "/DAppVersion=$version" `
  "packaging\win\xiaotu-inventory.iss"
```

壳未按候选路径找到时编译会直接失败并列出查找过的路径；确实要出纯服务端安装包时显式加
`/DXIAOTU_NO_SHELL=1`，也可用 `/DShellExe=<绝对路径>` 指定壳（指定后必须存在）。

产物：`packaging\win\out\xiaotu-inventory-<版本>-win64-setup.exe`。

安装包行为：

- **当前用户安装**：`PrivilegesRequired=lowest`，不请求管理员权限，默认装到
  `%LOCALAPPDATA%\Programs\小兔进销存`；开始菜单与桌面快捷方式也写当前用户配置。
- 安装 `xiaotu-server.exe` 与（若已构建）`xiaotu-shell.exe`，两者保持同级；
  附 `config.toml.example` 与面向用户的 `README.md`。
- 开始菜单创建「小兔进销存」；有壳时桌面也创建快捷方式。只有服务端时，开始菜单给
  「浏览器访问」入口（`--server`）。
- 卸载项显示名称与版本；卸载后**保留数据目录并弹窗提示路径**，不自动删除。
- 数据目录页默认 `%LOCALAPPDATA%\com.simem.xiaotu`，允许用户改选；因为不再提权，
  该路径始终属于当前用户，不会被管理员账户影响。选择非默认目录且尚无 `config.toml`
  时，安装器在默认目录写入 `[storage] data_dir = "..."`；已存在 `config.toml` 时
  该页锁定不改写，避免覆盖用户配置。
- 安装阶段读注册表检测 WebView2 Runtime（机器级 x64/x86 与用户级）；缺失且装了壳时，
  完成页给出官方安装入口并弹提示，纯浏览器模式不拦。
- 服务端与壳均以静态运行库（`/MT`，Debug 为 `/MTd`）构建，**不需要安装
  VC++ 2015-2022 Redistributable**。

### CI 产物

`.github/workflows/windows-build.yml` 在 `windows-latest` 上：

1. 编译服务端、跑 ctest、编译 Windows 壳；
2. `choco install innosetup`，调用 `ISCC.exe` 编出安装包；
3. 以 artifact `xiaotu-win-installer` 上传 `*.exe` 与 `SIGNING.txt`。

`SIGNING.txt` 会明确写「已签名」或「未签名：未配置 WINDOWS_CERT_PFX_BASE64」，
不会把未签名产物当成已签名。

### 代码签名

签名分两步，顺序不能反：**先签包内 exe，再打安装包**。Inno 只签安装包与卸载器，
不会回签已复制的 exe；先打包后签 exe 会得到「安装包已签、包内 exe 未签」的混合状态。

本机（PFX）：

```powershell
$env:XIAOTU_CERT_PASSWORD = '换成口令'   # 只放当前会话，不进仓库

# 1. 签包内 exe（壳按实际路径改）
.\packaging\win\sign.ps1 -PfxPath D:\certs\xiaotu.pfx `
  -Files build\windows\server\xiaotu-server.exe,shell\win\build\xiaotu-shell.exe

# 2. 打包；ISCC 用同一份证书签安装包与卸载器
$version = (Get-Content VERSION -Raw).Trim()
& "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe" "/DAppVersion=$version" /DSign=1 `
  '/Ssigntool=C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe sign /fd SHA256 /f D:\certs\xiaotu.pfx /p <口令> /tr http://timestamp.digicert.com /td SHA256 $f' `
  packaging\win\xiaotu-inventory.iss

# 3. 复验安装包并写 out\SIGNING.txt
.\packaging\win\sign.ps1 -PfxPath D:\certs\xiaotu.pfx
```

证书在 USB token / HSM 或已导入证书存储时，用指纹签名，不必在命令行传口令：

```powershell
.\packaging\win\sign.ps1 -Thumbprint <证书指纹>        # 当前用户存储
.\packaging\win\sign.ps1 -Thumbprint <指纹> -MachineStore
```

`sign.ps1` 对每个文件执行 `signtool verify /pa /v`；任一失败即以非 0 退出，并在
`out\SIGNING.txt` 写「签名失败」，不会写成「已签名」。

CI（`.github/workflows/windows-build.yml`）检测到 secret 时先用 `sign.ps1` 签两个 exe，
再由 ISCC 签安装包与卸载器，最后对安装包跑 `verify /pa /v`：

| 名称 | 类型 | 用途 |
| --- | --- | --- |
| `WINDOWS_CERT_PFX_BASE64` | secret | PFX 的 Base64；存在时才签名 |
| `WINDOWS_CERT_PFX_PASSWORD` | secret | PFX 口令 |

**证书怎么选**

- **OV**：消除「未知发布者」；SmartScreen 信誉需随下载量累积。个人可申请，需身份证明，
  部分 CA 还要求营业执照。
- **EV**：通常立即获得 SmartScreen 信任，成本更高，私钥在硬件 token / 云 HSM。
- **Azure Trusted Signing**：按月订阅、云端签名，适合 CI；需要 Azure 订阅并通过组织验证，
  个人开发者是否可申请以官方为准。
- 2023 年起 CA 通常不再交付可导出的裸 PFX，私钥须存放在硬件或云 HSM。证书以 token 形式交付时
  用 `-Thumbprint`；CI 无法插 token，应改用 CA 的云签名服务或 Azure Trusted Signing。
  上面两个 PFX secret 只适用于仍提供 PFX 的证书。

### Certum 云签名（候选方案，当前暂不购买）

2026-11-17 决定：Windows 端先不买证书，继续以未签名安装包分发给早期客户；以下流程保留，
后续启用时按此执行。

产品：**Certum Standard Code Signing in the Cloud**（SimplySign 云签名，OV）。个人可申请，
材料为身份证/护照 + 手机话费发票；证书主体是个人法定姓名（如 `Yuwei Lin`），不是 `si_mem`。

1. 在 Certum 官方店或授权服务商（如 SSLDUN）下单 1 年或 3 年。2026-02-27 起单张证书最多
   459 天，2/3 年期会在到期前免费 reissue，不是一张证书用满三年。
2. 提交身份与地址材料，完成 CA 的电话/视频核验。
3. 激活 SimplySign 账号（邮箱 + 手机 2FA），安装 SimplySign Desktop 并登录；证书会出现在
   Windows「个人」证书存储。
4. 取指纹：
   ```powershell
   Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert |
     Select-Object Subject, Thumbprint, NotAfter
   ```
5. 用指纹签名（SimplySign 会要求确认/2FA）：
   ```powershell
   .\packaging\win\sign.ps1 -Thumbprint <指纹> `
     -Files build\windows\server\xiaotu-server.exe,shell\win\build\xiaotu-shell.exe
   ```

**CI 限制**：SimplySign 依赖桌面虚拟卡与登录会话，GitHub 托管 runner 用不了。可选：
- 本地或自托管 Windows runner 上打包并签名（当前本机流程即可）；
- 改用带 API 的云签名（如 SSL.com eSigner），再改 CI；
- 现有 `WINDOWS_CERT_PFX_BASE64` / `WINDOWS_CERT_PFX_PASSWORD` 对 Certum 云证书不适用。

### 未签名包的分发方式

当前决定暂不购买代码签名证书，未签名包分发给早期客户时：

- **发版者 Checklist**：逐条核对 `docs/未签名分发注意事项.md` 的第二部分（含干净机器实测、SHA256 记录、客户说明随包发出）。
- **给客户的完整安装指引**：见 `docs/客户安装说明.md`，直接随包发出。
- 用户首次运行会看到 SmartScreen「未知发布者」，点「更多信息 → 仍要运行」即可；分发时必须同时给出安装包 SHA256，便于核对包未被篡改。
- 拿到证书后按上文 Certum 云签名流程完成签名并重编，即可去掉该提示；同时更新上述两份文档。

### 尚未验证的边界

- 真实 Authenticode 签名：脚本与 CI 已接入；当前决定暂不购买证书，启用后再实签验证
  （本地产物仍为未签名，`SIGNING.txt` 已注明）。
- 保留数据弹窗与「仅卸载不删数据」的交互提示：静默卸载已验「数据目录与 `app.db` 保留」，
  但交互模式下的提示文案与按钮仍需人工看一眼。
- WebView2 缺失提示、窗口位置记忆、打印对话框、SmartScreen 表现：见
  `tools/win/README.md` 的 MANUAL 项。

已在 Windows 实机（`12600k`，Inno Setup 6.7.3）验过的部分：

- 静默安装 → 同版本覆盖安装 → 静默卸载整条链路，21 项断言全 PASS
  （脚本 `tools/win/verify.ps1`，含签名状态、SHA256、安装路径、开始菜单/桌面快捷方式、
  数据目录、单机版启动与接口、业务冒烟、停止服务、覆盖安装后 `app.db` 哈希一致、
  卸载后数据保留）。
- 静态运行库（`/MT`）：服务端与壳的产物都不导入 `vcruntime140.dll` / `msvcp140.dll`。

## 两种打包模式

| 模式 | 触发条件 | 包内是否有桌面窗口 |
| --- | --- | --- |
| 桌面壳 + 服务端（默认） | 按候选顺序找到壳，且未加 `-NoShell` | 有，双击 `xiaotu-shell.exe` 即用 |
| 仅服务端 | 显式 `-NoShell` / `/DXIAOTU_NO_SHELL=1` | 无，只能浏览器访问 |

```powershell
.\packaging\win\make-zip.ps1              # 自动查找壳；找不到则报错
.\packaging\win\make-zip.ps1 -NoShell     # 显式只打服务端
.\packaging\win\make-zip.ps1 -NoArchive   # 只生成目录，便于本地试跑
```

可选参数：

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `-BuildDir` | `<仓库>\build\windows` | 服务端构建目录 |
| `-OutDir` | `<仓库>\build\package\win` | 输出目录 |
| `-Name` | `xiaotu-inventory-<版本>-win64` | 分发包目录名（zip 同名） |
| `-ShellExe` | 自动查找 | 显式指定桌面壳；指定后必须存在，不回退 |
| `-NoShell` | 关 | 显式跳过桌面壳（与 `-ShellExe` 互斥） |
| `-NoArchive` | 关 | 不压缩，只生成解压后的目录 |

版本号从根 `CMakeLists.txt` 的 `project(xiaotu_inventory VERSION ...)` 读取，脚本不修改任何文件。

## 包内结构（含壳）

```
xiaotu-inventory-<版本>-win64\
├── xiaotu-shell.exe         桌面窗口（双击即用，自动拉起同目录服务端）
├── xiaotu-server.exe        服务端，前端资源已内嵌
├── config.toml.example      配置样例
├── README.md                终端用户说明（含 WebView2 检查与安装地址）
└── logs\                    运行日志目录
```

仅服务端模式只少 `xiaotu-shell.exe`，`README.md` 顶部会多一段"本包不含桌面窗口"的提示。

两个 exe 必须同级：壳按自身所在目录查找 `xiaotu-server.exe`。脚本已保证这一点，
手工调整包结构时不要把它们分到子目录。

## WebView2 依赖怎么处理

- **只检查，不打包**：脚本读取注册表判断打包机是否装了 WebView2 Runtime 并打印版本，
  39 MB 的 SDK 缓存（`shell\win\.cache\`）不入包。
- 壳在目标机器启动时若发现没有 Runtime，会弹出提示并给出下载地址；包内 `README.md`
  也写了一条可直接粘贴的注册表检测命令与官方下载链接
  （https://go.microsoft.com/fwlink/p/?LinkId=2124703）。

## 单机与网络使用方式

两种用法共用同一份服务端和同一种数据目录格式，区别只在「谁监听、谁来访问」。

| | 单机 | 网络版 |
| --- | --- | --- |
| 启动 | 双击 `xiaotu-shell.exe`（或直接跑服务端） | 只跑 `xiaotu-server.exe --server` |
| 监听 | `127.0.0.1` + 系统分配的自动端口 | `0.0.0.0:8080`（可改） |
| 访问 | 本机 WebView 窗口 / 本机浏览器 | 局域网内其他机器的浏览器 |
| 数据 | `%LOCALAPPDATA%\com.simem.xiaotu`（可用 `--data` 改） | 同左，建议显式 `--data` 到固定磁盘 |

### 可直接复制的启动命令

含壳的安装包（单机，推荐）：

```powershell
& "$env:LOCALAPPDATA\Programs\小兔进销存\xiaotu-shell.exe"
```

壳不解析数据目录/端口参数：它在自身目录找 `xiaotu-server.exe`，以**无参数**方式拉起
（单机模式），工作目录与数据目录都按壳的位置和环境变量决定。

只用服务端 + 本机浏览器（单机）：

```powershell
.\xiaotu-server.exe --data D:\xiaotu-data
```

启动后打印 `服务地址: http://127.0.0.1:<端口>/#t=<一次性令牌>`，把该地址贴进浏览器即可；
令牌只用一次，刷新后按正常登录流程走。

网络版（一台机器做服务器，其他人用浏览器访问）：

```powershell
# 首次：给内置 admin 设口令（至少 8 位；走环境变量，不出现在命令行历史里）
$env:XIAOTU_PASSWORD = '换成你的口令'
.\xiaotu-server.exe --data D:\xiaotu-data --set-password admin

# 启动
.\xiaotu-server.exe --server --listen 0.0.0.0 --port 8080 --data D:\xiaotu-data
```

### 命令行参数（`--help` 实测）

| 参数 | 说明 | 默认 |
| --- | --- | --- |
| （无参数） | 单机模式，只监听回环 | `127.0.0.1:0`（自动端口） |
| `--server` | 网络版 | 关 |
| `--listen <地址>` | 网络版监听地址 | `0.0.0.0` |
| `--port <端口>` | 网络版端口 | `8080` |
| `--data <目录>` | 数据目录 | 系统应用数据目录（Windows 为 `%LOCALAPPDATA%\com.simem.xiaotu`） |
| `--config <文件>` | 配置文件 | `$XIAOTU_CONFIG` → `<数据目录>/config.toml` |
| `--backup` / `--backup-daily` / `--restore <文件>` | 立即备份 / 每日备份 / 恢复 | — |
| `--set-password <用户名>` | 用 `XIAOTU_PASSWORD` 设口令后退出 | — |
| `--version` / `--help` | 版本号 / 帮助 | — |

优先级：命令行 > 环境变量 > `config.toml` > 内置默认。

### 壳是怎么拉起服务端的

- 壳在自己的目录找 `xiaotu-server.exe`，`CreateProcessW` **只传 exe 路径**（无参数），
  工作目录为壳目录，环境变量原样继承（`XIAOTU_CONFIG`、`LOCALAPPDATA` 对子进程有效）。
- 端口不写死：服务端以 `127.0.0.1:0` 绑定，由系统分配空闲端口；壳从子进程 stdout 读到
  `服务地址:` 后再导航，所以不会撞固定端口。
- 关窗即收摊：`WM_DESTROY` 里先保存窗口位置，再关闭 Job 对象
  （`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`），子进程随之结束，另有 `TerminateProcess` 兜底；
  壳被任务管理器强杀时，Job 句柄关闭同样会带走服务端。
- 同一数据目录同时只允许一个服务端（`app.lock` 单实例互斥），重复启动会直接退出。

### 别的机器怎么连网络版

1. 在服务器机器上用**管理员** PowerShell 放行端口（按实际端口与网络位置调整）：

```powershell
netsh advfirewall firewall add rule name="小兔进销存 8080" dir=in action=allow protocol=TCP localport=8080 profile=private
```

   删除规则：

```powershell
netsh advfirewall firewall delete rule name="小兔进销存 8080"
```

2. `ipconfig` 查本机局域网 IP，确认客户端与服务器同网段；
3. 客户端浏览器打开 `http://<服务器IP>:8080/`；
4. 用用户名 + 口令登录。

### 登录、令牌与首次改密

- 初始账号：迁移种子里的 `admin`（姓名「管理员」），**口令为空**，必须先用
  `--set-password admin` 设一次，否则网页登录不了。
- `--set-password` 会把 `must_change_password` 置 1：首次登录后，除「改密 / 退出 / 查看自己」
  之外接口一律返回 `403 MUST_CHANGE_PASSWORD`，改完口令才放行（本机实测）。
- 登录接口 `POST /api/login {"username","password"}`，返回
  `Set-Cookie: sid=…; HttpOnly; SameSite=Strict; Max-Age=1800`（30 分钟空闲超时，
  可用 `[auth] idle_minutes` 调整），响应体带 `csrf`。
- 之后的写操作（POST/PUT/PATCH/DELETE）必须带 `X-CSRF-Token: <csrf>`，只带 Cookie 会得到
  `403 CSRF_INVALID`；GET 不需要。
- Cookie 的 `Secure` 仅在反向代理送来 `X-Forwarded-Proto: https` 时添加；服务端自身不带 TLS，
  需要 HTTPS 请放在反向代理后面。

### 注意事项

- **端口冲突**：绑定失败会启动失败并打印 `无法监听 <地址>:<端口>`；换 `--port`，或先查占用：
  `Get-NetTCPConnection -LocalPort 8080`。
- **数据目录权限**：运行账号必须能读写该目录。默认目录跟随 `LOCALAPPDATA`（按登录用户），
  换账号或用服务方式运行时路径会变，建议显式 `--data` 并单独授权。
- **不要放共享盘/同步盘**：代码只对 iCloud 与 OneDrive 目录告警（只告警不拦截）；
  SMB 网络共享没有专门检测，但 SQLite 放网络文件系统上容易损坏，同样不要用。
- **备份**：默认在 `<数据目录>\backups`，可用 `config.toml` 的 `[storage] backup_dir` 改
  （相对路径按数据目录解析）；`--backup` 立即备份一次，`--backup-daily` 供计划任务调用。
- **网络版是版本能力**：`--server` 会先检查 `deployment.network`，默认 full 版本可用；
  被隐藏的版本打印 `FEATURE_UNAVAILABLE` 并以退出码 2 结束，不会悄悄退回单机。

## 边界

- 不改动 `CMakeLists.txt`、`CMakePresets.json`、`shell/CMakeLists.txt`、`README.md` 等共享文件。
- 不修改 `core/`、`server/`、`web/`、`tests/`、`docs/`，也不碰生成物。
- 两个 `.ps1` 与 `.iss` 均以 **UTF-8 BOM** 保存：PowerShell 5.1 会按系统代码页（936）读取
  无 BOM 脚本，中文会变成乱码并导致解析失败；Inno Setup 也按 BOM 判断脚本编码。
  用编辑器改写这些文件时请保留 BOM。
