## 小兔进销存 1.0.28

### 本次更新（数量精度修复与前端模块化）

- **修复数量被放大 1 万倍**：新建/编辑单据、订单以及「订单转出库」时，本地数量不再按 ×10000 定点整数提交，统一改为十进制字符串，服务端按原值入库。
- **前端按职责拆分为 ES modules**：入口 `web/app.js` 只负责加载 `web/app/` 下的模块，公共层、单据、订单、开单页与基础资料页各自独立；页面结构与交互保持不变。
- 修复编辑草稿时明细表头与新建不一致的问题。
- 后端报表路由与导出实现按职责拆分，接口与输出不变。

### 构建与分发

- 双平台同步更新到 1.0.28，构建号 `24`。

### 下载

| 平台 | 文件 | 说明 |
| --- | --- | --- |
| macOS（Apple Silicon） | `xiaotu-1.0.28-macos-arm64.dmg` | Developer ID 签名 + 公证，`.app` 与 DMG 均已 staple |
| Windows | `xiaotu-1.0.28-win64-setup.exe` | Inno Setup 安装包 |
| Windows | `xiaotu-1.0.28-win64-portable.zip` | 绿色版 |

### 校验

```bash
shasum -a 256 xiaotu-1.0.28-macos-arm64.dmg
# 期望：见 stable/manifest.json 的 platforms.macos.artifacts.dmg.sha256
```

### 升级说明

- 1.0.25 及更早的 macOS 用户、1.0.27 及更早的 Windows 用户，可在应用内「检查更新」→「安装更新」完成升级；升级前请先备份。
- 若此前已录入被放大过的数量，请人工核对并修正草稿与单据。
- Windows 安装包尚未启用 Authenticode 签名，首次运行仍可能出现 SmartScreen 提示，选择「更多信息 → 仍要运行」即可。

### 系统要求

- macOS 13 及以上（Apple Silicon）
- Windows 10/11 64 位；桌面窗口需要 WebView2 Runtime（安装包可自动安装）
