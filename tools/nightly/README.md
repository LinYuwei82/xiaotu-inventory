# 夜间任务（家里那台）

每晚全量构建、跑测试、做 ASan/UBSan 与静态分析，失败时可通过 webhook 通知。脚本与启动模板在本目录，**仓库不会自动加载或启用 launchd**。

## 内容

| 文件 | 说明 |
| --- | --- |
| `nightly.sh` | 可直接运行的夜间任务脚本 |
| `com.simem.xiaotu.nightly.plist` | launchd 模板（含 `__REPO__`、`__LOG_DIR__` 占位符） |

执行顺序：

1. 构建：`cmake --preset macos-release` + `cmake --build --preset release`
2. 测试：`ctest --test-dir build/release --output-on-failure`
3. ASan/UBSan：在 `build/asan` 独立目录按 Debug 配置并构建，再跑一遍 ctest，不污染主构建
4. 静态分析：优先 `clang-tidy`（基于 `build/tidy/compile_commands.json`），未安装时用 `cppcheck`
5. 汇总：日志按日期落盘；任一步失败，退出码为 1

## 依赖

与 README 第 1 节一致：CMake 3.25+、Ninja、ccache、Xcode Command Line Tools。静态分析额外需要：

```bash
brew install llvm        # 提供 clang-tidy；或 brew install cppcheck
```

## 手动运行

```bash
tools/nightly/nightly.sh
tools/nightly/nightly.sh --help    # 查看环境变量
```

输出同时写入终端与日志：`~/Library/Logs/xiaotu-nightly/nightly-YYYY-MM-DD.log`。

可调环境变量：

| 变量 | 默认值 | 用途 |
| --- | --- | --- |
| `XIAOTU_LOG_DIR` | `~/Library/Logs/xiaotu-nightly` | 日志目录 |
| `XIAOTU_ASAN_DIR` | `<repo>/build/asan` | ASan/UBSan 构建目录 |
| `XIAOTU_TIDY_DIR` | `<repo>/build/tidy` | 静态分析构建目录 |
| `XIAOTU_NIGHTLY_WEBHOOK` | 空 | 失败通知地址，POST JSON；为空只写日志 |

webhook 收到的内容示例：

```json
{"service":"xiaotu-inventory","host":"macmini","status":"failed","failed_steps":"构建（macos-release）","log":"/Users/…/nightly-2026-11-12.log","finished_at":"2026-11-12 02:41:03"}
```

## 安装 launchd（可选，需手动执行）

1. 确认脚本可执行：

   ```bash
   chmod +x tools/nightly/nightly.sh
   ```

2. 生成 plist（把占位符替换成实际路径）：

   ```bash
   mkdir -p ~/Library/LaunchAgents ~/Library/Logs/xiaotu-nightly
   sed -e "s|__REPO__|$PWD|g" \
       -e "s|__LOG_DIR__|$HOME/Library/Logs/xiaotu-nightly|g" \
       tools/nightly/com.simem.xiaotu.nightly.plist \
       > ~/Library/LaunchAgents/com.simem.xiaotu.nightly.plist
   ```

3. 如需 webhook，编辑生成的 plist 中 `XIAOTU_NIGHTLY_WEBHOOK` 的值（默认空）。

4. 加载并立即试跑一次：

   ```bash
   launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.simem.xiaotu.nightly.plist
   launchctl kickstart -k gui/$(id -u)/com.simem.xiaotu.nightly
   ```

5. 查看状态与日志：

   ```bash
   launchctl print gui/$(id -u)/com.simem.xiaotu.nightly | head -30
   tail -f ~/Library/Logs/xiaotu-nightly/nightly-$(date '+%Y-%m-%d').log
   ```

6. 卸载：

   ```bash
   launchctl bootout gui/$(id -u) ~/Library/LaunchAgents/com.simem.xiaotu.nightly.plist
   rm ~/Library/LaunchAgents/com.simem.xiaotu.nightly.plist
   ```

默认在每天 02:30 运行；机器休眠错过的任务不会补跑（launchd 的 `StartCalendarInterval` 行为），需要补跑时手动执行脚本。

## 说明

- 脚本复用现有 preset，不新建构建体系；ASan/UBSan 只是同一套 CMake 配置追加 sanitizer 选项。
- `build/`、`.vcpkg-cache/`、`.ccache/` 均不入库，两台机器各自编译。
- 日志不会自动清理；需要长期保留时自行按日期归档或删除旧文件。
