## 小兔进销存 1.0.23

### 本次更新（Windows 更新界面简化）

- **Windows 软件更新界面与 macOS 对齐**：去掉「发现新版本时自动下载安装包」开关与「下载并校验 / 取消下载」按钮，统一为「检查更新 / 查看更新详情 / 安装更新」三个入口，下载进行中显示进度。
- **安装更新一键完成**：点击「安装更新」时若安装包尚未下载，程序会自动完成下载与 SHA256 校验后进入安装，无需手动分步操作；下载失败可直接重试。
- 数据与配置不受影响。

### 构建与分发

- 双平台同步更新到 1.0.23，构建号 `19`。

### 下载

| 平台 | 文件 | 说明 |
| --- | --- | --- |
| macOS（Apple Silicon） | `xiaotu-1.0.23-macos-arm64.dmg` | Developer ID 签名 + 公证，`.app` 与 DMG 均已 staple |
| Windows | `xiaotu-1.0.23-win64-setup.exe` | Inno Setup 安装包 |
| Windows | `xiaotu-1.0.23-win64-portable.zip` | 绿色版 |

### 校验

```bash
shasum -a 256 xiaotu-1.0.23-macos-arm64.dmg
# 期望：见 stable/manifest.json 的 platforms.macos.artifacts.dmg.sha256
```

### 升级说明

- 1.0.22 用户可直接在应用内「检查更新」获取本次版本；升级前请先备份。
- Windows 更新安装走协助器模式（等待退出 → 校验 → 安装 → 完成），失败自动回滚。
- Windows 安装包尚未启用 Authenticode 签名，首次运行仍可能出现 SmartScreen 提示，选择「更多信息 → 仍要运行」即可。

### 系统要求

- macOS 13 及以上（Apple Silicon）
- Windows 10/11 64 位；桌面窗口需要 WebView2 Runtime（安装包可自动安装）
