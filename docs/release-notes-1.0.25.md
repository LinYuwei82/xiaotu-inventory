## 小兔进销存 1.0.25

### 本次更新（界面与署名）

- **Windows 撤掉顶部原生菜单栏**：窗口不再显示「帮助」菜单，界面全部交给网页端，与 macOS 版观感一致；「关于与诊断」信息仍在应用内「系统」菜单查看。
- **版权署名更新为「小兔在线」**：应用关于信息、安装包属性、系统「程序和功能」列表中的发布者与版权统一更新。
- 数据与配置不受影响。

### 构建与分发

- 双平台同步更新到 1.0.25，构建号 `21`。

### 下载

| 平台 | 文件 | 说明 |
| --- | --- | --- |
| macOS（Apple Silicon） | `xiaotu-1.0.25-macos-arm64.dmg` | Developer ID 签名 + 公证，`.app` 与 DMG 均已 staple |
| Windows | `xiaotu-1.0.25-win64-setup.exe` | Inno Setup 安装包 |
| Windows | `xiaotu-1.0.25-win64-portable.zip` | 绿色版 |

### 校验

```bash
shasum -a 256 xiaotu-1.0.25-macos-arm64.dmg
# 期望：见 stable/manifest.json 的 platforms.macos.artifacts.dmg.sha256
```

### 升级说明

- 1.0.24 用户可直接在应用内「检查更新」→「安装更新」一键完成；升级前请先备份。
- Windows 安装包尚未启用 Authenticode 签名，首次运行仍可能出现 SmartScreen 提示，选择「更多信息 → 仍要运行」即可。

### 系统要求

- macOS 13 及以上（Apple Silicon）
- Windows 10/11 64 位；桌面窗口需要 WebView2 Runtime（安装包可自动安装）
