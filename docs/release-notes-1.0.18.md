## 小兔进销存 1.0.18

### 本次更新（macOS 退出问题修复）

- **修复：点窗口红叉后程序没有真正退出**：1.0.17 中关闭主窗口后应用进程仍留在后台，Dock 图标一直挂着"运行中"，需 Cmd+Q 或强制退出才能清掉。现在点红叉即正常退出。
- 未保存内容确认、Cmd+Q 退出、升级安装等行为不变；Windows 不受影响。

### 构建与分发

- 双平台同步更新到 1.0.18，构建号 `14`。

### 下载

| 平台 | 文件 | 说明 |
| --- | --- | --- |
| macOS（Apple Silicon） | `xiaotu-1.0.18-macos-arm64.dmg` | Developer ID 签名 + 公证，`.app` 与 DMG 均已 staple |
| Windows | `xiaotu-1.0.18-win64-setup.exe` | Inno Setup 安装包 |
| Windows | `xiaotu-1.0.18-win64-portable.zip` | 绿色版 |

### 校验

```bash
shasum -a 256 xiaotu-1.0.18-macos-arm64.dmg
# 期望：见 stable/manifest.json 的 platforms.macos.artifacts.dmg.sha256
```

### 升级说明

- 1.0.17 用户可直接在应用内「检查更新」获取本次版本；升级前请先备份。
- Windows 安装包尚未启用 Authenticode 签名，首次运行仍可能出现 SmartScreen 提示，选择「更多信息 → 仍要运行」即可。

### 系统要求

- macOS 13 及以上（Apple Silicon）
- Windows 10/11 64 位；桌面窗口需要 WebView2 Runtime（安装包可自动安装）
