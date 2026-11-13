#!/usr/bin/env bash
# 验证 deploy/backup.sh 的参数拼装与退出码透传。
# 用法：deploy_backup_test.sh <deploy/backup.sh 路径>
set -uo pipefail

SCRIPT=${1:?用法: deploy_backup_test.sh <deploy/backup.sh 路径>}
TMP=$(mktemp -d "${TMPDIR:-/tmp}/xiaotu-deploy-XXXXXX")
trap 'rm -rf "$TMP"' EXIT

cat >"$TMP/fake-server" <<EOF
#!/usr/bin/env bash
printf '%s\n' "\$@" >"$TMP/args.txt"
exit \${FAKE_EXIT:-0}
EOF
chmod +x "$TMP/fake-server"
: >"$TMP/config.toml"

run_case() {
  XIAOTU_BIN="$TMP/fake-server" XIAOTU_CONFIG="$TMP/config.toml" "$@"
}

if ! run_case "$SCRIPT" >/dev/null 2>&1; then
  echo "FAIL: 默认 daily 模式应成功"
  exit 1
fi
grep -q -- '--backup-daily' "$TMP/args.txt" || {
  echo "FAIL: daily 模式应传 --backup-daily"
  cat "$TMP/args.txt"
  exit 1
}
grep -q -- '--config' "$TMP/args.txt" || {
  echo "FAIL: 应传 --config"
  exit 1
}

if ! run_case env XIAOTU_BACKUP_MODE=manual "$SCRIPT" >/dev/null 2>&1; then
  echo "FAIL: manual 模式应成功"
  exit 1
fi
grep -q -- '--backup$' "$TMP/args.txt" || {
  echo "FAIL: manual 模式应传 --backup"
  cat "$TMP/args.txt"
  exit 1
}

run_case env XIAOTU_BACKUP_MODE=unknown "$SCRIPT" >/dev/null 2>&1
[ $? -eq 2 ] || {
  echo "FAIL: 未知模式应返回 2"
  exit 1
}

run_case env FAKE_EXIT=7 "$SCRIPT" >/dev/null 2>&1
[ $? -eq 7 ] || {
  echo "FAIL: 应透传底层退出码"
  exit 1
}

XIAOTU_BIN="$TMP/not-exist" XIAOTU_CONFIG="$TMP/config.toml" "$SCRIPT" >/dev/null 2>&1
[ $? -eq 1 ] || {
  echo "FAIL: 可执行文件缺失应返回 1"
  exit 1
}

echo "deploy_backup_test 通过"
