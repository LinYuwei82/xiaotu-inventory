# Windows 实机验收脚本

`verify.ps1` 在真实 Windows 机器上跑出 PASS / FAIL / SKIP / MANUAL，不需要人工逐条敲命令。PowerShell 5.1 与 7 均可运行。

## 用法

在 VS 开发者 PowerShell（保证 `dumpbin` 在 PATH）中：

```powershell
# 最简：仓库、安装包都给绝对路径
.\tools\win\verify.ps1 `
  -Repo C:\src\xiaotu-inventory `
  -Installer C:\Users\me\Downloads\xiaotu-inventory-0.9.0-win64-setup.exe `
  -DataDir C:\Users\me\AppData\Local\com.simem.xiaotu `
  -ExpectedVersion 0.9.0
```

常用参数：

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `-Repo` | `C:\src\xiaotu-inventory` | 仓库根，必须含 `.git` |
| `-Installer` | 自动在 `build\package\win` 与 `%TEMP%\xiaotu-win-installer` 找 | 安装包路径 |
| `-ExpectedVersion` | `0.9.0` | 用于校验 `/api/health` 的版本号 |
| `-DataDir` | `%LOCALAPPDATA%\com.simem.xiaotu` | 测试用数据目录；**不要指向客户真实数据** |
| `-Port` | `18080` | 保留参数，单机版实际用随机端口 |
| `-LogDir` | `%TEMP%\xiaotu-verify-<时间>` | 日志、安装日志、备份 |
| `-SkipBuild` | 关 | 跳过 cmake/ctest |
| `-SkipInstall` | 关 | 跳过安装、卸载与依赖安装的检查 |
| `-AllowExistingData` | 关 | 数据目录已有 `app.db` 时允许继续，会先备份到 `-LogDir\data-backup` |
| `-Yes` | 关 | 安装前不再等待回车确认 |

## 安全约定

- 非 Windows 直接退出码 2，不执行任何操作。
- `-DataDir` 已有 `app.db` 且未加 `-AllowExistingData` 时**中止**，避免把测试跑在真实数据上。
- 安装与卸载前会提示将写入 `%LOCALAPPDATA%`；不加 `-Yes` 时等待回车。
- 所有临时目录用唯一名字，结束时清理；`/we7560` 检查在 `git worktree` 副本里做，不污染主仓库。
- 任何步骤的失败输出都写入 `-LogDir` 下的对应日志。

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
| ctest | `ctest --preset windows-release` 返回 0；失败时列出失败用例名 |
| 静态运行库 /MT | `dumpbin /dependents` 不含 `vcruntime140.dll`、`msvcp140.dll`、`ucrtbased.dll`；找不到 dumpbin 记 SKIP |
| /we7560 生效 | 临时 worktree 的 `document.cpp` 追加错序指定初始化器后编译失败，且输出含 `C7560`；随后主仓库 `document.cpp` 无改动 |
| 安装包签名状态 | `SIGNING.txt` 明确写「未签名」且不写「已签名」 |
| 安装包 SHA256 | 记录 `Get-FileHash` 结果 |
| 静默安装 | 安装器退出码 0。Inno 语义：0 成功、1 用户取消、2 致命错误；静默安装出现 1 视为 FAIL |
| 安装路径 | `%LOCALAPPDATA%\Programs\小兔进销存\xiaotu-server.exe` 存在，`%ProgramFiles%` 下无同名目录 |
| 开始菜单快捷方式 | 开始菜单递归能找到「小兔进销存」快捷方式 |
| 桌面快捷方式 | 有壳时存在，无壳时不存在 |
| 数据目录位置 | `-DataDir` 存在 |
| 单机版启动与接口 | 20 秒内读到服务地址；token 换会话 200；`/api/health` 200 且 version 等于 `-ExpectedVersion`；`/api/me` 200 |
| 业务冒烟 | 建商品、建客户、采购入库草稿、审核、查库存余额全部 200，且余额含验收商品 |
| 停止服务 | 杀进程后进程不再存在 |
| 同版本覆盖安装 | 再跑一次安装器返回 0；`app.db` 仍存在；程序目录无 `.old` / `.tmp` 残留 |
| 静默卸载 | 卸载器返回 0；`xiaotu-server.exe` 消失；数据目录仍存在；开始菜单快捷方式消失 |

## MANUAL 项

脚本只打印人工操作提示并记 MANUAL，不假装验过：

- WebView2 缺失提示（虚拟机先卸载 WebView2）；
- 窗口位置与大小记忆；
- 打印对话框（纸型 / 边距 / 缩放）；
- SmartScreen 未签名提示。

## 已知不能由本脚本替代

- 真实针式打印机（套打、连续走纸、多层复写）；
- 购买证书后的 SmartScreen 信誉表现；
- Apple 公证 / Gatekeeper（macOS 侧）。
