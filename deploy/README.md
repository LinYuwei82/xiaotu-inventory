# 部署补充：每日备份

服务端进程内已带每日备份调度（启动时检查一次、之后每 30 分钟检查），正常情况下无需额外的定时任务。本文的 systemd timer / cron 适合希望「进程未运行时也能备份」或统一由系统调度管理的部署。

## 行为

- 统一入口：`deploy/backup.sh`（内部调用 `xiaotu-server --backup-daily --config ...`），cron 与 systemd timer 都只调用它，避免两套命令漂移。
- 幂等：当天已有成功备份时直接退出（输出「跳过每日备份」），不会重复生成。
- 手动立即备份：`XIAOTU_BACKUP_MODE=manual deploy/backup.sh`（底层 `--backup`，无条件生成一份）。
- 并发安全：备份目录内的 `.backup.lock` 互斥；进程内调度与系统定时任务同时触发时只有一方执行，抢锁失败返回非零。
- 保留策略：清理 `backups/app-<时间戳>-daily.db`，保留最近 30 份日备份，并在每个自然月额外保留 1 份（该月最新）。`manual`、`pre-v*`（迁移前）、`pre-restore-*`（恢复前安全备份）不参与清理。
- 生成后校验：备份库自记录的 schema 版本必须与源库一致且不高于程序支持版本，否则该备份判定不可用并写入状态与日志。
- 状态文件：`<备份目录>/backup-status.json`（默认 `<数据目录>/backups/`，可用 `[storage] backup_dir` 单独指定），分别记录最近成功与最近失败；`/api/health` 的 `backup` 字段读取同一来源（只返回文件名，不含目录）。
- 日志：`<数据目录>/logs/app.log`（或 `[log] dir` 配置的目录）。

## 手动与接口

- 命令行：`xiaotu-server --backup --data <数据目录>` 立即生成一份手装备份；失败返回非零。
- HTTP：`POST /api/backup`（管理员 + `setting.write`），返回 `file`、`bytes`、`at`，不返回绝对路径；并发备份返回 409 `BACKUP_BUSY`。
- 在线恢复不提供 HTTP 接口，继续使用 `--restore`，且必须先停服。

## systemd timer（推荐）

```bash
sudo cp deploy/systemd/xiaotu-backup.service /etc/systemd/system/
sudo cp deploy/systemd/xiaotu-backup.timer /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now xiaotu-backup.timer
systemctl list-timers xiaotu-backup.timer
```

systemd 与 cron 二选一，不要同时启用；两者都只调用 `deploy/backup.sh`。

手动触发一次：

```bash
sudo systemctl start xiaotu-backup.service
journalctl -u xiaotu-backup.service -n 50
```

## cron

```bash
sudo cp deploy/cron/xiaotu-backup /etc/cron.d/xiaotu-backup
sudo chmod 644 /etc/cron.d/xiaotu-backup
```

cron.d 文件需要 `用户` 字段（已包含），不要改成用户 crontab 的格式。日志追加到 `/var/log/xiaotu/backup.log`，需确保该目录存在且属主为 `xiaotu`。

## 恢复与版本检查

`--restore <备份文件>` 在覆盖当前库之前会比对 schema 版本：

- 备份版本高于当前程序：拒绝恢复，返回 `SCHEMA_TOO_NEW`，提示先升级程序；
- 无法识别版本（不是本程序的库 / 缺少版本信息）：拒绝，返回 `SCHEMA_UNKNOWN`；
- 备份版本较低：允许恢复，下次启动自动迁移（日志中会有 warn）。

恢复必须在服务停止时执行；确认程序版本与备份匹配后再恢复。

## 更新模块（网络版 / 私有化服务端）

本节只适用于**网络版/私有化服务端**的更新检查。单机版客户端不读取这些配置：它直接使用 `Info.plist` 中的 `XiaotuUpdateManifestURL` 拉取厂商清单，安装由 Sparkle 完成。

### 1. 前置条件

- Python 3 与 `cryptography`（服务端调用与发布侧相同的验签工具）：

```bash
python3 -m venv /opt/xiaotu/tools/package/.venv
/opt/xiaotu/tools/package/.venv/bin/pip install -r /opt/xiaotu/tools/package/requirements.txt
```

- 未安装或路径不可达时，`/api/update/check` 返回 `UPDATE_VERIFY_UNAVAILABLE`，不会下发更新。
- 未配置 `public_key_file` 时返回 `UPDATE_UNVERIFIED`，同样不下发。

### 2. 完整配置

