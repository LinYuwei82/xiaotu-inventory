## 小兔进销存 1.0.10

### 本次更新

- **双端统一到 1.0.10**：macOS 与 Windows 同一版本、同一构建号 `6`。macOS 从 1.0.8 直接升级，Windows 从 1.0.9 升级。
- **Windows 包完整性**：延续 1.0.9 的修复，绿色包与安装包随带 OpenSSL 运行时（`libcrypto-3-x64.dll`、`libssl-3-x64.dll`），未安装 OpenSSL 的机器也能正常启动。
- **发布链路升级**：GitHub / Gitee Release 改由 CI 自动创建，附件含安装包、绿色包与 `SHA256SUMS.txt`；更新清单新增 `notes_url`，客户端「查看更新详情」可直接打开本说明页。
- **Windows 编码统一**：新增独立模块 `win_encoding`，统一命令行参数、路径、控制台输出、子进程输出与错误文本的 UTF-8 边界，修复中文乱码。
- **修复 Windows 出包构建号**：桌面壳不再沿用 `CMakeCache` 里的旧构建号；绿色包与安装包统一使用本次构建的壳，避免两端构建号不一致导致反复提示更新。
- **WebView2 SDK 兼容性**：支持 nuget 与扁平两种目录布局，并按架构/版本自检，缺 SDK 时给出可操作提示。

### 构建与分发

- macOS 安装包使用 **Developer ID 签名 + Apple 公证 + stapler 票据**。
- 包体实测：dmg `6,393,471` 字节（约 `6.10` MiB），数据来源 `dist/manifest.json`。
- Windows 安装包与绿色版同步更新，构建号 `6`。

### 下载

| 平台 | 文件 | 说明 |
| --- | --- | --- |
| macOS（Apple Silicon） | `xiaotu-1.0.10-macos-arm64.dmg` | 拖拽到「应用程序」即可 |
| Windows | `xiaotu-1.0.10-win64-setup.exe` | 安装版 |
| Windows | `xiaotu-1.0.10-win64-portable.zip` | 免安装绿色版 |

### 校验

随附 `SHA256SUMS.txt`，下载后可自行校验：

```bash
# macOS / Linux
shasum -a 256 -c SHA256SUMS.txt

# Windows
certutil -hashfile xiaotu-1.0.10-win64-setup.exe SHA256
```

### 升级说明

- macOS 1.0.8 与 Windows 1.0.9 及更早版本均可在应用内「检查更新」直接升级到 1.0.10，也可下载安装包覆盖安装。
- Windows 安装包尚未启用 Authenticode 签名，首次运行仍可能出现 SmartScreen 提示，选择「更多信息 → 仍要运行」即可。

### 系统要求

- macOS 14.0 及以上（Apple Silicon）
- Windows 10 及以上（64 位，需 WebView2 Runtime）

---

<sub>使用中遇到问题请在 Issues 中反馈，并附上版本号与系统信息。</sub>
