#!/usr/bin/env bash
# 从正式图标 xiaotu.icns 生成 Windows .ico。
# 依赖：macOS 自带 sips；python3（Pillow）。
#
# 用法：tools/package/make_icons.sh
# 输入：
#   shell/mac/icon/xiaotu.icns（青绿色底 + "兔"字，正式版）
# 产物：
#   packaging/win/xiaotu.ico

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
src_icns="$repo_root/shell/mac/icon/xiaotu.icns"
out_ico="$repo_root/packaging/win/xiaotu.ico"

if [[ ! -f "$src_icns" ]]; then
  echo "缺少源图标：$src_icns" >&2
  exit 1
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# icns 内部各尺寸独立抽取不稳定，用最大的那张缩放出所有小尺寸。
base="$tmp/base.png"
sips -s format png "$src_icns" --out "$base" >/dev/null
max_size=$(sips -g pixelWidth "$base" 2>/dev/null | awk '/pixelWidth:/{print $2}')

python3 - "$base" "$out_ico" <<'PY'
import os, sys
from PIL import Image

base_path, out_path = sys.argv[1], sys.argv[2]
sizes = [16, 24, 32, 48, 64, 128, 256]

base = Image.open(base_path).convert('RGBA')
# 取最长边缩放，保持正方形
side = min(base.width, base.height)
left = (base.width - side) // 2
top = (base.height - side) // 2
base = base.crop((left, top, left + side, top + side))

imgs = [base.resize((s, s), Image.LANCZOS) for s in sizes]
imgs[0].save(out_path, format='ICO',
             sizes=[(s, s) for s in sizes],
             append_images=imgs[1:])
print(f'  ✓ {out_path}')
PY

echo "生成：$out_ico"
echo "输入：${src_icns}（青绿色底 + \"兔\"字，已作为 macOS App 正式图标）"
