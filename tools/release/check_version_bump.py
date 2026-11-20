#!/usr/bin/env python3
"""发布前版本号 / 构建号递增校验（三仓同一份）。

规则（与 guard.py 的 build 递增门禁配套）：
- 本次 VERSION 必须 >= 线上版本；相同版本仅允许构建号前移（同版本重发）；
- 本次 build 必须严格大于线上所有平台的最大 build；
- 线上清单不可读时默认失败；`--allow-missing` 仅用于首次发布。

用法：
  python3 tools/release/check_version_bump.py --build 20261120123000 \
      --manifest-url https://dl.xiaotu.online/xiaotu/updates/stable/manifest.json
  python3 tools/release/check_version_bump.py --build 20261120123000 \
      --manifest-file /tmp/manifest.json
"""
from __future__ import annotations

import argparse
import json
import sys
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def parse_semver(text: str) -> tuple[int, int, int] | None:
    parts = text.strip().split(".")
    if len(parts) != 3 or not all(p.isdigit() for p in parts):
        return None
    return int(parts[0]), int(parts[1]), int(parts[2])


def load_manifest(args: argparse.Namespace) -> dict | None:
    if args.manifest_file:
        path = Path(args.manifest_file)
        if not path.is_file():
            return None
        try:
            return json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            return None
    if not args.manifest_url:
        return None
    try:
        with urllib.request.urlopen(args.manifest_url, timeout=15) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except Exception:
        return None


def remote_versions(manifest: dict) -> tuple[list[tuple[int, int, int]], list[int]]:
    versions: list[tuple[int, int, int]] = []
    builds: list[int] = []
    for section in (manifest.get("platforms") or {}).values():
        if not isinstance(section, dict):
            continue
        parsed = parse_semver(str(section.get("version", "")))
        if parsed:
            versions.append(parsed)
        try:
            builds.append(int(section.get("build", 0)))
        except (TypeError, ValueError):
            pass
    if not versions:
        top = parse_semver(str(manifest.get("version", "")))
        if top:
            versions.append(top)
        try:
            builds.append(int(manifest.get("build", 0)))
        except (TypeError, ValueError):
            pass
    return versions, builds


def main() -> int:
    parser = argparse.ArgumentParser(description="发布前版本号 / 构建号递增校验")
    parser.add_argument("--product", default="", help="产品名（仅用于提示）")
    parser.add_argument("--version-file", default=str(REPO / "VERSION.txt"))
    parser.add_argument("--version", default="", help="覆盖 VERSION.txt 的值")
    parser.add_argument("--build", required=True, help="本次构建号（纯数字）")
    parser.add_argument("--manifest-url", default="")
    parser.add_argument("--manifest-file", default="")
    parser.add_argument("--allow-missing", action="store_true",
                        help="线上清单不可读时放行（仅首次发布）")
    args = parser.parse_args()

    tag = f"[{args.product}] " if args.product else ""
    if not args.build.isdigit():
        print(f"{tag}构建号必须是纯数字：{args.build}")
        return 2

    local_version = args.version.strip()
    if not local_version:
        path = Path(args.version_file)
        if not path.is_file():
            print(f"{tag}找不到版本文件：{path}")
            return 2
        local_version = path.read_text(encoding="utf-8").strip()
    local_semver = parse_semver(local_version)
    if local_semver is None:
        print(f"{tag}VERSION 不是 x.y.z 形式：{local_version}")
        return 2

    manifest = load_manifest(args)
    if manifest is None:
        if args.allow_missing:
            print(f"{tag}线上清单不可读，已按 --allow-missing 放行：{local_version} / {args.build}")
            return 0
        print(f"{tag}线上清单不可读，无法校验版本/构建号递增"
              "（首次发布可加 --allow-missing）")
        return 2

    versions, builds = remote_versions(manifest)
    if not versions:
        print(f"{tag}线上清单缺少 version 字段，无法校验")
        return 2
    remote_version = max(versions)
    remote_build = max(builds) if builds else 0

    if local_semver < remote_version:
        print(f"{tag}版本回退：本地 {local_version} < 线上 {'.'.join(map(str, remote_version))}")
        return 2
    if local_semver > remote_version:
        print(f"{tag}版本递增 OK：{local_version}（线上 {'.'.join(map(str, remote_version))}）")
        return 0
    if int(args.build) <= remote_build:
        print(f"{tag}同版本重发但构建号未前移：{args.build} <= 线上 {remote_build}")
        return 2
    print(f"{tag}同版本重发 OK：{local_version} 构建号 {args.build} > 线上 {remote_build}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
