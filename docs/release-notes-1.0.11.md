## 小兔进销存 1.0.11

### 本次更新

- **Windows 安装包自动安装 WebView2**：目标机缺少 WebView2 Runtime 时，向导「附加任务」出现默认勾选的自动安装项，安装期联网下载官方引导程序并静默安装；下载或安装失败不阻断主程序安装，完成页仍提供手动入口。IT 静默安装（`/VERYSILENT /SUPPRESSMSGBOXES`）同样自动执行。
- **Windows 导出 PDF 修正**：按当前打印模板的纸型与边距创建 `PrintSettings`，导出的 PDF 不再沿用固定纸型。
- **Windows 打印缩放修正**：缩放补偿改到 CSS 层，修掉部分模板整体缩小 6.25% 的问题。
- **macOS 升级流程收敛**：安装阶段改由 Sparkle 完成，不再打开 dmg 让用户拖拽。
- **发布链路修复**：生成清单前先合并线上 `--prev`，避免单平台发版覆盖另一平台段；版本递增校验按平台比较；Release 标题与说明改走 UTF-8 文件，修复 Gitee 标题乱码。
- **文档与验收清单**：Windows 用户手册、客户安装说明、验收清单与发布检查清单同步到 1.0.11，并补充 WebView2 自动安装的实测数据（Win10 干净 VM 首次安装 67.6 s，其中 WebView2 引导器 64.9 s）。

### 构建与分发

- macOS 安装包使用 **Developer ID 签名 + Apple 公证 + stapler 票据**。
- Windows 安装包与绿色版同步更新，构建号 `7`。

### 下载

| 平台 | 文件 | 说明 |
| --- | --- | --- |
| macOS（Apple Silicon） | `xiaotu-1.0.11-macos-arm64.dmg` | 拖拽到「应用程序」即可 |
| Windows | `xiaotu-1.0.11-win64-setup.exe` | 安装版 |
| Windows | `xiaotu-1.0.11-win64-portable.zip` | 免安装绿色版 |

### 校验

随附 `SHA256SUMS.txt`，下载后可自行校验：

```bash
# macOS / Linux
shasum -a 256 -c SHA256SUMS.txt

# Windows
certutil -hashfile xiaotu-1.0.11-win64-setup.exe SHA256
```

### 升级说明

- 可在应用内「检查更新」升级，或直接下载本次安装包覆盖安装；数据目录与数据库迁移保持不变。
- Windows 安装包尚未启用 Authenticode 签名，首次运行仍可能出现 SmartScreen 提示，选择「更多信息 → 仍要运行」即可。

### 系统要求

- macOS 13 及以上（Apple Silicon）
- Windows 10/11 64 位；桌面窗口需要 WebView2 Runtime（安装包可自动安装）
