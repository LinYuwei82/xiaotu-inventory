> 历史归档：本页是 1.0.6 发布时的说明与构建号，命令与路径可能已过时，勿照做。

## 小兔进销存 1.0.6

### 本次修复

- **更新提示反复出现**：安装新版本后残留「待更新」状态，导致界面一直提示「发现新版本」，现已清理该状态。
- **更新弹窗改为应用内提示**：macOS 端不再弹出 Sparkle 默认对话框，改为应用内自定义提示，交互更一致。

### 构建与分发

- macOS 安装包使用 **Developer ID 签名 + Apple 公证**，首次打开不再被 Gatekeeper 拦截（无需右键打开）。
- Windows 安装包与绿色版同步更新，构建号 `202611191417`。

### 下载

| 平台 | 文件 | 说明 |
| --- | --- | --- |
| macOS（Apple Silicon） | `xiaotu-1.0.6-macos-arm64.dmg` | 拖拽到「应用程序」即可 |
| Windows | `xiaotu-1.0.6-win64-setup.exe` | 安装版 |
| Windows | `xiaotu-1.0.6-win64-portable.zip` | 免安装绿色版 |

### 校验

随附 `SHA256SUMS.txt`，下载后可自行校验：

```bash
# macOS / Linux
shasum -a 256 -c SHA256SUMS.txt

# Windows
certutil -hashfile xiaotu-1.0.6-win64-setup.exe SHA256
```

### 系统要求

- macOS 13.0 及以上（Apple Silicon）
- Windows 10 及以上（64 位）

---

<sub>使用中遇到问题请在 Issues 中反馈，并附上版本号与系统信息。</sub>
