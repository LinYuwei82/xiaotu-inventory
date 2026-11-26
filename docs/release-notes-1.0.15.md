## 小兔进销存 1.0.15

### 本次更新（重点：Windows）

- **修复「点安装更新后程序直接消失」**（Windows）：
  - 静默安装完成后必定重新拉起新版本；此前使用绿色解压版的机器升级时程序被关掉后不会自动回来，本次修复。
  - 更新安装前先把旧程序温和关闭，避免 WebView2 运行环境残留导致新版本启动失败；温和关闭不生效时才强制结束。
  - 启动安装器前对已下载的安装包重新校验大小与 SHA-256：包体损坏或被安全软件截断时拒绝安装并提示重新下载，程序不会再"先关掉自己再装失败"。
  - 服务端发布新包后，本机缓存的旧安装包自动作废并重新下载，不会再误装旧包。
- **更新状态显示修复**：重启应用后，已下载完成的安装包不再显示为「0 B」，改为显示真实大小。
- macOS 功能无变化，与 Windows 同步版本号。

### 构建与分发

- 双平台同步更新到 1.0.15，构建号 `11`。

### 下载

| 平台 | 文件 | 说明 |
| --- | --- | --- |
| macOS（Apple Silicon） | `xiaotu-1.0.15-macos-arm64.dmg` | Developer ID 签名 + 公证，`.app` 与 DMG 均已 staple |
| Windows | `xiaotu-1.0.15-win64-setup.exe` | Inno Setup 安装包 |
| Windows | `xiaotu-1.0.15-win64-portable.zip` | 绿色版 |

### 校验

```bash
shasum -a 256 xiaotu-1.0.15-macos-arm64.dmg
# 期望：见 stable/manifest.json 的 platforms.macos.artifacts.dmg.sha256
```

### 升级说明

- **曾遇到「点安装更新就闪退」的 Windows 用户**：本次请从官网下载页手动下载 `xiaotu-1.0.15-win64-setup.exe` 双击安装（覆盖安装，数据不丢）；装好后应用内更新即恢复正常。
- 1.0.14 用户可直接在应用内「检查更新 → 安装更新」；升级前请先备份。
- Windows 安装包尚未启用 Authenticode 签名，首次运行仍可能出现 SmartScreen 提示，选择「更多信息 → 仍要运行」即可。

### 系统要求

- macOS 13 及以上（Apple Silicon）
- Windows 10/11 64 位；桌面窗口需要 WebView2 Runtime（安装包可自动安装）
