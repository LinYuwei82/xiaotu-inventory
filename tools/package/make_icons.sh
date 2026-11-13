#!/usr/bin/env bash
# 从候选 A（兔耳 + 条形码）生成 macOS .icns 与 Windows .ico。
# 依赖：macOS 自带 sips / iconutil / python3；不需要 ImageMagick 或手工拖拽。
#
# 用法：tools/package/make_icons.sh
# 产物：
#   shell/mac/icon/xiaotu-a-rabbit-barcode.icns
#   packaging/win/xiaotu-a-rabbit-barcode.ico
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
svg="$repo_root/shell/mac/icon/candidate-a-rabbit-barcode.svg"
out_icns="$repo_root/shell/mac/icon/xiaotu-a-rabbit-barcode.icns"
out_ico="$repo_root/packaging/win/xiaotu-a-rabbit-barcode.ico"

if [[ ! -f "$svg" ]]; then
  echo "缺少 SVG：$svg" >&2
  exit 1
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# sips 直接读 SVG 的 1024×1024 画布，保留透明通道。
base="$tmp/base-1024.png"
sips -s format png "$svg" --out "$base" >/dev/null
sips -z 1024 1024 "$base" >/dev/null

iconset="$tmp/xiaotu.iconset"
mkdir -p "$iconset"
for size in 16 32 64 128 256 512; do
  sips -z "$size" "$size" "$base" --out "$iconset/icon_${size}x${size}.png" >/dev/null
  double=$((size * 2))
  sips -z "$double" "$double" "$base" --out "$iconset/icon_${size}x${size}@2x.png" >/dev/null
done
iconutil -c icns "$iconset" -o "$out_icns"

# iconutil 不会写独立的 64×64 块（只把它并进 32@2x）。macOS 的 ICNS 有 icp6
# 这个 64×64 类型，手动补一块，保证 16/32/64/128/256/512 都有独立尺寸。
sips -z 64 64 "$base" --out "$tmp/icon-64.png" >/dev/null
python3 - "$out_icns" "$tmp/icon-64.png" <<'PY'
import struct
import sys

icns_path, png_path = sys.argv[1], sys.argv[2]
with open(icns_path, "rb") as fh:
    data = bytearray(fh.read())
with open(png_path, "rb") as fh:
    png = fh.read()

# 已有 icp6 就跳过，保证脚本可重复执行。
i = 8
while i + 8 <= len(data):
    kind = bytes(data[i:i + 4])
    length = struct.unpack_from(">I", data, i + 4)[0]
    if kind == b"icp6":
        break
    if length < 8:
        break
    i += length
else:
    i = -1

if i < 0:
    chunk = b"icp6" + struct.pack(">I", 8 + len(png)) + png
    data[8:8] = chunk
    struct.pack_into(">I", data, 4, len(data))
    with open(icns_path, "wb") as fh:
        fh.write(data)
PY

ico_dir="$tmp/ico"
mkdir -p "$ico_dir"
for size in 16 24 32 48 64 128 256; do
  sips -z "$size" "$size" "$base" --out "$ico_dir/icon-${size}.png" >/dev/null
done

python3 - "$ico_dir" "$out_ico" <<'PY'
import os
import struct
import sys

src_dir, out_path = sys.argv[1], sys.argv[2]
sizes = [16, 24, 32, 48, 64, 128, 256]
images = []
for size in sizes:
    with open(os.path.join(src_dir, f"icon-{size}.png"), "rb") as fh:
        images.append((size, fh.read()))

# ICONDIR + ICONDIRENTRY[] + PNG payloads（Vista+ 支持 PNG 压缩的 ICO）
offset = 6 + 16 * len(images)
header = struct.pack("<HHH", 0, 1, len(images))
entries = bytearray()
payload = bytearray()
for size, data in images:
    width = 0 if size >= 256 else size
    height = 0 if size >= 256 else size
    entries += struct.pack("<BBBBHHII", width, height, 0, 0, 1, 32, len(data), offset)
    payload += data
    offset += len(data)

with open(out_path, "wb") as fh:
    fh.write(header)
    fh.write(entries)
    fh.write(payload)
PY

echo "生成：$out_icns"
echo "生成：$out_ico"
