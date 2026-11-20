#!/usr/bin/env python3
"""发布 tag 与构建号：`v<semver>+<seq>`（三仓同一份）。

约定（2026-11 起）：
- 正式发布只从 annotated tag `v1.3.2+240` 触发；
- tag 里的 semver 必须等于 VERSION.txt，序号只增不减，tag 不可删除重打；
- 构建号 = 固定基数 + 序号（基数默认 202700000000，保证大于历史 2026… 构建号，
  已安装客户端不会把新版本判成更旧）。

用法：
  python3 tools/release/release_tag.py parse --tag v1.3.2+240
  python3 tools/release/release_tag.py create --version 1.3.2 --dry-run
  python3 tools/release/release_tag.py create --version 1.3.2 --push
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

DEFAULT_BASE = 202700000000
TAG_RE = re.compile(r"^v(\d+\.\d+\.\d+)\+(\d{1,6})$")
MAX_SEQ = 999999


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def parse_tag(tag: str) -> tuple[str, int]:
    match = TAG_RE.match(tag)
    if not match:
        raise ValueError(f"tag 不符合 v<semver>+<seq>：{tag}（示例 v1.3.2+240）")
    version, seq = match.group(1), int(match.group(2))
    if seq < 1:
        raise ValueError(f"序号必须从 1 开始：{tag}")
    return version, seq


def read_version(path: Path) -> str:
    if not path.is_file():
        raise ValueError(f"找不到版本文件：{path}")
    return path.read_text(encoding="utf-8").strip()


def existing_seq(repo: Path, exclude: str = "") -> int:
    result = subprocess.run(["git", "-C", str(repo), "tag", "-l", "v*+*"],
                            capture_output=True, text=True, check=False)
    seqs = []
    for line in result.stdout.splitlines():
        tag = line.strip()
        if tag == exclude:
            continue
        try:
            seqs.append(parse_tag(tag)[1])
        except ValueError:
            continue
    return max(seqs, default=0)


def cmd_parse(args: argparse.Namespace) -> int:
    try:
        version, seq = parse_tag(args.tag)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    local_version = args.version or read_version(Path(args.version_file))
    if version != local_version:
        print(f"error: tag 版本 {version} 与 VERSION.txt {local_version} 不一致",
              file=sys.stderr)
        return 2
    build = args.base + seq
    if args.ensure_latest:
        max_other = existing_seq(Path(args.repo_root), exclude=args.tag)
        if seq <= max_other:
            print(f"error: 序号未递增：{seq} <= 已发布最大序号 {max_other}"
                  "（tag 不可删除重打，丢弃该 tag 后换新号）", file=sys.stderr)
            return 2
    if args.emit == "sh":
        print(f"VERSION={version}")
        print(f"RELEASE_SEQ={seq}")
        print(f"BUILD={build}")
    else:
        print(f"version={version} seq={seq} build={build}")
    return 0


def cmd_create(args: argparse.Namespace) -> int:
    root = Path(args.repo_root).resolve()
    try:
        version = args.version or read_version(root / "VERSION.txt")
        if not re.match(r"^\d+\.\d+\.\d+$", version):
            raise ValueError(f"VERSION 不是 x.y.z：{version}")
        max_seq = existing_seq(root)
        seq = args.seq if args.seq is not None else max_seq + 1
        if seq <= max_seq:
            raise ValueError(f"序号必须递增：{seq} <= 已发布最大序号 {max_seq}"
                             "（打错的 tag 不可删除重打，只能换新号）")
        if seq > MAX_SEQ:
            raise ValueError(f"序号超过上限 {MAX_SEQ}")
        tag = f"v{version}+{seq}"
        exists = subprocess.run(["git", "-C", str(root), "rev-parse", "-q", "--verify",
                                 f"refs/tags/{tag}"],
                                capture_output=True, text=True, check=False)
        if exists.returncode == 0:
            raise ValueError(f"tag 已存在（序号已消耗）：{tag}")
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    build = args.base + seq
    message = f"发布 {tag}（build {build}）"
    print(f"tag={tag} build={build}")
    if args.dry_run:
        print("（--dry-run：未创建）")
        return 0
    result = subprocess.run(["git", "-C", str(root), "tag", "-a", tag, "-m", message],
                            capture_output=True, text=True, check=False)
    if result.returncode != 0:
        print(f"error: git tag 失败：{result.stderr.strip()}", file=sys.stderr)
        return 2
    print(f"已创建 {tag}")
    if args.push:
        pushed = subprocess.run(["git", "-C", str(root), "push", "origin", tag],
                                capture_output=True, text=True, check=False)
        if pushed.returncode != 0:
            print(f"error: 推送 tag 失败：{pushed.stderr.strip()}", file=sys.stderr)
            return 2
        print(f"已推送 {tag} → origin（CI 将自动发布）")
    else:
        print(f"下一步：git push origin {tag}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="发布 tag / 构建号工具")
    sub = parser.add_subparsers(dest="command", required=True)

    p_parse = sub.add_parser("parse", help="从 tag 解析版本与构建号（CI 用）")
    p_parse.add_argument("--tag", required=True)
    p_parse.add_argument("--version", default="", help="覆盖 VERSION.txt")
    p_parse.add_argument("--version-file", default=str(repo_root() / "VERSION.txt"))
    p_parse.add_argument("--base", type=int, default=DEFAULT_BASE, help="构建号基数")
    p_parse.add_argument("--repo-root", default=str(repo_root()))
    p_parse.add_argument("--ensure-latest", action="store_true",
                         help="要求序号大于仓库内其它发布 tag（CI 使用，需 fetch 全部 tag）")
    p_parse.add_argument("--emit", choices=["text", "sh"], default="text")
    p_parse.set_defaults(func=cmd_parse)

    p_create = sub.add_parser("create", help="分配下一个序号并创建 tag")
    p_create.add_argument("--repo-root", default=str(repo_root()))
    p_create.add_argument("--version", default="", help="默认读 VERSION.txt")
    p_create.add_argument("--seq", type=int, default=None, help="显式序号（默认最大 +1）")
    p_create.add_argument("--base", type=int, default=DEFAULT_BASE)
    p_create.add_argument("--dry-run", action="store_true")
    p_create.add_argument("--push", action="store_true", help="创建后推送到 origin")
    p_create.set_defaults(func=cmd_create)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
