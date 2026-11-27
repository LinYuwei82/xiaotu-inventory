## 小兔进销存 1.0.17

### 本次更新（启动速度优化）

- **macOS 开窗更快**：双击图标后先显示「正在启动」窗口，服务就绪后同窗直接切换为首屏，开窗比之前早约 0.2 秒，不再出现"点了没反应"的空白等待。
- **首屏加载更快**：启动阶段的多个网络请求由串行改为并行，减少多次往返等待。
- **启动期维护后移**：数据库维护与每日备份检查不再占用启动过程，备份检查延后到启动 60 秒后进行，避免与首屏抢磁盘。
- **启动耗时打点**：服务端、桌面壳、界面三段各自记录启动耗时日志，后续如遇"启动慢"可直接定位到具体环节。
- 除启动过程外，功能与界面无变化。

### 构建与分发

- 双平台同步更新到 1.0.17，构建号 `13`。

### 下载

| 平台 | 文件 | 说明 |
| --- | --- | --- |
| macOS（Apple Silicon） | `xiaotu-1.0.17-macos-arm64.dmg` | Developer ID 签名 + 公证，`.app` 与 DMG 均已 staple |
| Windows | `xiaotu-1.0.17-win64-setup.exe` | Inno Setup 安装包 |
| Windows | `xiaotu-1.0.17-win64-portable.zip` | 绿色版 |

### 校验

```bash
shasum -a 256 xiaotu-1.0.17-macos-arm64.dmg
# 期望：见 stable/manifest.json 的 platforms.macos.artifacts.dmg.sha256
```

### 升级说明

- 1.0.16 用户可直接在应用内「检查更新」获取本次版本；升级前请先备份。
- Windows 安装包尚未启用 Authenticode 签名，首次运行仍可能出现 SmartScreen 提示，选择「更多信息 → 仍要运行」即可。

### 系统要求

- macOS 13 及以上（Apple Silicon）
- Windows 10/11 64 位；桌面窗口需要 WebView2 Runtime（安装包可自动安装）
