# Windows 实机验收脚本

`verify.ps1` 在真实 Windows 机器上跑出 PASS / FAIL / SKIP / MANUAL，不需要人工逐条敲命令。PowerShell 5.1 与 7 均可运行。

## 用法

在 VS 开发者 PowerShell（保证 `dumpbin` 在 PATH）中：

```powershell
# 最简：仓库、安装包都给绝对路径
.\tools\win\verify.ps1 `
  -Repo C:\src\xiaotu-inventory `
  -Installer C:\Users\me\Downloads\xiaotu-inventory-1.0.3-win64-setup.exe `
  -DataDir C:\Users\me\AppData\Local\com.simem.xiaotu `
  -ExpectedVersion 1.0.3
```

常用参数：

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `-Repo` | `C:\src\xiaotu-inventory` | 仓库根，必须含 `.git` |
| `-Installer` | 自动在 `dist` 与 `%TEMP%\xiaotu-win-installer` 找 | 安装包路径 |
| `-ExpectedVersion` | `1.0.3` | 用于校验 `/api/health` 的版本号 |
| `-DataDir` | `%LOCALAPPDATA%\com.simem.xiaotu` | 测试用数据目录；**不要指向客户真实数据** |
| `-Port` | `18080` | 保留参数，单机版实际用随机端口 |
| `-LogDir` | `%TEMP%\xiaotu-verify-<时间>` | 日志、安装日志、备份 |
| `-SkipBuild` | 关 | 跳过 cmake/ctest |
| `-SkipInstall` | 关 | 跳过安装、卸载与依赖安装的检查 |
| `-AllowExistingData` | 关 | 数据目录已有 `app.db` 时允许继续，会先备份到 `-LogDir\data-backup` |
| `-ShellExe` | 空（自动解析） | 显式指定桌面壳路径，语义同 iss 的 `/DShellExe`：给了就必须存在，不回退；不传则按 iss 的候选顺序解析 |
| `-Yes` | 关 | 安装前不再等待回车确认 |

## 安全约定

- 非 Windows 直接退出码 2，不执行任何操作。
- `-DataDir` 已有 `app.db` 且未加 `-AllowExistingData` 时**中止**，避免把测试跑在真实数据上。
- 安装与卸载前会提示将写入 `%LOCALAPPDATA%`；不加 `-Yes` 时等待回车。
- 所有临时目录用唯一名字，结束时清理；C7560 检查在 `git worktree` 副本里做，不污染主仓库。
- 任何步骤的失败输出都写入 `-LogDir` 下的对应日志。

## 脚本编码约定

- 本目录及仓库内的 PowerShell 脚本统一保存为 **UTF-8 with BOM**。PowerShell 5.1 会把无 BOM 的 UTF-8 按当前 ANSI 代码页读取（简体中文为 936），中文注释和中文输出会变成乱码，甚至直接语法解析失败、脚本起不来。
- 编辑脚本时保留开头的 BOM：保存为「UTF-8 with BOM」，或确认文件前三字节是 `EF BB BF`。不要改成「UTF-8 无 BOM」。
- 当前已带 BOM 的脚本：`tools/win/verify.ps1`、`tools/win/profile-build.ps1`、`packaging/win/make-zip.ps1`、`shell/win/fetch-webview2.ps1`。新增脚本沿用同一约定。

## 退出码

| 退出码 | 含义 |
| --- | --- |
| 0 | 没有 FAIL（可以有 SKIP / MANUAL） |
| 1 | 至少一条 FAIL |
| 2 | 环境不满足（非 Windows、仓库路径错、数据目录保护中止） |

## 每条检查的含义

