# 小兔进销存 Windows 打包说明

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

- 安装 `xiaotu-server.exe` 与（若已构建）`xiaotu-shell.exe`，两者保持同级；
  附 `config.toml.example` 与面向用户的 `README.md`。
- 开始菜单创建「小兔进销存」；有壳时桌面也创建快捷方式。只有服务端时，开始菜单给
  「浏览器访问」入口（`--server`）。
- 卸载项显示名称与版本；卸载后**保留数据目录并弹窗提示路径**，不自动删除。
- 数据目录页默认 `%LOCALAPPDATA%\com.simem.xiaotu`，允许用户改选；选择非默认目录且
  尚无 `config.toml` 时，安装器在默认目录写入 `[storage] data_dir = "..."`。
  已存在 `config.toml` 时该页锁定不改写，避免覆盖用户配置。
- 安装阶段读注册表检测 WebView2 Runtime（机器级 x64/x86 与用户级）；缺失且装了壳时，
  完成页给出官方安装入口并弹提示，纯浏览器模式不拦。

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

### 未签名包的分发限制

未签名安装包只用于内部验证，**不得对外分发**：

- SmartScreen 会提示「未知发布者」，部分企业策略直接阻止运行；
- 无法证明包未被篡改，用户无法通过数字签名核验来源；
- 正式发布必须等 Authenticode 证书到位并在 CI 配置两个 secret 后重编。

### 尚未验证的边界

- 真实 Authenticode 签名：需要证书，未验。
- 安装 / 升级 / 卸载的真实行为（快捷方式、数据目录页、WebView2 提示、保留数据弹窗）：
  需要在 Windows 机器上实际跑一次，未验；CI 只保证脚本能编出安装包。
- VC++ 运行库：当前按 MSVC 默认动态运行库构建，干净 Windows 机器可能缺
  VC++ 2015-2022 Redistributable；正式发布前需确认是否随包分发或改为静态运行库。

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
