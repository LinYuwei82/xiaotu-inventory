#!/usr/bin/env python3
"""更新清单与 appcast 生成/签名/校验工具。

只依赖 cryptography（Ed25519）。清单签名输入遵循 docs/更新模块技术方案.md：
去掉 signature 字段后按键排序、无空白、UTF-8 编码，再做 Ed25519 签名。

用法：
  python update_manifest.py generate \
      --file build/package/xiaotu-inventory-1.4.2.dmg \
      --version 1.4.2 --build 2026111301 \
      --url https://cdn.example.com/releases/macos/1.4.2/xiaotu-1.4.2.dmg \
      --key update_signing.pem --out latest.json --appcast appcast.xml
  python update_manifest.py verify --manifest latest.json --pub update_signing.pub.pem
  python update_manifest.py verify --manifest latest.json --pub update_signing.pub.pem \
      --file build/package/xiaotu-inventory-1.4.2.dmg
"""

from __future__ import annotations

import argparse
import base64
import datetime as dt
import hashlib
import json
import sys
from pathlib import Path
from typing import Any
from xml.sax.saxutils import escape

try:
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import (
        Ed25519PrivateKey,
        Ed25519PublicKey,
    )
except ImportError as exc:  # pragma: no cover - 环境缺失时给出明确提示
    print("缺少 cryptography：pip install -r tools/package/requirements.txt", file=sys.stderr)
    raise SystemExit(3) from exc


REQUIRED_FIELDS = {
    "manifest_version",
    "key_id",
    "platform",
    "channel",
    "version",
    "build",
    "url",
    "sha256",
    "size",
    "published_at",
    "schema_min",
    "schema_max",
    "api_min",
    "api_max",
    "rollout_percent",
}