| 检查 | 通过标准 |
| --- | --- |
| MSVC 构建 | `cmake --preset windows-release` 与构建均返回 0 |
| 编译参数不含 /we7560 | `build/windows/build.ninja` 里搜不到 `/we7560`（根 `CMakeLists.txt` 已移除该开关） |
| ctest | `ctest --preset windows-release` 返回 0；失败时列出失败用例名 |
| 静态运行库 /MT | `dumpbin /dependents` 不含 `vcruntime140.dll`、`msvcp140.dll`、`ucrtbased.dll`；找不到 dumpbin 记 SKIP |
| C7560 生效 | 临时 worktree 的 `document.cpp` 追加错序指定初始化器后编译失败，且输出含 `C7560`；随后主仓库 `document.cpp` 无改动。判定依据是「C7560 被当作错误」，**不是**「`/we7560` 被识别」：本机 MSVC 14.44 对 `/we7560` 报 `D9014`（值无效），而 C7560 默认就是 error，所以该开关是否被接受不在本项验证范围内 |
| 安装包签名状态 | `SIGNING.txt` 明确写「未签名」且不写「已签名」 |
| 安装包 SHA256 | 记录 `Get-FileHash` 结果 |
| 安装包含桌面壳 | 按 `packaging/win/xiaotu-inventory.iss` 的同一套候选顺序解析源壳（`-ShellExe` → `build\windows\shell\win\` → `...\Release\` → 旧 `shell\win\build\` → `...\Release\`），记录命中路径与时间戳；显式 `-ShellExe` 给了但不存在、或四处都没有即 FAIL（后者说明本次包会静默漏壳，后续安装结论不作数） |
| 安装前无残留数据 | 安装目录无 `app.db`；`-DataDir` 起跑前为空（加 `-AllowExistingData` 时只检查安装目录，并注明前提） |
| 静默安装 | 安装器退出码 0。Inno 语义：0 成功、1 用户取消、2 致命错误；静默安装出现 1 视为 FAIL |
| 安装路径 | `%LOCALAPPDATA%\Programs\小兔进销存\xiaotu-server.exe` 存在，`%ProgramFiles%` 下无同名目录 |
| 安装目录含桌面壳 | 安装目录里确实有 `xiaotu-shell.exe`；缺失即 FAIL 并标注「包不完整」 |
| 开始菜单快捷方式 | 开始菜单递归能找到「小兔进销存」快捷方式 |
| 桌面快捷方式 | 有壳时存在，无壳时不存在 |
| 数据目录位置 | `-DataDir` 存在 |
| 单机版启动与接口 | 20 秒内读到服务地址；token 换会话 200；`/api/health` 200 且 version 等于 `-ExpectedVersion`；`/api/me` 200；且 `app.db` 落在 `-DataDir`（数据没有落到别处） |
| 业务冒烟 | 建商品、建客户、采购入库草稿、审核、查库存余额全部 200，且余额含验收商品。写操作带 `/api/me` 返回的 `csrf`（`X-CSRF-Token`），否则服务端 403 |
| 停止服务 | 杀进程后进程不再存在 |
| 同版本覆盖安装 | 再跑一次安装器返回 0；`app.db` 仍在且 SHA256 前后一致（数据未被覆盖安装改动）；程序目录无 `.old` / `.tmp` 残留 |
| 静默卸载 | 卸载器返回 0（返回 1 即用户取消，同样视为 FAIL）；`xiaotu-server.exe` 消失；数据目录与 `app.db` 仍存在；开始菜单快捷方式消失 |

## C7560 与 `/we7560`（不要再加回该开关）

C7560 指「指定初始化器（designated initializer）顺序与结构体成员声明顺序不一致」。根 `CMakeLists.txt` 曾经写过 `add_compile_options(/we7560)` 想把它升级为错误，实际不必要且有害。

本机实测环境：`cl` 版本 **19.44.35230**，工具集 **14.44.35207**（VS 2022 BuildTools），`/std:c++20`。四组对照（源码分别是一份错序 initialized 的 `struct P { int a; int b; }; P p{ .b = 2, .a = 1 };` 与一份空 `main`）：

| 源码 | 编译选项 | 输出 | 退出码 |
| --- | --- | --- | --- |
| 错序 | `/we7560` | `warning D9014 : 值"7560"对于"/we"无效` + `error C7560` | 2 |
| 错序 | 无 `/we` | 仅 `error C7560` | 2 |
| 正常 | `/we7560` | 仅 `warning D9014` | 0 |
| 正常 | 无 `/we` | 无输出 | 0 |

结论：**C7560 在该版本默认就是 error**，`/we7560` 不被 cl 识别（只多一条命令行告警 `D9014`，流水线式地给每个编译单元刷一遍）。因此根 `CMakeLists.txt` 的 MSVC 分支已删除该开关；Clang/GCC 的 `-Werror=reorder-init-list` 分支保持原样。请不要以「防止乱序初始化器漂进主干」为由把它加回来——C7560 已经会直接让构建失败。

## MANUAL 项

`verify.ps1` 只在装不出自动化条件时才记 MANUAL：

- SmartScreen 未签名提示（需要联网真机 + 未签名包）。

其余三项已由 `tools\win\ui-checks.ps1` 自动判定，不再靠人工记忆：

- WebView2 缺失提示；
- 窗口位置与大小记忆；
- 打印对话框（预览能否调出、能否取消、取消后是否卡死）。

对应关系：`verify.ps1` 里这三条 MANUAL 的说明文字已指向 `ui-checks.ps1`，跑法见下一节。

## UI 回归检查（ui-checks.ps1）

替代上面三项 MANUAL，自动判定并输出 `[PASS]/[FAIL]/[SKIP]` 与汇总行；**退出码 1 表示至少一项 FAIL**，SKIP 不影响退出码。

```powershell
# 默认用仓库构建产物：shell\win\build\xiaotu-shell.exe + build\windows\server\xiaotu-server.exe
.\tools\win\ui-checks.ps1

# 指定产物与超时/容差
.\tools\win\ui-checks.ps1 -ShellPath C:\dist\xiaotu-shell.exe -StartupTimeoutSec 90 -WindowTolerancePx 4

