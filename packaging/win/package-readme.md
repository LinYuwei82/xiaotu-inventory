# 小兔进销存（Windows）

发布者：个人开发者 `si_mem`；版权：`Copyright © 2026 小兔在线`。

## 最快开始

### 方式一：双击桌面窗口（包内有 `xiaotu-shell.exe` 时）

1. 双击本目录里的 **`xiaotu-shell.exe`**。
2. 稍等几秒，会弹出「小兔进销存」窗口，里面就是软件界面；第一次使用按提示登录。
3. 关闭窗口即退出，后台服务会一起停掉，不会残留进程。

### 方式二：用浏览器访问（任何包都能用）

桌面窗口只是把界面套了个窗口，服务端本身也能单独跑：

```powershell
# 1. 首次：给内置管理员设置口令（8–128 位，自己替换引号里的内容）
$env:XIAOTU_PASSWORD = '你的口令'
.\xiaotu-server.exe --set-password admin

# 2. 启动服务，然后浏览器打开 http://localhost:8080/
.\xiaotu-server.exe --server --port 8080
```

要让局域网里其他电脑访问，就用本机 IP 打开 `http://<本机IP>:8080/`，并放行该端口的防火墙。

数据保存在 `%LOCALAPPDATA%\com.simem.xiaotu`，卸载时删除该目录即可。

## 桌面窗口需要 WebView2 Runtime

`xiaotu-shell.exe` 依赖系统的 **WebView2 Runtime**（Windows 11 自带；部分 Windows 10 没有）。
用安装包装时，向导「附加任务」里的「下载并安装 WebView2 运行环境」默认勾选（需要联网），
装完会自动装好；IT 静默安装（`/VERYSILENT`）同样会自动装，不需要额外参数。绿色包没有这一步。
双击后若提示缺少 WebView2，或窗口一片空白，按下面处理：

- 检测方法（在 PowerShell 里执行，有版本号输出即为已安装）：
  ```powershell
  (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}' -ErrorAction SilentlyContinue).pv
  ```
- 未安装时到这里下载安装（约 2 MB，装完重新双击 `xiaotu-shell.exe`）：
  https://go.microsoft.com/fwlink/p/?LinkId=2124703
- 不想装也可以直接用上面的「方式二：用浏览器访问」。

## 可选：配置文件

把 `config.toml.example` 复制为 `config.toml` 放到数据目录
`%LOCALAPPDATA%\com.simem.xiaotu` 下，或用 `--config <文件>` 指定，可调整端口、监听地址、
日志级别等。命令行参数优先级高于配置文件。

## 目录内容

| 文件 | 说明 |
| --- | --- |
| `xiaotu-shell.exe` | 桌面窗口，双击即用（自动拉起同目录的 `xiaotu-server.exe`）；仅含桌面窗口的包才有 |
| `xiaotu-server.exe` | 服务端本体，也可单独用浏览器访问 |
| `config.toml.example` | 配置样例 |
| `logs\` | 服务端日志（启动后生成 app.log） |

两个 exe 必须放在同一目录；移动时请整目录一起移动。
