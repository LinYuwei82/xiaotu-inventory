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
Copy-Item shell\win\build\xiaotu-shell.exe build\package\win\xiaotu-inventory-0.1.0-win64\
.\build\package\win\xiaotu-inventory-0.1.0-win64\xiaotu-shell.exe
```

- 数据目录：服务端默认位置 `%LOCALAPPDATA%\com.simem.xiaotu`
- WebView2 用户数据：`%LOCALAPPDATA%\com.simem.xiaotu\webview2`
- 若同目录没有 `xiaotu-server.exe`、服务端 30 秒内没给出访问地址、或未安装 WebView2
  Runtime，壳会弹出明确的错误提示（含服务端最后的输出），不会白屏

## 实现要点

| 关注点 | 做法 |
| --- | --- |
| 输出解析 | 只匹配 ASCII 标记 `http://` 与 `/#t=<hex>`，不依赖中文，UTF-8 / GBK 输出都能识别 |
| 编码 | 错误信息展示时才嗅探编码：先按 UTF-8 严格解码，失败回落系统代码页（936） |
| 子进程生命周期 | 子进程加入带 `KILL_ON_JOB_CLOSE` 的 Job 对象；关窗时先等退出、超时再 `TerminateProcess`，壳崩溃也不会留孤儿 |
| 窗口缩放 | `WM_SIZE` → `ICoreWebView2Controller::put_Bounds` |
| 首屏 | WebView2 环境初始化与服务端启动并行，拿到地址即 `Navigate` |
