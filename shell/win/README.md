# 小兔进销存 Windows 桌面壳

独立的 Win32 + WebView2 壳：启动时拉起同目录的 `xiaotu-server.exe`（单机形态，仅监听
127.0.0.1），从它的标准输出解析一次性访问地址，用 WebView2 打开；关窗时终止子进程。

本工程**不接入顶层 CMake 构建**，根 `CMakeLists.txt`、`CMakePresets.json`、
`shell/CMakeLists.txt` 均不受影响。

## 前提

- Windows 10/11 x64
- MSVC（VS 2022 Build Tools 或 Community，含 C++ 工具集）
- CMake 3.25+、Ninja
- 目标机器需安装 **WebView2 Runtime**（Win11 自带；Win10 可从
  https://go.microsoft.com/fwlink/p/?LinkId=2124703 安装）

## 构建

在 **Developer PowerShell**（vcvars 已注入，`cl.exe` 在 PATH）里：

```powershell
# 1. 取 WebView2 SDK（nuget 包，幂等；只需一次）
.\shell\win\fetch-webview2.ps1

# 2. 单独构建本工程
cmake -S shell\win -B shell\win\build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build shell\win\build

# 3. 产物
shell\win\build\xiaotu-shell.exe
```

SDK 与构建目录都在 `.cache\`、`build\` 下，已被本目录的 `.gitignore` 排除。

## 运行

```powershell
# 把壳放到服务端程序旁边（分发包目录），或在同一目录下同时存在两个 exe
Copy-Item shell\win\build\xiaotu-shell.exe dist\xiaotu-inventory-1.0.3-win64\
.\dist\xiaotu-inventory-1.0.3-win64\xiaotu-shell.exe
```

- 数据目录：服务端默认位置 `%LOCALAPPDATA%\com.simem.xiaotu`
- WebView2 用户数据：`%LOCALAPPDATA%\com.simem.xiaotu\webview2`
- 若同目录没有 `xiaotu-server.exe`、服务端迟迟没给出访问地址、或未安装 WebView2
  Runtime，壳会弹出明确的错误提示（含服务端最后的输出），不会白屏

## 命令行参数与环境变量

| 项 | 作用 | 优先级 |
| --- | --- | --- |
| `--startup-timeout <秒>` | 等待服务端给出访问地址的上限，1–600 | 最高 |
| `XIAOTU_SHELL_STARTUP_TIMEOUT` | 同上，用环境变量给出（秒） | 次之 |
| 默认值 | 30 秒 | 兜底 |

```powershell
.\xiaotu-shell.exe --startup-timeout 90
$env:XIAOTU_SHELL_STARTUP_TIMEOUT = '60'; .\xiaotu-shell.exe
```

参数非法（非数字、0、超过 600）会弹一次警告并沿用当前值，不会静默忽略。

服务端启动失败时不会等到超时：壳会读子进程退出码，立即提示
「服务端未能启动（退出码 N）」并附上常见原因（数据目录不可写、数据库被占用、
配置文件有误）与服务端最后的输出。

## 单实例

同一登录会话内只允许一个壳：第二个实例会把已运行窗口提到前台（最小化时先还原），
然后以退出码 0 结束，不会启动第二个服务端。

互斥体用 `Local\` 前缀命名，作用域是**当前登录会话**而不是整台机器：多用户同时登录
（含 RDP 会话、快速用户切换）时各用户都有自己的窗口，不会互相挤掉；
**不要**改成 `Global\`。

## 窗口尺寸与位置记忆

- 关窗时把普通状态下的窗口矩形与是否最大化写入
  `%LOCALAPPDATA%\com.simem.xiaotu\shell-window.txt`（格式 `x y w h maximized`）。
- 启动时读回；最大化启动仍会恢复到最大化前的普通尺寸。
- 以下情况自动回退到默认 1280×840 并走系统默认位置，不会开到屏幕外：
  文件不存在、内容无法解析、宽高小于 400×300、矩形与任何显示器工作区
  重叠不足 200×100（例如换过显示器或分辨率变小）。

## 实现要点

| 关注点 | 做法 |
| --- | --- |
| 输出解析 | 只匹配 ASCII 标记 `http://` 与 `/#t=<hex>`，不依赖中文，UTF-8 / GBK 输出都能识别 |
| 编码 | 错误信息展示时才嗅探编码：先按 UTF-8 严格解码，失败回落系统代码页（936） |
| 子进程生命周期 | 子进程加入带 `KILL_ON_JOB_CLOSE` 的 Job 对象；关窗时先等退出、超时再 `TerminateProcess`，壳崩溃也不会留孤儿 |
| 单实例 | `CreateMutexW` + `Local\` 命名互斥体；`FindWindowW` 找到旧窗口后 `ShowWindow` + `SetForegroundWindow` |
| 超时 | `SetTimer` 用可配置毫秒值；子进程退出由读线程检测并带退出码提示 |
| 窗口记忆 | `GetWindowPlacement` 取 `rcNormalPosition` 落盘，`MonitorFromRect` + 工作区交集校验 |
| 窗口缩放 | `WM_SIZE` → `ICoreWebView2Controller::put_Bounds` |
| 首屏 | WebView2 环境初始化与服务端启动并行，拿到地址即 `Navigate` |

## 自动更新桥（`updater_win`）

`updater_win.cpp` / `updater_win.h` 实现与 macOS 同规范的壳内自动更新，**零新增第三方依赖**：

| 环节 | 实现 |
| --- | --- |
| 拉清单 | WinHTTP GET `manifest.json`（Schema v2 统一清单，只取 `platforms.windows`） |
| 验签 | `xiaotu::verify_manifest_signature`（OpenSSL Ed25519，trust anchor 编译期注入） |
| 判定 | 先比 `version`、同版本再比 `build`；通道匹配、灰度分桶、`mandatory` |
| 校验包 | BCrypt 计算 SHA256，与清单逐字节比对 |
| 安装 | `ShellExecute` 跑 Inno Setup `/VERYSILENT /RESTARTAPPLICATIONS`，自动关壳、覆盖、启新壳 |
| 定时 | 启动 30 秒后首查，之后 24h±2h；失败按 1 分钟 / 10 分钟 / 1 小时退避 |
| 持久化 | `%LOCALAPPDATA%\com.simem.xiaotu\updater.json` |
| 握手 | `checkMinSupportedVersion` 读服务端 `/api/health` 的 `min_supported_version` |

对外暴露：`main.cpp` 用 `AddHostObjectToScript("updater_win", ...)` 把 `UpdaterWin` 挂到 `window.chrome.webview.hostObjects.updater_win`，可用 `checkNow()` / `downloadUpdate()` / `installUpdate()` / `cancelPrefetch()` / `setOptions(auto, prefetch)` / `statusJson()`。

**当前状态**（2026-11-21 核对）：`web/app.js` 已接入 `chrome.webview.hostObjects.updater_win` 前端卡片；1.0.6 已在 12600K 完成真机验收（检查 → 下载 → ISCC 静默安装 → 重启 → 版本核对，结论见 [发行与安装说明.md](../../docs/发行与安装说明.md) §1/§4）。安装包仍未签名，首次运行需手动放行 SmartScreen；正式分发仍以客户从下载页获取安装包为主。
