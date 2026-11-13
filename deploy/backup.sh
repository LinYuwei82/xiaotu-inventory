#!/usr/bin/env bash
# 小兔进销存备份统一入口（systemd timer 与 cron 共用，避免两套调用逻辑）。
#
# 环境变量：
#   XIAOTU_BIN           可执行文件，默认 /opt/xiaotu/xiaotu-server
#   XIAOTU_CONFIG        配置文件，默认 /etc/xiaotu/config.toml
#   XIAOTU_BACKUP_MODE   daily（默认，幂等）或 manual（强制执行一次）
#
# daily 模式调用 --backup-daily：当天已成功备份则直接跳过，适合定时任务；
# manual 模式调用 --backup：无条件生成一份，适合人工临检。
#
# 注意：进程内已有每日备份调度。本脚本只给「进程未运行也要备份」或统一由
# 系统调度管理的部署使用；cron 与 systemd timer 二选一，不要同时启用。

set -uo pipefail

BIN="${XIAOTU_BIN:-/opt/xiaotu/xiaotu-server}"
CONFIG="${XIAOTU_CONFIG:-/etc/xiaotu/config.toml}"
MODE="${XIAOTU_BACKUP_MODE:-daily}"

case "$MODE" in
  daily) FLAG="--backup-daily" ;;
  manual) FLAG="--backup" ;;
  *)
    echo "未知的 XIAOTU_BACKUP_MODE：${MODE}（可选 daily / manual）" >&2
    exit 2
    ;;
esac

if [ ! -x "$BIN" ]; then
  echo "找不到可执行文件：${BIN}" >&2
  exit 1
fi
if [ ! -f "$CONFIG" ]; then
  echo "找不到配置文件：${CONFIG}" >&2
  exit 1
fi

exec "$BIN" "$FLAG" --config "$CONFIG"
