# 小兔进销存 Windows 打包说明

发布者：个人开发者 `si_mem`；版权：`Copyright © 2026 si_mem`。

本目录放 Windows 侧的分发产物构建脚本，不参与顶层 CMake 构建：

- `xiaotu-inventory.iss`：Inno Setup 6 安装包脚本（安装、快捷方式、卸载、数据目录、WebView2 检查）。
- `make-zip.ps1`：从现有 `windows-release` 产物（`build\windows\server\xiaotu-server.exe`）生成绿色 zip。

两者都读取同一批产物：服务端 exe 为必需，桌面壳 `shell\win\build\xiaotu-shell.exe` 存在时一并打入。

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

### 签名（参数化空位）

| 名称 | 类型 | 用途 |
| --- | --- | --- |
| `WINDOWS_CERT_PFX_BASE64` | secret | PFX 证书的 Base64；存在时才签名 |
| `WINDOWS_CERT_PFX_PASSWORD` | secret | PFX 口令 |

两个 secret 都存在时，CI 用 Windows SDK 的 `signtool.exe` 以 SHA256 + RFC3161 时间戳
（`http://timestamp.digicert.com`）签名安装包与卸载器；任一缺失则跳过签名并打印「未签名」。
证书配置了但签名失败时，CI 直接失败，不会产出看似正常的包。

### 未签名包的分发方式

当前已定：**暂不购买证书，未签名包可分发给早期客户**，按手工方式发送，并随包附上首次放行说明：

- 用户首次运行会看到 SmartScreen「未知发布者」，点「更多信息 → 仍要运行」即可；
- 分发时给出 SHA256，便于客户核对包未被篡改；
- 完整说明见 [`docs/发行与安装说明.md`](../../docs/发行与安装说明.md)；
- 购买 OV 证书并配置两个 secret 后重编，即可去掉该提示。

### 尚未验证的边界

- 真实 Authenticode 签名：需要证书，未验（本地产物为未签名，`SIGNING.txt` 已注明）。
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
| 桌面壳 + 服务端（默认） | `shell\win\build\xiaotu-shell.exe` 存在且未加 `-NoShell` | 有，双击 `xiaotu-shell.exe` 即用 |
| 仅服务端 | 壳未构建，或显式 `-NoShell` | 无，只能浏览器访问 |

```powershell
.\packaging\win\make-zip.ps1              # 有壳就打壳，没有只打服务端并提示
.\packaging\win\make-zip.ps1 -NoShell     # 强制只打服务端
.\packaging\win\make-zip.ps1 -NoArchive   # 只生成目录，便于本地试跑
```

可选参数：

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `-BuildDir` | `<仓库>\build\windows` | 服务端构建目录 |
| `-OutDir` | `<仓库>\build\package\win` | 输出目录 |
| `-Name` | `xiaotu-inventory-<版本>-win64` | 分发包目录名（zip 同名） |
| `-ShellExe` | `<仓库>\shell\win\build\xiaotu-shell.exe` | 桌面壳路径，便于从别的构建目录取 |
| `-NoShell` | 关 | 跳过桌面壳 |
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

## 目标机器上的用法

含壳的包：解压后双击 `xiaotu-shell.exe` 即可，数据写在
`%LOCALAPPDATA%\com.simem.xiaotu`。

仅服务端的包：

```powershell
$env:XIAOTU_PASSWORD = '你的口令'
.\xiaotu-server.exe --set-password admin
.\xiaotu-server.exe --server --port 8080      # 浏览器打开 http://localhost:8080/
```

## 边界

- 不改动 `CMakeLists.txt`、`CMakePresets.json`、`shell/CMakeLists.txt`、`README.md` 等共享文件。
- 不修改 `core/`、`server/`、`web/`、`tests/`、`docs/`，也不碰生成物。
- 两个 `.ps1` 与 `.iss` 均以 **UTF-8 BOM** 保存：PowerShell 5.1 会按系统代码页（936）读取
  无 BOM 脚本，中文会变成乱码并导致解析失败；Inno Setup 也按 BOM 判断脚本编码。
  用编辑器改写这些文件时请保留 BOM。
