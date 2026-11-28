## 小兔进销存 1.0.24

### 本次更新（双平台自动更新简化）

- **Windows 安装版一键完成更新**：点击「安装更新」后程序静默运行新安装包并自动重启到新版本，不再依赖单独的协助进程，步骤更少、失败点更少。
- **Windows 绿色版更新更直接**：点击「安装更新」后自动下载校验并打开更新包所在目录，关闭程序后解压覆盖原文件、重新打开即完成更新。
- **macOS 更新全面交给 Sparkle**：一键完成下载、签名校验、原子替换与自动重启；移除「回退到上一版本」入口（如需回到旧版，可随时从官网 Release 页重装）。
- **「检查更新」随时可用**：移除手动检查冷却限制，连续点击不再被拦截；后台自动检查间隔与失败退避保持不变。
- 数据与配置不受影响。

### 构建与分发

- 双平台同步更新到 1.0.24，构建号 `20`。

### 下载

| 平台 | 文件 | 说明 |
| --- | --- | --- |
| macOS（Apple Silicon） | `xiaotu-1.0.24-macos-arm64.dmg` | Developer ID 签名 + 公证，`.app` 与 DMG 均已 staple |
| Windows | `xiaotu-1.0.24-win64-setup.exe` | Inno Setup 安装包 |
| Windows | `xiaotu-1.0.24-win64-portable.zip` | 绿色版 |

### 校验

```bash
shasum -a 256 xiaotu-1.0.24-macos-arm64.dmg
# 期望：见 stable/manifest.json 的 platforms.macos.artifacts.dmg.sha256
```

### 升级说明

- 1.0.23 用户可直接在应用内「检查更新」获取本次版本；升级前请先备份。
- 从 1.0.23 升级 Windows 安装版时仍走旧协助器兼容路径，升级完成后即获得新的一键更新流程。
- Windows 安装包尚未启用 Authenticode 签名，首次运行仍可能出现 SmartScreen 提示，选择「更多信息 → 仍要运行」即可。

### 系统要求

- macOS 13 及以上（Apple Silicon）
- Windows 10/11 64 位；桌面窗口需要 WebView2 Runtime（安装包可自动安装）
