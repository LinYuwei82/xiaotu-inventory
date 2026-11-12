# 小兔进销存 Windows 打包说明

本目录只放 Windows 侧的分发产物构建脚本，不参与顶层 CMake 构建：脚本从现有
`windows-release` 产物（`build\windows\server\xiaotu-server.exe`）取二进制，配合仓库里的
`deploy\config.toml.example` 打成可分发的 zip。

## 生成安装包

前置：先按 README 第 2 节构建 Windows 服务端（需要 Developer PowerShell，`cl.exe` 在 PATH 里）。

```powershell
cmake --preset windows-release
cmake --build --preset windows-release

# 打包
.\packaging\win\make-zip.ps1
```

默认输出 `build\package\win\xiaotu-inventory-<版本>-win64.zip`，命令行会打印路径、大小与 SHA256。

可选参数：

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `-BuildDir` | `<仓库>\build\windows` | CMake 构建目录 |
| `-OutDir` | `<仓库>\build\package\win` | 输出目录 |
| `-Name` | `xiaotu-inventory-<版本>-win64` | 分发包目录名（zip 同名） |
| `-NoArchive` | 关 | 只生成解压后的目录，不打 zip（便于本地快速试用） |

版本号从根 `CMakeLists.txt` 的 `project(xiaotu_inventory VERSION ...)` 读取，脚本不修改任何文件。

## 分发包内容

```
xiaotu-inventory-<版本>-win64\
├── xiaotu-server.exe        服务端（前端资源已内嵌，无外部静态文件依赖）
├── config.toml.example      配置样例，复制为 config.toml 后按需修改
├── README.md                本说明的新增文件副本（用法、端口、数据目录）
└── logs\                    运行日志目录（为空，首次启动写入 app.log）
```

`xiaotu-server.exe` 自带 Web 前端（`/api/health` 返回的 `embedded_resources` 即内嵌资源数），
因此分发包不需要 `web/` 目录。

## 目标机器上的用法

```powershell
# 1. 解压后进入目录，首次先给内置管理员设置口令（8–128 位）
$env:XIAOTU_PASSWORD = '你的口令'
.\xiaotu-server.exe --data .\data --set-password admin

# 2. 启动网络版；浏览器打开 http://localhost:8080/ 用 admin 登录
.\xiaotu-server.exe --server --port 8080 --data .\data

# 可选：使用配置文件（放在数据目录下，或用 XIAOTU_CONFIG 指定）
Copy-Item .\config.toml.example .\data\config.toml
```

- 数据目录默认是 `%LOCALAPPDATA%\com.simem.xiaotu`；分发包里显式传 `--data` 可以让数据跟着
  程序目录走，卸载时删目录即可。
- 服务端只监听你给的端口；要让局域网其他机器访问，用本机 IP 打开，并确保防火墙放行该端口。
- 当前版本没有 Windows 桌面壳（WebView2 壳见 `shell\win\`），所以用浏览器访问。

## 边界

- 不改动 `CMakeLists.txt`、`CMakePresets.json`、`shell/CMakeLists.txt`、`README.md` 等共享文件。
- 不修改 `core/`、`server/`、`web/`、`tests/`、`docs/`，也不碰生成物。
