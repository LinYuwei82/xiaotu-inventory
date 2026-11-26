## 小兔进销存 1.0.14

### 本次更新（macOS 与 Windows）

- **成本重算修复**：采购入库流水金额被改动后，「成本重算」原先走不到按单据单价还原的分支，会与结存对不上；现已按单据单价正确重算，重算后与库存余额一致。销售退货一并纳入精确匹配。
- **单号续号更稳**：自动生成结算单号时，若同前缀存在脏后缀（非数字或超长），原会直接报错；现改为校验后回落到从头计数，不再出现提交失败。
- **安全加固**（用户不可见）：
  - 会话 Cookie 的 `Secure` 标志只在请求确实来自本机反向代理时才采信 `X-Forwarded-Proto`，直连客户端自报该头不再生效。
  - 关闭服务端点的回环判断增加 `X-Forwarded-For` 末跳校验，同机反向代理部署下的远程访客不再被当作本机。
  - 单机版带启动令牌的一次性访问地址改走 stderr，避免服务化 / launchd 场景把它写进日志。
- **检查更新提示更明确**：手动检查更新处于冷却期时，直接提示「请 N 秒后再试」，不再笼统显示「稍后再试」。

### 构建与分发

- 双平台同步更新到 1.0.14，构建号 `10`。

### 下载

| 平台 | 文件 | 说明 |
| --- | --- | --- |
| macOS（Apple Silicon） | `xiaotu-1.0.14-macos-arm64.dmg` | Developer ID 签名 + 公证，`.app` 与 DMG 均已 staple |
| Windows | `xiaotu-1.0.14-win64-setup.exe` | NSIS 安装包 |
| Windows | `xiaotu-1.0.14-win64-portable.zip` | 绿色版 |

### 校验

```bash
shasum -a 256 xiaotu-1.0.14-macos-arm64.dmg
# 期望：见 stable/manifest.json 的 platforms.macos.artifacts.dmg.sha256
```

### 升级说明

- 1.0.13 用户可直接在应用内「检查更新 → 安装更新」，或下载 DMG 覆盖安装；升级前请先备份。
- Windows 安装包尚未启用 Authenticode 签名，首次运行仍可能出现 SmartScreen 提示，选择「更多信息 → 仍要运行」即可。

### 系统要求

- macOS 13 及以上（Apple Silicon）
- Windows 10/11 64 位；桌面窗口需要 WebView2 Runtime（安装包可自动安装）
