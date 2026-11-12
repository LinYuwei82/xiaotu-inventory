# 小兔进销存 Windows 打包说明

本目录只放 Windows 侧的分发产物构建脚本，不参与顶层 CMake 构建：脚本从现有
`windows-release` 产物（`build\windows\server\xiaotu-server.exe`）取二进制，若桌面壳
`shell\win\build\xiaotu-shell.exe` 已构建则一并打入，配合 `deploy\config.toml.example`
与面向终端用户的 `package-readme.md` 打成可分发的 zip。

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
- 两个 `.ps1` 均以 **UTF-8 BOM** 保存：PowerShell 5.1 会按系统代码页（936）读取无 BOM 脚本，
  中文会变成乱码并导致解析失败。用编辑器改写这两个文件时请保留 BOM。