def canonical_bytes(body: dict[str, Any]) -> bytes:
    """RFC 8785 在本清单结构上的实现：键排序、无空白、UTF-8。"""
    payload = {key: value for key, value in body.items() if key != "signature"}
    return json.dumps(
        payload, ensure_ascii=False, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")


def load_private_key(path: Path) -> Ed25519PrivateKey:
    key = serialization.load_pem_private_key(path.read_bytes(), password=None)
    if not isinstance(key, Ed25519PrivateKey):
        raise SystemExit("私钥不是 Ed25519")
    return key


def load_public_key(path: Path) -> Ed25519PublicKey:
    key = serialization.load_pem_public_key(path.read_bytes())
    if not isinstance(key, Ed25519PublicKey):
        raise SystemExit("公钥不是 Ed25519")
    return key


def sign_manifest(body: dict[str, Any], private_key: Ed25519PrivateKey) -> dict[str, Any]:
    signed = dict(body)
    signed["signature"] = base64.b64encode(
        private_key.sign(canonical_bytes(body))
    ).decode("ascii")
    return signed


def verify_manifest(body: dict[str, Any], public_key: Ed25519PublicKey) -> None:
    signature = body.get("signature")
    if not isinstance(signature, str) or not signature:
        raise SystemExit("清单缺少 signature")
    missing = REQUIRED_FIELDS - set(body)
    if missing:
        raise SystemExit(f"清单缺少字段：{', '.join(sorted(missing))}")
    public_key.verify(base64.b64decode(signature), canonical_bytes(body))


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def appcast_xml(
    body: dict[str, Any], file_bytes: bytes, private_key: Ed25519PrivateKey
) -> str:
    signature = base64.b64encode(private_key.sign(file_bytes)).decode("ascii")
    published = dt.datetime.strptime(
        body["published_at"], "%Y-%m-%dT%H:%M:%SZ"
    ).replace(tzinfo=dt.timezone.utc)
    pub_date = published.strftime("%a, %d %b %Y %H:%M:%S +0000")
    short_version = escape(body["version"])
    build = int(body["build"])
    url = escape(body["url"])
    length = int(body["size"])
    min_os = escape(body.get("min_os") or "")
    channel = escape(body["channel"])
    title = f"小兔进销存 {short_version}"
    channel_tag = (
        f"\n      <sparkle:channel>{channel}</sparkle:channel>" if channel != "stable" else ""
    )
    min_os_tag = (
        f"\n      <sparkle:minimumSystemVersion>{min_os}</sparkle:minimumSystemVersion>"
        if min_os
        else ""
    )
    return f"""<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle">
  <channel>
    <title>小兔进销存</title>
    <item>
      <title>{title}</title>
      <pubDate>{pub_date}</pubDate>{channel_tag}{min_os_tag}
      <enclosure url="{url}"
                 sparkle:version="{build}"
                 sparkle:shortVersionString="{short_version}"
                 sparkle:edSignature="{signature}"
                 length="{length}"
                 type="application/octet-stream"/>
    </item>
  </channel>
</rss>
"""


def command_generate(args: argparse.Namespace) -> None:
    package = Path(args.file)
    if not package.is_file():
        raise SystemExit(f"安装包不存在：{package}")
    if not args.url.startswith("https://"):
        raise SystemExit("url 必须是 https")
    if args.mandatory and args.reason not in {
        "SECURITY",
        "PROTOCOL",
        "REGULATORY",
        "MIN_SUPPORTED",
    }:
        raise SystemExit("强制更新必须给出四类理由之一")
    published_at = args.published_at or dt.datetime.now(dt.timezone.utc).strftime(
        "%Y-%m-%dT%H:%M:%SZ"
    )
    body: dict[str, Any] = {
        "manifest_version": 1,
        "key_id": args.key_id,
        "platform": args.platform,
        "channel": args.channel,
        "version": args.version,
        "build": int(args.build),
        "min_os": args.min_os or "",
        "url": args.url,
        "sha256": sha256_file(package),
        "size": package.stat().st_size,
        "published_at": published_at,
        "release_notes_url": args.release_notes_url or "",
        "mandatory": bool(args.mandatory),
        "mandatory_reason": args.reason if args.mandatory else None,
        "mandatory_deadline": args.deadline if args.mandatory else None,
        "schema_min": int(args.schema_min),
        "schema_max": int(args.schema_max),
        "api_min": int(args.api_min),
        "api_max": int(args.api_max),
        "rollout_percent": int(args.rollout),
    }
    if args.upgrade_step:
        steps = []
        for raw in args.upgrade_step:
            parts = raw.split(":")
            if len(parts) != 3:
                raise SystemExit("--upgrade-step 格式应为 schema:version:build")
            steps.append(
                {"schema": int(parts[0]), "version": parts[1], "build": int(parts[2])}
            )
        body["upgrade_path"] = steps
    private_key = load_private_key(Path(args.key))
    signed = sign_manifest(body, private_key)
    Path(args.out).write_text(
        json.dumps(signed, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(f"清单已生成：{args.out}（sha256={body['sha256'][:12]}…）")
    if args.appcast:
        Path(args.appcast).write_text(
            appcast_xml(signed, package.read_bytes(), private_key), encoding="utf-8"
        )
        print(f"appcast 已生成：{args.appcast}")


def command_verify(args: argparse.Namespace) -> None:
    body = json.loads(Path(args.manifest).read_text(encoding="utf-8"))
    verify_manifest(body, load_public_key(Path(args.pub)))
    print("清单签名有效")
    if args.file:
        actual = sha256_file(Path(args.file))
        if actual != body["sha256"]:
            raise SystemExit(f"sha256 不符：清单 {body['sha256']}，文件 {actual}")
        print("安装包 sha256 与清单一致")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    gen = sub.add_parser("generate", help="生成并签名清单，可选生成 appcast")
    gen.add_argument("--file", required=True, help="安装包路径")
    gen.add_argument("--version", required=True)
    gen.add_argument("--build", required=True, type=int)
    gen.add_argument("--url", required=True)
    gen.add_argument("--platform", default="macos", choices=["macos", "windows"])
    gen.add_argument("--channel", default="stable", choices=["stable", "beta"])
    gen.add_argument("--min-os", default="")
    gen.add_argument("--release-notes-url", default="")
    gen.add_argument("--published-at", default="")
    gen.add_argument("--mandatory", action="store_true")
    gen.add_argument(
        "--reason",
        default="",
        choices=["SECURITY", "PROTOCOL", "REGULATORY", "MIN_SUPPORTED"],
    )
    gen.add_argument("--deadline", default=None)
    gen.add_argument("--schema-min", required=True, type=int)
    gen.add_argument("--schema-max", required=True, type=int)
    gen.add_argument("--api-min", default=1, type=int)
    gen.add_argument("--api-max", default=1, type=int)
    gen.add_argument("--rollout", default=100, type=int, choices=range(1, 101))
    gen.add_argument(
        "--upgrade-step",
        action="append",
        default=[],
        help="旧 schema 的中间升级步骤，格式 schema:version:build，可重复",
    )
    gen.add_argument("--key-id", default="2026-01")
    gen.add_argument("--key", required=True, help="Ed25519 私钥 PEM")
    gen.add_argument("--out", required=True)
    gen.add_argument("--appcast", default="")
    gen.set_defaults(func=command_generate)

    ver = sub.add_parser("verify", help="校验清单签名与安装包哈希")
    ver.add_argument("--manifest", required=True)
    ver.add_argument("--pub", required=True)
    ver.add_argument("--file", default="")
    ver.set_defaults(func=command_verify)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    args.func(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
