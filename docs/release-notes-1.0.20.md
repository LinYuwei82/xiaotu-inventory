## 小兔进销存 1.0.20

### 本次更新（内部架构升级）

- **数据库访问层抽象**：业务代码不再直接依赖 SQLite 接口，为网络版支持 PostgreSQL 打下基础。本版无用户可见的功能变化。
- Windows 自动更新协助器（1.0.19 引入）持续稳定运行。
- macOS 行为不变。

### 构建与分发

- 双平台同步更新到 1.0.20，构建号 `16`。

### 下载

| 平台 | 文件 | 说明 |
| --- | --- | --- |
| macOS（Apple Silicon） | `xiaotu-1.0.20-macos-arm64.dmg` | Developer ID 签名 + 公证，`.app` 与 DMG 均已 staple |
| Windows | `xiaotu-1.0.20-win64-setup.exe` | Inno Setup 安装包 |
| Windows | `xiaotu-1.0.20-win64-portable.zip` | 绿色版 |

### 校验

```bash
shasum -a 256 xiaotu-1.0.20-macos-arm64.dmg
# 期望：见 stable/manifest.json 的 platforms.macos.artifacts.dmg.sha256
```

### 升级说明

- 1.0.19 用户可直接在应用内「检查更新」获取本次版本；升级前请先备份。
- 本次更新安装走 1.0.19 引入的协助器模式（等待退出 → 校验 → 安装 → 完成），失败自动回滚。
- Windows 安装包尚未启用 Authenticode 签名，首次运行仍可能出现 SmartScreen 提示，选择「更多信息 → 仍要运行」即可。

### 系统要求

- macOS 13 及以上（Apple Silicon）
- Windows 10/11 64 位；桌面窗口需要 WebView2 Runtime（安装包可自动安装）
