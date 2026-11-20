#!/usr/bin/env python3
"""
gen_build — 生成三仓库统一格式的 build 号

格式：YYYY * 100000 + git_rev_list_count（10 位纯整数）
      例：2026 * 100000 + 418 = 2026000418

单调递增保证：
  - 同一 commit 跨机器完全一致（git rev-list --count HEAD 不变）
  - 跨仓库各自独立（git count 各算各的）
  - 跨平台同仓库同 build（不区分 macOS/Windows，用同一份）

强制校验：build 必须严格大于线上 manifest.json
  的 platforms.{mac,windows}.build 最大值。

用法：
  # 取推荐 build（自动校验线上递增）
  python3 tools/release/gen_build.py --product xiaotu --channel stable

  # 离线模式（不校验线上，用于首次发布或内网无外网）
  python3 tools/release/gen_build.py --product xiaotu --offline

  # 同时打印线上 (version, build) 供人核对
  python3 tools/release/gen_build.py --product mote --verbose

exit code：
  0  成功，stdout 打印 build 号（仅数字，便于 shell $() 捕获）
  1  校验失败或参数错误
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import urllib.request
from datetime import datetime, timezone


DEFAULT_BASE = "https://dl.xiaotu.online"


def _die(msg: str) -> None:
    print(f"gen_build: {msg}", file=sys.stderr)
    sys.exit(1)


def _run(cmd: list[str], cwd: str | None = None) -> str:
    """跑命令，返回 stdout（stripped）；失败 die。"""
    try:
        out = subprocess.check_output(cmd, cwd=cwd, stderr=subprocess.PIPE, text=True)
        return out.strip()
    except (subprocess.CalledProcessError, FileNotFoundError) as e:
        _die(f"{' '.join(cmd)!r} 失败：{e}")
    return ""


def _git_count(repo_root: str) -> int:
    """git rev-list --count HEAD → 整数。"""
    raw = _run(["git", "rev-list", "--count", "HEAD"], cwd=repo_root)
    try:
        return int(raw)
    except ValueError:
        _die(f"git rev-list --count HEAD 输出非整数：{raw!r}")
    return 0  # unreachable


def _build_from_components(count: int) -> int:
    """
    build = YYYY * 10^12 + git_rev_list_count（13 位纯整数）

    为什么乘 10^12：
      上一代用 UTC 时间戳 YYYYMMDDHHmm（12 位），最大 209912312359。
      13 位数字天然大于任何 12 位时间戳，一次切换完不回退。

    为什么加 git_count：
      同一 commit 跨机器完全一致（git rev-list --count HEAD 不变），
      跨仓库各自独立，跨平台同仓库同 build。
    """
    year = datetime.now(timezone.utc).year
    return year * 10**12 + count


def _online_max_build(product: str, channel: str, base: str) -> tuple[str, int]:
    """拉线上 manifest.json，返回 (max_version, max_build) 跨 mac/win 两平台。"""
    url = f"{base}/{product}/updates/{channel}/manifest.json"
    try:
        resp = urllib.request.urlopen(url, timeout=10)
        data = json.loads(resp.read().decode("utf-8"))
    except (urllib.error.URLError, json.JSONDecodeError) as e:
        _die(f"无法拉取线上清单 {url}: {e}")

    platforms = data.get("platforms", {})
    if not platforms:
        _die(f"线上 manifest.json 缺少 platforms 段：{url}")

    # v2 manifest 键是 "macos" / "windows"（不是 "mac"/"win"）
    mac = platforms.get("macos")
    win = platforms.get("windows")

    pairs: list[tuple[str, int]] = []
    for label, section in [("macos", mac), ("windows", win)]:
        if section is None:
            continue
        try:
            v = str(section.get("version", ""))
            b = int(section.get("build", 0))
            pairs.append((v, b))
        except (TypeError, ValueError):
            continue

    if not pairs:
        return ("", 0)

    # 取 build 最大那个
    pairs.sort(key=lambda x: x[1], reverse=True)
    return pairs[0]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--product", required=True, choices=["xiaotu", "mote", "devcard"],
                    help="产品名（决定线上 manifest 路径）")
    ap.add_argument("--channel", default="stable", choices=["stable", "beta"],
                    help="发布通道（默认 stable）")
    ap.add_argument("--repo-root", default=os.getcwd(),
                    help="git 仓库根（默认 cwd）")
    ap.add_argument("--base", default=DEFAULT_BASE,
                    help=f"OSS 基础 URL（默认 {DEFAULT_BASE}）")
    ap.add_argument("--offline", action="store_true",
                    help="不校验线上递增（首次发布 / 内网环境）")
    ap.add_argument("--verbose", action="store_true",
                    help="额外打印线上 (version, build) 供核对")
    args = ap.parse_args()

    repo_root = args.repo_root
    if not os.path.isdir(os.path.join(repo_root, ".git")):
        _die(f"{repo_root} 不是 git 仓库（无 .git 目录）")

    count = _git_count(repo_root)
    new_build = _build_from_components(count)

    if not args.offline:
        online_version, online_build = _online_max_build(args.product, args.channel, args.base)
        if new_build <= online_build:
            # git count 没增长 → 这台机器没有新 commit
            # 加 1 保证递增
            new_build = online_build + 1
            print(f"[gen_build] 本地 git count 未增长 → 取 {online_build} + 1 = {new_build}",
                  file=sys.stderr)
        elif args.verbose:
            print(f"[gen_build] 线上：{online_version} build={online_build} → "
                  f"本地：build={new_build}  ✓", file=sys.stderr)

    # stdout 只输出 build 号（纯数字），方便 $(gen_build.py ...) 捕获
    print(new_build)
    return 0


if __name__ == "__main__":
    sys.exit(main())