```toml
[update]
# 厂商清单地址：服务端拉取、验签后缓存，缓存有效期 10 分钟。
manifest_url = "https://dl.xiaotu.online/xiaotu/updates/stable/manifest.json"
# 可选：本地/离线清单（v2 统一清单，两平台同一份；服务端按请求平台取对应段）。
# manifest_url 不可用且没有缓存时，才使用这里的文件。
manifest_file = "/srv/xiaotu-updates/stable/manifest.json"
channel = "stable"                  # stable / beta
min_supported_version = "1.3.0"     # 低于此版本强制升级（理由 MIN_SUPPORTED）
rollout_percent = 100               # 灰度上限，实际取 min(清单, 本值)
auto_check = true                   # 启动 30 秒后与每 24h±2h 自动检查
prefetch = true                     # 自动检查命中新版本时后台下载并校验
public_key_file = "/etc/xiaotu/update_signing.pub.pem"
verifier_python = "/opt/xiaotu/tools/package/.venv/bin/python"
verifier_script = "/opt/xiaotu/tools/package/update_manifest.py"
```

- `manifest_url` 必须是 HTTPS；服务端把清单缓存在 `<data_dir>/updates/manifest/<channel>/manifest.json`，有效期内不重复访问外网。
- v2 清单里两个平台共用一份文件；服务端按 `/api/update/check` 的 `platform` 取 `platforms.macos` 或 `platforms.windows`，非桌面平台返回 `PLATFORM_MISMATCH`。
- 远端拉取失败（超时、5xx、断网）或清单校验失败时，回退到上一次验签通过的缓存，状态接口会带 `manifest_stale=true` 与原因；没有缓存才返回错误。
- 新拉取的清单必须先通过结构校验与 Ed25519 验签才会替换缓存，损坏或未签名内容不会污染缓存。
- `manifest_file` 保留给内网/离线部署：不需要访问外网时只配它，每次请求重新读取，替换文件后无需重启。
- 其余配置在进程启动时读取，修改后需重启服务。
- `auto_check` / `prefetch` 在服务启动时读取，运行中不提供切换；修改后必须重启 `xiaotu-server` 才生效，`GET /api/update/status` 只读回显当前值。
- `public_key_file`、`verifier_python`、`verifier_script` 缺一不可，否则按错误码拒绝。

### 3. 公钥生成、保管与更换

生成（离线机器执行，私钥不外传）：

```bash
python3 - <<'PY'
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

key = Ed25519PrivateKey.generate()
open("update_signing_2026.pem", "wb").write(key.private_bytes(
    serialization.Encoding.PEM,
    serialization.PrivateFormat.PKCS8,
    serialization.NoEncryption()))
open("update_signing_2026.pub.pem", "wb").write(key.public_key().public_bytes(
    serialization.Encoding.PEM,
    serialization.PublicFormat.SubjectPublicKeyInfo))
PY
```

- 私钥只保存在离线介质与发布机 CI Secret 中，不进仓库、不进镜像、不进备份明文。
- 公钥部署到 `/etc/xiaotu/update_signing.pub.pem`，权限 0644，属主 root。

更换流程：

1. 生成新密钥对，先用新私钥签发**新清单**。
2. 把新公钥放到新路径（如 `update_signing_2027.pub.pem`），更新 `public_key_file` 并重启服务。
3. 重启后立即验证 `/api/update/check` 返回正常；旧清单若仍被引用需重新签名。
4. 旧私钥归档或销毁，公钥保留一个发布周期用于排查。
5. 注意：客户端 Sparkle 的 `SUPublicEDKey` 内置于 app，**更换该公钥必须先发布一版客户端**；服务端验签公钥可以独立更换，两者不是同一把也可，但建议保持一致以便排查。

### 4. 清单与 appcast 同步

配置 `manifest_url` 时服务端会自行拉取并缓存，不需要本节脚本；只有使用 `manifest_file` 的内网/离线部署才需要同步。同步时不要手工 `cp`：

```bash
XIAOTU_UPDATE_SOURCE=https://dl.xiaotu.online/xiaotu/updates/stable/manifest.json \
XIAOTU_UPDATE_TARGET=/srv/xiaotu-updates/stable/manifest.json \
XIAOTU_UPDATE_APPCAST_SOURCE=https://dl.xiaotu.online/xiaotu/updates/stable/appcast.xml \
XIAOTU_UPDATE_APPCAST_TARGET=/srv/xiaotu-updates/stable/appcast.xml \
XIAOTU_UPDATE_PUBKEY=/etc/xiaotu/update_signing.pub.pem \
XIAOTU_UPDATE_PYTHON=/opt/xiaotu/tools/package/.venv/bin/python \
XIAOTU_UPDATE_TOOL=/opt/xiaotu/tools/package/update_manifest.py \
/opt/xiaotu/deploy/update-sync.sh
```

