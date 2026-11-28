## 小兔进销存 1.0.22

### 本次更新（双端关于菜单）

- **新增：macOS「关于」面板**：应用菜单「关于小兔进销存」打开系统标准关于面板，显示版本号与构建号，并附应用简介与主页链接。
- **新增：Windows「帮助 → 关于」菜单**：主窗口菜单栏新增「帮助」菜单，「关于小兔进销存」弹窗显示版本号、构建号与版权信息。
- 功能与界面无其他变化，数据与配置不受影响。

### 构建与分发

- 双平台同步更新到 1.0.22，构建号 `18`。

### 下载

| 平台 | 文件 | 说明 |
| --- | --- | --- |
| macOS（Apple Silicon） | `xiaotu-1.0.22-macos-arm64.dmg` | Developer ID 签名 + 公证，`.app` 与 DMG 均已 staple |
| Windows | `xiaotu-1.0.22-win64-setup.exe` | Inno Setup 安装包 |
| Windows | `xiaotu-1.0.22-win64-portable.zip` | 绿色版 |

### 校验

```bash
shasum -a 256 xiaotu-1.0.22-macos-arm64.dmg
# 期望：见 stable/manifest.json 的 platforms.macos.artifacts.dmg.sha256
```

### 升级说明

- 1.0.21 用户可直接在应用内「检查更新」获取本次版本；升级前请先备份。
- Windows 更新安装走协助器模式（等待退出 → 校验 → 安装 → 完成），失败自动回滚。
- Windows 安装包尚未启用 Authenticode 签名，首次运行仍可能出现 SmartScreen 提示，选择「更多信息 → 仍要运行」即可。

### 系统要求

- macOS 13 及以上（Apple Silicon）
- Windows 10/11 64 位；桌面窗口需要 WebView2 Runtime（安装包可自动安装）
