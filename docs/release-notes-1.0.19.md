## 小兔进销存 1.0.19

### 本次更新（Windows 自动更新安装闪退修复）

- **修复：Windows 自动更新安装时闪退**：此前的更新安装流程过于复杂，部分环境下安装器会无声闪退、更新失败。本版将 Windows 更新安装重构为「协助器模式」——应用退出后由独立的更新协助器完成校验、安装与失败回滚，全程有明确状态（等待退出 → 校验 → 安装 → 完成），成功后自动重启程序。
- **绿色版（portable）用户自动续更 zip 包**：更新清单按已安装的包类型选择下载产物，不再误装安装包。
- **安装结果提示**：更新重启后界面会一次性显示本次安装结果，失败时不再静默。
- macOS 行为不变。

### 构建与分发

- 双平台同步更新到 1.0.19，构建号 `15`。

### 下载

| 平台 | 文件 | 说明 |
| --- | --- | --- |
| macOS（Apple Silicon） | `xiaotu-1.0.19-macos-arm64.dmg` | Developer ID 签名 + 公证，`.app` 与 DMG 均已 staple |
| Windows | `xiaotu-1.0.19-win64-setup.exe` | Inno Setup 安装包 |
| Windows | `xiaotu-1.0.19-win64-portable.zip` | 绿色版 |

### 校验

```bash
shasum -a 256 xiaotu-1.0.19-macos-arm64.dmg
# 期望：见 stable/manifest.json 的 platforms.macos.artifacts.dmg.sha256
```

### 升级说明

- 1.0.18 用户可直接在应用内「检查更新」获取本次版本；升级前请先备份。
- 过渡说明：由旧版壳（≤1.0.18）安装本次更新时仍走一次原有安装路径；自 1.0.19 起全部使用新的协助器模式。
- Windows 安装包尚未启用 Authenticode 签名，首次运行仍可能出现 SmartScreen 提示，选择「更多信息 → 仍要运行」即可。

### 系统要求

- macOS 13 及以上（Apple Silicon）
- Windows 10/11 64 位；桌面窗口需要 WebView2 Runtime（安装包可自动安装）