- 脚本先下载到同目录临时文件、验签通过后再原子替换，服务端不会读到半个文件。
- 定时执行：安装 `deploy/cron/xiaotu-update-sync` 到 `/etc/cron.d/`，或改用 systemd timer，**二选一**。
- 若 `XIAOTU_UPDATE_PUBKEY` 为空，脚本不验签；生产环境必须配置。
- 验签工具默认 `tools/package/update_manifest.py`（v1/v2 都认，需要 `cryptography`）；也可以指向纯标准库的 `tools/release/manifest_v2.py`，免装依赖。

### 4.1 安装与自动替换

`POST /api/update/install`（管理员 + `setting.write`，请求体 `{"confirm": true}`）：

1. 再次校验清单签名与安装包 `sha256`；
2. 按安装包形态定位新程序：单文件可执行程序直接使用；`.zip` 解压后查找与当前程序同名的文件（如 `xiaotu-server.exe`）；
3. 在 `<data_dir>/updates/staging-<version>-<build>/` 生成替换脚本并后台启动；
4. 返回响应后服务优雅退出；脚本等待进程退出（最多 120 秒），把旧程序备份为 `<程序>.old`、覆盖新程序，再按**原启动参数、原工作目录**重启；
5. 覆盖失败时把 `.old` 移回原位置，程序保持旧版本可用；结果写入 `staging-*/update-apply.log`。

限制与注意事项：

- 安装包必须是单文件可执行程序或 zip 包；`.dmg` 等安装器型包不支持服务端自动替换，会返回 `UPDATE_PACKAGE_UNSUPPORTED`。
- 只替换当前可执行文件，不覆盖 web 静态资源、配置、数据库或备份；这些仍由部署流程管理。
- systemd 部署同样适用：脚本会直接拉起新进程；`Restart=on-failure` 只作为额外兜底。
- 替换脚本以服务进程相同权限运行，因此程序所在目录必须可写；只读目录会失败并保留旧版本。

### 5. 灰度放行与回滚

清单是签名的，**不能直接改 `platforms.<platform>.rollout_percent`**（会导致签名失效）。使用 `manifest_v2.py resign` 只改放量比例并重新签名，其余字段原样保留。

自动逐档放量（默认 1% → 10% → 50% → 100%，每档 24 小时）：

```bash
# 0. 取回当前线上清单（publish_oss.sh 的临时目录不会保留）
curl -fsS https://dl.xiaotu.online/xiaotu/updates/stable/manifest.json \
  -o build/package/manifest.json

# 1. 预览：只本地重签并验签，不上传、不等待
tools/package/rollout_oss.sh --manifest build/package/manifest.json --dry-run

# 2. 正式放量：每档自动上传并等待源站生效，再等观察时长进入下一档
OSS_BUCKET=你的bucket OSS_REGION=cn-hangzhou \
  tools/package/rollout_oss.sh --manifest build/package/manifest.json --yes

# 只放量一个平台：--platform all / macos / windows（默认 all）
tools/package/rollout_oss.sh --manifest build/package/manifest.json --platform windows --yes

# 自定义：每档 12 小时，比例 5% → 20% → 60% → 100%
tools/package/rollout_oss.sh --steps 5,20,60,100 --interval 12h --yes
```

- 脚本只改清单，appcast 不随比例变化，不重传；清单每档归档到 `build/package/rollout/<product>-<channel>-<时间戳>/manifest-<比例>.json`。
- 每档上传后轮询各平台 `rollout_percent`，目标平台全部生效才进入下一档；超时则停下，当前档保持生效。
- `dl.xiaotu.online` 直连 OSS、没有 CDN 层，发布脚本默认不 purge（`PURGE=true` 仅备将来接入 CDN）。
- Ctrl-C 可随时停在当前档，不会回退比例；要继续时重跑并跳过已完成的档位即可。
- 依赖 OSS 凭据（同 `publish_oss.sh`）与签名私钥；`--dry-run` 不需要 OSS。

手动重签仍然可用（`manifest_v2.py` 是三仓库共用的纯标准库实现）：

```bash
# 只把比例改成 10%，其余字段不动（--platform 默认 all）
python3 tools/release/manifest_v2.py resign \
  --manifest build/package/manifest.json \
  --key ~/keys/update_signing.pem \
  --rollout 10 --platform windows
```

- `--out` 缺省时原地覆盖输入清单；建议先归档原始清单。
- 只接受 1–100 的 `--rollout`；版本、构建号、URL、sha256、强制字段等不可在重签时改写。
- 重签会重新计算 Ed25519 签名，`updated_at` 沿用原值（不参与刷新），同一输入连续重签输出一致。

