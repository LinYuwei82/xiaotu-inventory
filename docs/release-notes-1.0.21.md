## 小兔进销存 1.0.21

### 本次更新（Windows 更新日志落盘）

- **修复：Windows 单机版更新日志不落盘**：桌面壳的更新检查、下载、安装日志此前只在调试控制台可见，正式运行时无处留存。本版起写入 `<数据目录>\logs\shell.log`（与服务端日志分文件，自动轮转），更新异常可直接取证。
- 功能无变化，数据与配置不受影响。

### 构建与分发

- 双平台同步更新到 1.0.21，构建号 `17`。

### 下载

| 平台 | 文件 | 说明 |
| --- | --- | --- |
| macOS（Apple Silicon） | `xiaotu-1.0.21-macos-arm64.dmg` | Developer ID 签名 + 公证，`.app` 与 DMG 均已 staple |
| Windows | `xiaotu-1.0.21-win64-setup.exe` | Inno Setup 安装包 |
| Windows | `xiaotu-1.0.21-win64-portable.zip` | 绿色版 |

### 校验

```bash
shasum -a 256 xiaotu-1.0.21-macos-arm64.dmg
# 期望：见 stable/manifest.json 的 platforms.macos.artifacts.dmg.sha256
```

### 升级说明

- 1.0.20 用户可直接在应用内「检查更新」获取本次版本；升级前请先备份。
- Windows 更新安装走协助器模式（等待退出 → 校验 → 安装 → 完成），失败自动回滚。
- Windows 安装包尚未启用 Authenticode 签名，首次运行仍可能出现 SmartScreen 提示，选择「更多信息 → 仍要运行」即可。

### 系统要求

- macOS 13 及以上（Apple Silicon）
- Windows 10/11 64 位；桌面窗口需要 WebView2 Runtime（安装包可自动安装）
