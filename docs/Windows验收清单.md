# Windows 验收清单（12600k 实机）

适用版本：1.0.3。目标：确认 MSVC 构建、未签名安装包、安装/启动/业务/升级/卸载全链路在真实 Windows 上可用。

用法：逐条执行，在「结果」列填 **通过 / 不通过 / 未测**，附截图或命令输出；失败项贴完整报错原文，不要概括。

环境记录（先填）：

| 项 | 值 |
| --- | --- |
| Windows 版本 / 内部版本 | |
| 是否全新机器或干净账户 | |
| Visual Studio / MSVC 版本 | |
| CMake / Ninja 版本 | |
| 是否已装 WebView2 Runtime（版本号） | |

## 1. MSVC 编译

- [ ] 拉取 `main`，记录 commit：`git rev-parse --short HEAD`。
- [ ] 配置：`cmake --preset windows-release`
- [ ] 构建：`cmake --build --preset windows-release`
- [ ] 测试：`ctest --preset windows-release --output-on-failure`

结果：______；commit：______；ctest 通过数：____ / ____。

### 1.1 `/we7560` 生效

- [ ] 在 VS 开发者命令行执行探针，确认指定初始化器错序被当作错误：

  ```bat
  > probe.cpp (
  echo struct S { int a; int b; };
  echo S s{ .b = 2, .a = 1 };
  )
  cl /nologo /std:c++20 /we7560 /c probe.cpp
  ```

  期望：编译失败，报 **C7560**（designated initializers 顺序与声明不一致）。记下完整报错。
- [ ] 撤掉探针文件，确认没有留在仓库或构建目录里。
- [ ] 确认项目本身确实带上了该选项：`cmake --build --preset windows-release --verbose` 的输出中能看到 `/we7560`（没有则记不通过）。

结果：______；报错原文：______。

### 1.2 `/MT` 静态运行库

- [ ] `dumpbin /dependents build\windows\server\xiaotu-server.exe | findstr /i "vcruntime msvcp"`
  期望：**无输出**（不依赖 `vcruntime140.dll` / `msvcp140.dll`）。
- [ ] 对 `xiaotu-shell.exe` 重复一次，期望同样无输出。
- [ ] 服务端与壳的 exe 都存在：
  `build\windows\server\xiaotu-server.exe`、`shell\win\build\xiaotu-shell.exe`
  （若壳已接入顶层构建，则位于 `build\windows\shell\win\xiaotu-shell.exe`，打包脚本两条路径都会找）。

结果：______。

## 2. 安装包

- [ ] 从 CI artifact `xiaotu-win-installer` 下载，记录完整文件名与大小。
- [ ] 打开包内 `SIGNING.txt`，记录全文。
- [ ] 确认它与安装包实际签名状态一致：未签名包应写「未签名」，已签名包应写「已签名」且 `Get-AuthenticodeSignature` 为 `Valid`，不能两者矛盾。
- [ ] 右键包内 `xiaotu-server.exe` → 属性 → 详细信息，`文件版本`应为 1.0.3（与仓库根 `VERSION` 一致，不能为空）；`xiaotu-shell.exe` 同样有值。

文件名：______；大小：______；`SIGNING.txt`：______。

## 3. 安装

- [ ] 在全新机器或清干净的账户上运行安装包。
- [ ] 全程**没有 UAC 提权弹窗**（`PrivilegesRequired=lowest`）。
- [ ] 安装路径为 `%LOCALAPPDATA%\Programs\小兔进销存`。
- [ ] 开始菜单出现「小兔进销存」（有壳时为窗口入口，无壳时为「浏览器访问」）。
- [ ] 有 `xiaotu-shell.exe` 时，桌面出现「小兔进销存」快捷方式（安装时勾选了桌面图标）。
- [ ] 数据目录为 `%LOCALAPPDATA%\com.simem.xiaotu`，安装后尚未启动时不生成数据库属正常。

结果：______；实际路径：______。

## 4. 首次启动

- [ ] 双击桌面/开始菜单入口，单机版能打开并显示登录或首屏。
- [ ] 首次使用按提示设置管理员口令，能登录。
- [ ] `%LOCALAPPDATA%\com.simem.xiaotu\app.db` 生成。
- [ ] WebView2 缺失场景（虚拟机里先卸载 WebView2）：双击壳应给出明确提示，而不是白屏或闪退；按提示装回 WebView2 后可正常打开。
- [ ] 浏览器模式：`xiaotu-server.exe --server --port 8080` 能启动，浏览器打开 `http://localhost:8080/` 可登录。

结果：______；WebView2 缺失时实际表现：______。

## 5. 业务冒烟

确认「装出来的包真能用」，不是重复跑测试：

- [ ] 新建商品 1 个（填名称、单位、售价），保存成功。
- [ ] 新建客户 1 个，保存成功。
- [ ] 开一张采购入库单（商品、数量 10、单价 10），保存并审核。
- [ ] 库存余额中该商品数量为 10、金额为 100.00。
- [ ] 库存流水能查到这张入库单。
- [ ] 销售明细或采购明细导出 CSV，文件能打开且数字与页面一致。
- [ ] 打开审计页，能看到登录、建档、审核三类记录。

结果：______；异常项：______。

## 6. 升级

- [ ] 先安装上一版本（若有旧包）；没有旧包时用同版本覆盖安装。
- [ ] 覆盖安装前，在数据目录放一条可见数据（如新建商品「升级前测试」）。
- [ ] 覆盖安装完成后，数据目录未被清空，「升级前测试」仍在。
- [ ] 启动后数据库正常迁移（日志无迁移错误），`/api/health` 返回的 `schema_version` 为 v39。
- [ ] 关于/诊断处显示的版本号为新版本 1.0.3。

结果：______；`schema_version`：______。

## 7. 卸载

- [ ] 控制面板或设置中卸载。
- [ ] 程序目录 `%LOCALAPPDATA%\Programs\小兔进销存` 被清空或删除。
- [ ] 数据目录 `%LOCALAPPDATA%\com.simem.xiaotu` **保留**，卸载时弹窗提示该路径。
- [ ] 开始菜单与桌面快捷方式被清除。
- [ ] 注册表卸载项被清除：`HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\*` 下不再有「小兔进销存」。

结果：______；残留项：______。

## 8. 回报格式

每条给「通过 / 不通过 / 未测」+ 证据：

```text
[编号] 结果：通过/不通过/未测
证据：截图文件名 或 命令输出
备注：（失败时贴完整报错原文，不要概括）
```

汇总时列出：通过 __ 条、不通过 __ 条、未测 __ 条；不通过项按严重度排序。

## 已知不能验（不要白费力气）

- **真实针式打印机**：本机无设备，套打偏差、连续走纸、多层复写无法验；见 `docs/M4打印验收清单.md` 的「待真机」。
- **签名后的 SmartScreen 表现**：当前包未签名，只能验「未签名时如何放行」；签名后的信誉表现需购买证书后再看。
- **Apple 公证 / Gatekeeper 正式放行**：macOS 侧事项，Windows 机器无法验证。
