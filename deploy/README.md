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

## 更新源（网络版 / 私有化）

更新检查由服务端读取本地清单完成，不主动访问外网；清单由 `deploy/update-sync.sh` 从厂商更新源同步：

1. 复制 `deploy/nginx/updates.conf.example` 到 Nginx 配置目录，替换域名与路径后 reload。
2. 在 `/etc/xiaotu/update_signing.pub.pem` 放置 Ed25519 公钥。
3. 安装 `deploy/cron/xiaotu-update-sync` 到 `/etc/cron.d/`，或改用 systemd timer（二选一，不要重复）。
4. 在 `config.toml` 增加：

```toml
[update]
manifest_file = "/srv/xiaotu-updates/macos/stable/latest.json"
channel = "stable"
min_supported_version = "1.3.0"
rollout_percent = 100
```

灰度放行：把清单里的 `rollout_percent` 依次改为 1、10、50、100，每次替换后 purge CDN 的 `latest.json` 与 `appcast.xml`，观察 24 小时；服务端启动时会读取新文件，无需重启。回滚时把清单指回上一版本并保留安装包。