# 只跑其中两项
.\tools\win\ui-checks.ps1 -SkipPrint -SkipWebView2Missing
```

| 检查 | 验什么 | 判定依据 |
| --- | --- | --- |
| 窗口位置与大小记忆 | 移动/缩放窗口后正常关闭，再启动是否回到原位 | 记忆文件 `%LOCALAPPDATA%\com.simem.xiaotu\shell-window.txt` 与关闭前矩形一致；重启后 `GetWindowRect` 各边偏差 ≤ `-WindowTolerancePx`（默认 2px） |
| 打印预览 | 界面「报表 → 库存余额表 → 打印」能否调出打印界面、取消后是否卡死 | 出现 CDP 目标 `edge://print/` 且界面文本含「打印机」「份数」；点「取消」后目标关闭，主界面仍能执行脚本，`print-view` / `print-mode` 已被 `afterprint` 清理 |
| WebView2 缺失提示 | 运行时缺失时是否给出可读提示而非白屏/闪退 | 仅对子进程设 `WEBVIEW2_BROWSER_EXECUTABLE_FOLDER` 指向不存在的临时目录；弹窗含 `HRESULT 0x80070002` 与下载链接；点确定后退出码 1；去掉变量后能正常加载 `#/item` |

前提与边界：

- 需要先构建 Windows 壳与服务端（脚本会在 `%TEMP%\ui-checks-*` 里复制一份，壳与服务端必须能配对启动）；
- 打印预览需要 WebView2 Runtime，且系统至少有一个打印队列（虚拟打印机即可，例如 Microsoft Print to PDF）；没有打印机时 Edge 打印预览仍会出现，只是没有可用目标；
- 每次运行使用独立临时 stage 并把 `LOCALAPPDATA` 指向其中，**不读写真实数据目录**，也不写 `dist`；
- 不安装/不卸载 WebView2，不改注册表、不动系统组件；模拟缺失只靠子进程环境变量，退出即还原；
- 失败不误报：`WEBVIEW2_BROWSER_EXECUTABLE_FOLDER` 未生效（壳正常启动）时该项记 SKIP；本机确实没有 WebView2 Runtime 时，提示项只校验文案与退出码，复原启动记 SKIP，窗口记忆项也记 SKIP；
- Windows 的系统打印对话框在 WebView2 里是内置打印预览（`edge://print/`），不是 Win32 `#32770`；`#32770` 只在用户在预览里按 Ctrl+Shift+P 时出现，本脚本不覆盖该分支。

## 已知不能由本脚本替代

- 真实针式打印机（套打、连续走纸、多层复写）；
- 购买证书后的 SmartScreen 信誉表现；
- Apple 公证 / Gatekeeper（macOS 侧）。

## 构建耗时采集（profile-build.ps1）

给 Windows 侧排查「比 mac 慢」用。**默认只读**：只采集环境，不跑 cmake/ctest，不改任何设置。

```powershell
# 只采集环境
.\tools\win\profile-build.ps1 -Repo C:\src\xiaotu-inventory

# 额外做一次增量计时（用现有 build 目录）
.\tools\win\profile-build.ps1 -Repo C:\src\xiaotu-inventory -Measure

# 全量计时（另建临时构建目录，结束删除）；与 -Measure 相互独立
.\tools\win\profile-build.ps1 -Repo C:\src\xiaotu-inventory -FullMeasure
```

采集五项：configure/build/ctest 耗时（仅开关打开时）、全量或增量与缓存/生成器、仓库与 vcpkg 的盘和卷类型、Defender 实时保护与排除项、CPU/内存/页面文件。结束打印十几行摘要，最后一行给「配置问题 / 硬件问题 / 无法判断」及依据。退出码 0 表示采集完整；有项采不到会写明原因并返回 1。

### 判断标准（启发式，不是实测均值）

| 现象 | 异常阈值 | 先动哪一项 |
| --- | --- | --- |
| configure 超过 60 秒 | 明显偏慢 | 检查 vcpkg 是否在机械盘/网络盘，是否有杀毒扫描 |
| 增量 build 超过 10 分钟 | 偏慢 | 先看 Defender 排除项，再看是否每次全量重编 |
| ctest 超过 2 分钟 | 偏慢 | 看是否有用例在等超时，而非 CPU 慢 |
| 无 ccache/sccache 且 build 超过 10 分钟 | 缺缓存 | 装 sccache 并设 `CMAKE_CXX_COMPILER_LAUNCHER` |
| 仓库或 vcpkg 在 HDD / 网络盘 / 同步盘 | 任何一项 | 移到本地 SSD，排除同步盘 |
| Defender 实时开且仓库/vcpkg 未排除 | 未排除 | 加目录排除（需管理员；脚本只读不改） |
| 物理核 < 4 或内存 < 16 GB | 低于门槛 | 属硬件限制，先加内存/换机 |
| 页面文件 < 8 GB | 偏小 | 调大页面文件，避免构建时换页 |
| 生成器是 VS 而非 Ninja | 非 Ninja | 改用 Ninja 预设 |

这些阈值是人给的启发式判断，不是实测出来的均值；不要当成测量结论。
