## 小兔进销存 1.0.16

### 本次更新（自动更新模块架构升级）

- **更新逻辑全面重构**：检查、下载、校验、安装收口为全平台统一的单一状态机，macOS 与 Windows 行为完全一致，杜绝两平台各写一套逻辑导致的行为差异。
- **更新可靠性提升**：
  - 下载中断或程序被强退后，重启会自动回到可重试状态，不残留半截安装包；
  - 服务端发布新包后，本机缓存的旧包立即作废并重新下载，不会误装旧包；
  - 安装前始终重新校验包体大小与 SHA-256，坏包直接拒绝安装并提示重新下载。
- **macOS**：强制更新、跳过版本、检查冷却与失败退避与 Windows 同口径；后台预下载的流量提示确认、失败自动重试等交互保持不变。
- 除更新模块外，功能与界面无变化。

### 构建与分发

- 双平台同步更新到 1.0.16，构建号 `12`。

### 下载

| 平台 | 文件 | 说明 |
| --- | --- | --- |
| macOS（Apple Silicon） | `xiaotu-1.0.16-macos-arm64.dmg` | Developer ID 签名 + 公证，`.app` 与 DMG 均已 staple |
| Windows | `xiaotu-1.0.16-win64-setup.exe` | Inno Setup 安装包 |
| Windows | `xiaotu-1.0.16-win64-portable.zip` | 绿色版 |

### 校验

```bash
shasum -a 256 xiaotu-1.0.16-macos-arm64.dmg
# 期望：见 stable/manifest.json 的 platforms.macos.artifacts.dmg.sha256
```

### 升级说明

- 1.0.15 用户可直接在应用内「检查更新」获取本次版本；升级前请先备份。
- Windows 安装包尚未启用 Authenticode 签名，首次运行仍可能出现 SmartScreen 提示，选择「更多信息 → 仍要运行」即可。

### 系统要求

- macOS 13 及以上（Apple Silicon）
- Windows 10/11 64 位；桌面窗口需要 WebView2 Runtime（安装包可自动安装）