放量观察：每档至少 24 小时，确认 `/api/update/check` 返回符合预期，且无启动失败/回退日志后再进入下一档。服务端读取 `min(清单 rollout, 配置 rollout_percent)`；单机版壳按安装 UUID + 目标 build 稳定分桶，强制更新不受比例限制。

回滚：

1. 每次发布保留已签名清单与 appcast 归档（如 `releases/1.4.2/manifest.json`）。
2. 把上一版归档的 `manifest.json`、`appcast.xml` 原子替换回更新源。
3. 确认 `/api/update/check` 返回上一版本；安装包保留至少 3 个 stable 版本。
4. 回滚包若重新打包，构建号必须递增，否则 Windows 同版本会判为“无更新”（macOS 只比 version）。

### 6. 常见错误码

| 错误码 | 出现原因 | 处理 |
| --- | --- | --- |
| `UPDATE_NOT_CONFIGURED` | 未配置 `manifest_url` 与 `manifest_file`；返回 200 且无更新 | 补齐配置并重启 |
| `UPDATE_MANIFEST_UNREACHABLE` | 厂商清单拉取失败且本地没有可用缓存（503） | 检查网络、代理与 `manifest_url` |
| `UPDATE_MANIFEST_CACHE_UNWRITABLE` | 清单缓存目录不可写（503） | 检查 `<data_dir>/updates` 权限 |
| `UPDATE_UNVERIFIED` | 未配置 `public_key_file`，服务端拒绝信任清单（503） | 配置公钥并重启 |
| `UPDATE_VERIFY_UNAVAILABLE` | 公钥/验签脚本路径不存在，或 Python/cryptography 缺失（503） | 安装依赖、修正 `verifier_python` 与 `verifier_script` |
| `UPDATE_SIGNATURE_INVALID` | 清单被篡改、私钥不匹配、规范化不一致（503） | 用发布工具重新生成并签名；检查 `key_id` 与公钥是否配套 |
| `MANIFEST_PLATFORM_MISSING` | v2 清单里没有请求平台的段（503） | 确认清单包含 `platforms.macos` / `platforms.windows` |
| `MANIFEST_ARTIFACT_MISSING` | v2 平台段缺 `artifacts.dmg`（macOS），或 `setup` / `portable` 都没有（Windows）（503） | 重新发布完整产物并重签 |
| `MANIFEST_UPGRADE_PATH_INVALID` | 仅 v1：`upgrade_path` 元素缺字段、schema/build 非正数、version 非 SemVer（503） | 修正清单的 `upgrade_path` 后重新签名 |
| `INTERMEDIATE_REQUIRED` | 仅 v1：客户端 schema 低于 `schema_min`，需先升到中间版本（200，`update_available=false`） | 按响应中的 `intermediate.version` 引导用户先升级 |
| `INCOMPATIBLE_SCHEMA` | 仅 v1：客户端 schema 高于 `schema_max`，或低于 `schema_min` 且无可用升级路径（200） | v2 已不携带 schema 区间；如仍出现，检查是否误投了 v1 清单 |
| `UPDATE_PACKAGE_MISSING` | 安装前发现待装包不存在（409） | 重新下载并校验 |
| `UPDATE_EXTRACT_FAILED` | zip 解压失败（409） | 检查系统 `tar`/`unzip` 是否可用，或改用单文件包 |
| `UPDATE_PACKAGE_PAYLOAD_MISSING` | zip 内没有与当前程序同名的文件（409） | 打包时保留原程序名 |
| `UPDATE_PACKAGE_UNSUPPORTED` | 安装包既不是可执行文件也不是 zip（409） | 改用可自动替换的包；否则手动安装 |
| `UPDATE_APPLY_UNSUPPORTED` | 程序路径或启动参数含批处理/shell 不支持的字符（500） | 去掉参数中的 `%`、双引号或换行后重试 |
| `UPDATE_APPLY_FAILED` | 替换脚本写入或启动失败（500） | 检查程序目录与 `updates/` 的写权限 |

### 7. 发布侧清单生成

v2 清单由三仓库共用的 `tools/release/manifest_v2.py` 生成并签名；appcast 仍由 `tools/package/update_manifest.py` 生成。`tools/package/publish_oss.sh` 会串起整个流程，参数见 `tools/package/README.md`。发布顺序：上传安装包到 OSS → 生成并签名 `manifest.json` / `appcast.xml` → 上传清单 → 下载预热 → 冒烟验证 `/api/update/check`（无 CDN 层，默认不 purge）。
