# 部署补充：每日备份

服务端进程内已带每日备份调度（启动时检查一次、之后每 30 分钟检查），正常情况下无需额外的定时任务。本文的 systemd timer / cron 适合希望「进程未运行时也能备份」或统一由系统调度管理的部署。

## 行为

- 入口：`xiaotu-server --backup-daily --config /etc/xiaotu/config.toml`
- 幂等：当天已有成功备份时直接退出（输出「跳过每日备份」），不会重复生成。
- 并发安全：备份目录内的 `.backup.lock` 互斥；进程内调度与系统定时任务同时触发时只有一方执行。
- 保留策略：清理 `backups/app-<时间戳>-daily.db`，保留最近 30 份日备份，并在每个自然月额外保留 1 份（该月最新）。`manual`、`pre-v*`（迁移前）、`pre-restore-*`（恢复前安全备份）不参与清理。
- 状态文件：`<数据目录>/backups/backup-status.json`，记录最近一次备份时间、结果、大小与失败原因；`/api/health` 的 `backup` 字段读取同一来源。
- 日志：`<数据目录>/logs/app.log`（或 `[log] dir` 配置的目录）。

## systemd timer（推荐）

```bash
sudo cp deploy/systemd/xiaotu-backup.service /etc/systemd/system/
sudo cp deploy/systemd/xiaotu-backup.timer /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now xiaotu-backup.timer
systemctl list-timers xiaotu-backup.timer
```

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
