# 部署补充：每日备份

服务端进程内已带每日备份调度（启动时检查一次、之后每 30 分钟检查），正常情况下无需额外的定时任务。本文的 systemd timer / cron 适合希望「进程未运行时也能备份」或统一由系统调度管理的部署。

## 行为

- 统一入口：`deploy/backup.sh`（内部调用 `xiaotu-server --backup-daily --config ...`），cron 与 systemd timer 都只调用它，避免两套命令漂移。
- 幂等：当天已有成功备份时直接退出（输出「跳过每日备份」），不会重复生成。
- 手动立即备份：`XIAOTU_BACKUP_MODE=manual deploy/backup.sh`（底层 `--backup`，无条件生成一份）。
- 并发安全：备份目录内的 `.backup.lock` 互斥；进程内调度与系统定时任务同时触发时只有一方执行，抢锁失败返回非零。
- 保留策略：清理 `backups/app-<时间戳>-daily.db`，保留最近 30 份日备份，并在每个自然月额外保留 1 份（该月最新）。`manual`、`pre-v*`（迁移前）、`pre-restore-*`（恢复前安全备份）不参与清理。
- 生成后校验：备份库自记录的 schema 版本必须与源库一致且不高于程序支持版本，否则该备份判定不可用并写入状态与日志。
- 状态文件：`<数据目录>/backups/backup-status.json`，分别记录最近成功与最近失败；`/api/health` 的 `backup` 字段读取同一来源（只返回文件名，不含目录）。
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
# 部署侧同步到本机的清单文件（服务端只读，不主动访问外网）
manifest_file = "/srv/xiaotu-updates/macos/stable/latest.json"
channel = "stable"                  # stable / beta
min_supported_version = "1.3.0"     # 低于此版本强制升级（理由 MIN_SUPPORTED）
rollout_percent = 100               # 灰度上限，实际取 min(清单, 本值)
public_key_file = "/etc/xiaotu/update_signing.pub.pem"
verifier_python = "/opt/xiaotu/tools/package/.venv/bin/python"
verifier_script = "/opt/xiaotu/tools/package/update_manifest.py"
```

- `manifest_file` 每次请求重新读取，替换文件后无需重启。
- 其余配置在进程启动时读取，修改后需重启服务。
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

使用 `deploy/update-sync.sh`，不要手工 `cp`：

```bash
XIAOTU_UPDATE_SOURCE=https://updates.example.com/macos/stable/latest.json \
XIAOTU_UPDATE_TARGET=/srv/xiaotu-updates/macos/stable/latest.json \
XIAOTU_UPDATE_APPCAST_SOURCE=https://updates.example.com/macos/stable/appcast.xml \
XIAOTU_UPDATE_APPCAST_TARGET=/srv/xiaotu-updates/macos/stable/appcast.xml \
XIAOTU_UPDATE_PUBKEY=/etc/xiaotu/update_signing.pub.pem \
XIAOTU_UPDATE_PYTHON=/opt/xiaotu/tools/package/.venv/bin/python \
XIAOTU_UPDATE_TOOL=/opt/xiaotu/tools/package/update_manifest.py \
/opt/xiaotu/deploy/update-sync.sh
```

- 脚本先下载到同目录临时文件、验签通过后再原子替换，服务端不会读到半个文件。
- 定时执行：安装 `deploy/cron/xiaotu-update-sync` 到 `/etc/cron.d/`，或改用 systemd timer，**二选一**。
- 若 `XIAOTU_UPDATE_PUBKEY` 为空，脚本不验签；生产环境必须配置。

### 5. 灰度放行与回滚

清单是签名的，**不能直接改 `rollout_percent`**（会导致签名失效）。使用 `resign` 只改放量比例并重新签名，其余字段原样保留：

```bash
# 1% → 10% → 50% → 100%
tools/package/.venv/bin/python tools/package/update_manifest.py resign \
  --manifest build/package/latest.json \
  --key ~/keys/update_signing.pem \
  --rollout 10
```

- 不加 `--out` 时原地覆盖输入清单；建议先归档原始清单。
- `resign` 只接受 `--rollout`（1–100）与 `--channel`（stable/beta），版本、构建号、URL、sha256、强制理由等不可在重签时改写。
- 重签后的清单可直接被 `verify` 与 core 解析器接受；同一输入连续重签同一值输出一致（Ed25519 确定性签名，`published_at` 沿用原值，不参与刷新）。

放量步骤：1% → 10% → 50% → 100%，每档观察至少 24 小时；替换清单与 appcast 后 purge CDN，确认 `/api/update/check` 返回符合预期。服务端读取 `min(清单 rollout, 配置 rollout_percent)`。

回滚：

1. 每次发布保留已签名清单与 appcast 归档（如 `releases/macos/1.4.2/latest.json`）。
2. 把上一版归档的 `latest.json`、`appcast.xml` 原子替换回更新源并 purge。
3. 确认 `/api/update/check` 返回上一版本；安装包保留至少 3 个 stable 版本。
4. 回滚包若重新打包，构建号必须递增，否则客户端会判为“无更新”。

### 6. 常见错误码

| 错误码 | 出现原因 | 处理 |
| --- | --- | --- |
| `UPDATE_NOT_CONFIGURED` | 未配置 `manifest_file`；返回 200 且无更新 | 补齐配置并重启 |
| `UPDATE_UNVERIFIED` | 未配置 `public_key_file`，服务端拒绝信任清单（503） | 配置公钥并重启 |
| `UPDATE_VERIFY_UNAVAILABLE` | 公钥/验签脚本路径不存在，或 Python/cryptography 缺失（503） | 安装依赖、修正 `verifier_python` 与 `verifier_script` |
| `UPDATE_SIGNATURE_INVALID` | 清单被篡改、私钥不匹配、规范化不一致（503） | 用发布工具重新生成并签名；检查 `key_id` 与公钥是否配套 |
| `MANIFEST_UPGRADE_PATH_INVALID` | `upgrade_path` 元素缺字段、schema/build 非正数、version 非 SemVer（503） | 修正清单的 `upgrade_path` 后重新签名 |
| `INTERMEDIATE_REQUIRED` | 客户端 schema 低于 `schema_min`，需先升到中间版本（200，`update_available=false`） | 按响应中的 `intermediate.version` 引导用户先升级 |
| `INCOMPATIBLE_SCHEMA` | 客户端 schema 高于 `schema_max`，或低于 `schema_min` 且无可用升级路径（200） | 检查版本是否过旧/过新；必要时补充 `upgrade_path` 或人工迁移 |

### 7. 发布侧清单生成

清单、appcast 与签名统一由 `tools/package/update_manifest.py` 生成，参数见 `tools/package/README.md`。发布顺序：上传安装包到 CDN → 生成并签名清单/appcast → 原子替换更新源 → purge → 冒烟验证 `/api/update/check`。
