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

# macOS 的 sips 直接用 -s format png 读 .icns 时只会导出其中最小的一档（16×16），
# 再放大就整片模糊。这里先用 iconutil 解成 iconset，取最大的一档作为母版。
iconset="$tmp/icon.iconset"
mkdir -p "$iconset"
iconutil -c iconset "$src_icns" -o "$iconset"

base=""
best=0
for f in "$iconset"/*.png; do
  [[ -f "$f" ]] || continue
  w=$(sips -g pixelWidth "$f" 2>/dev/null | awk '/pixelWidth:/{print $2}')
  if [[ -n "${w:-}" && "$w" -gt "$best" ]]; then
    best="$w"
    base="$f"
  fi
done

if [[ -z "$base" || "$best" -lt 256 ]]; then
  echo "无法从 $src_icns 取到 256 以上的图（最大 ${best:-0}px）" >&2
  exit 1
fi

echo "母版：$(basename "$base") ${best}×${best}"

python3 - "$base" "$out_ico" <<'PY'
import sys
from PIL import Image

base_path, out_path = sys.argv[1], sys.argv[2]
sizes = [16, 24, 32, 48, 64, 128, 256]

base = Image.open(base_path).convert('RGBA')
side = min(base.width, base.height)
left = (base.width - side) // 2
top = (base.height - side) // 2
base = base.crop((left, top, left + side, top + side))

# 用最大母版一次性产出所有尺寸，Pillow 会为每个尺寸写入独立帧；
# 单帧 ICO 在资源管理器/任务栏放大显示时会被拉伸成马赛克。
base.save(out_path, format='ICO', sizes=[(s, s) for s in sizes])
print(f'  ✓ {out_path}')
PY

echo "生成：$out_ico"
echo "输入：${src_icns}（青绿色底 + \"兔\"字，已作为 macOS App 正式图标）"
