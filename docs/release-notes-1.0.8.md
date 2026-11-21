## 小兔进销存 1.0.8

### 本次更新

- **Windows 应用内更新链路可用**：1.0.6 已在真机完成「检查更新 → 下载 → ISCC 静默安装 → 重启 → 版本核对」全链路验收，1.0.8 起macOS 与 Windows 双端均可通过应用内更新升级。
- **Windows 编码统一**：新增独立模块 `win_encoding`，统一命令行参数、路径、控制台输出、子进程输出与错误文本的 UTF-8 边界，修复中文乱码。
- **修复 Windows 出包构建号**：桌面壳不再沿用 `CMakeCache` 里的旧构建号；绿色包与安装包统一使用本次构建的壳，避免两端构建号不一致导致反复提示更新。
- **WebView2 SDK 兼容性**：支持 nuget 与扁平两种目录布局，并按架构/版本自检，缺 SDK 时给出可操作提示。
- **CI 稳定性**：Windows 工作流优先使用 runner 预装的 OpenSSL 3，choco 源失效不再中断发布。
- **发布门禁**：v2 清单校验单测接入 CI；pre-commit 公钥校验覆盖全仓；Windows 更新验收状态与包体实测随发版同步。

### 构建与分发

- macOS 安装包使用 **Developer ID 签名 + Apple 公证 + stapler 票据**。
- 包体实测：dmg `6,393,430` 字节（约 6.10 MiB），数据来源 `dist/manifest.json`。
- Windows 安装包与绿色版同步更新，构建号 `2`。

### 下载

| 平台 | 文件 | 说明 |
| --- | --- | --- |
| macOS（Apple Silicon） | `xiaotu-1.0.8-macos-arm64.dmg` | 拖拽到「应用程序」即可 |
| Windows | `xiaotu-1.0.8-win64-setup.exe` | 安装版 |
| Windows | `xiaotu-1.0.8-win64-portable.zip` | 免安装绿色版 |

### 校验

随附 `SHA256SUMS.txt`，下载后可自行校验：

```bash
# macOS / Linux
shasum -a 256 -c SHA256SUMS.txt

# Windows
certutil -hashfile xiaotu-1.0.8-win64-setup.exe SHA256
```

### 升级说明

- 1.0.6 及更早版本可在应用内「检查更新」，或直接下载本次安装包覆盖安装。
- Windows 首次升级仍可能出现 SmartScreen 提示（安装包尚未启用 Authenticode 签名），选择「更多信息 → 仍要运行」即可。

### 系统要求

- macOS 14.0 及以上（Apple Silicon）
- Windows 10 及以上（64 位，需 WebView2 Runtime）

---

<sub>使用中遇到问题请在 Issues 中反馈，并附上版本号与系统信息。</sub>
