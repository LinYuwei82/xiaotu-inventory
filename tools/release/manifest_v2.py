#!/usr/bin/env python3
"""更新清单 Schema v2 统一生成/签名/验签工具（三仓库同步同一份）。

设计稿见 mote/docs/更新清单Schema-v2.md。只依赖 Python 标准库：
Ed25519 为内嵌的 RFC 8032 纯 Python 实现（签名约 0.1s，发布场景可接受），
不引入 cryptography，保证 macOS / Windows / CI 任意环境可直接运行。

签名规则与小兔 v1 工具（tools/package/update_manifest.py）完全一致：
去掉 signature 字段 → 按键排序、无空白、UTF-8 → Ed25519 → base64。

用法：
  python manifest_v2.py generate --product mote --channel stable \
      --platform windows --version 1.0.2 --build 202611180530 --min-os 10.0 \
      --artifact portable=build/.../mote-1.0.2-win64-portable.zip \
      --artifact setup=build/.../mote-1.0.2-win64-setup.exe \
      --notes-url https://dl.xiaotu.online/mote/release-notes-1.0.2.html \
      --key ~/.config/mote-updates/ed25519_seed.b64 \
      --prev dist/manifest.json --out dist/manifest.json
  python manifest_v2.py verify --manifest dist/manifest.json \
      --pub ~/.config/mote-updates/ed25519_pub.pem \
      --artifact portable=path/to/zip
  python manifest_v2.py selftest

私钥文件支持两种格式（自动识别）：
  - Ed25519 PKCS#8 PEM（openssl genpkey 产物，如小兔 update_signing_2026.pem）
  - 32 字节原始种子（base64 / hex / 二进制，如 Mote 的 ed25519_seed.b64）
"""

from __future__ import annotations

import argparse
import base64
import datetime as dt
import hashlib
import json
import re
import sys
import tempfile
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

# Windows 控制台默认 cp1252/GBK，输出 ✓ 会直接抛 UnicodeEncodeError
for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        try:
            _stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass

SCHEMA_VERSION = 2
DEFAULT_BASE = "https://dl.xiaotu.online"
PRODUCTS = ("devcard", "xiaotu", "mote")
KEY_ID_DEFAULT = "2026-11"
MANDATORY_REASONS = ("SECURITY", "PROTOCOL", "REGULATORY", "MIN_SUPPORTED")

# v2 安装包命名：<product>-<version>-<os>-<arch>-<type>.<ext>
ROLE_EXT = {
    "dmg": "macos-arm64.dmg",
    "setup": "win64-setup.exe",
    "portable": "win64-portable.zip",
}
PLATFORM_ROLES = {
    "macos": ("dmg",),
    "windows": ("setup", "portable"),
}
VERSION_RE = re.compile(r"^\d+\.\d+\.\d+$")


class ManifestError(Exception):
    pass


def die(msg: str) -> None:
    raise ManifestError(msg)


# ---------------------------------------------------------------- Ed25519（RFC 8032）

_P = 2 ** 255 - 19
_L = 2 ** 252 + 27742317777372353535851937790883648493
_INV_P = pow(2, _P - 2, _P)


def _inv(x: int) -> int:
    return pow(x, _P - 2, _P)


_D = -121665 * _inv(121666) % _P
_I = pow(2, (_P - 1) // 4, _P)


def _xrecover(y: int) -> int:
    xx = (y * y - 1) * _inv(_D * y * y + 1)
    x = pow(xx, (_P + 3) // 8, _P)
    if (x * x - xx) % _P != 0:
        x = (x * _I) % _P
    if x % 2 != 0:
        x = _P - x
    return x


_By = 4 * _inv(5) % _P
_Bx = _xrecover(_By)
# 扩展齐次坐标 (X, Y, Z, T)，T = XY/Z
_B = (_Bx % _P, _By % _P, 1, (_Bx * _By) % _P)
_IDENT = (0, 1, 1, 0)


def _add(p1: Tuple[int, int, int, int], p2: Tuple[int, int, int, int]):
    x1, y1, z1, t1 = p1
    x2, y2, z2, t2 = p2
    a = (y1 - x1) * (y2 - x2) % _P
    b = (y1 + x1) * (y2 + x2) % _P
    c = 2 * t1 * t2 * _D % _P
    d = 2 * z1 * z2 % _P
    e = b - a
    f = d - c
    g = d + c
    h = b + a
    return (e * f % _P, g * h % _P, f * g % _P, e * h % _P)


def _mult(point, e: int):
    result = _IDENT
    while e > 0:
        if e & 1:
            result = _add(result, point)
        point = _add(point, point)
        e >>= 1
    return result


def _compress(point) -> bytes:
    x, y, z, _ = point
    zi = _inv(z)
    x = x * zi % _P
    y = y * zi % _P
    return int.to_bytes(y | ((x & 1) << 255), 32, "little")


def _decompress(data: bytes):
    if len(data) != 32:
        return None
    y = int.from_bytes(data, "little")
    sign = y >> 255
    y &= (1 << 255) - 1
    if y >= _P:
        return None
    xx = (y * y - 1) * _inv(_D * y * y + 1) % _P
    x = pow(xx, (_P + 3) // 8, _P)
    if (x * x - xx) % _P != 0:
        x = (x * _I) % _P
    if (x * x - xx) % _P != 0:
        return None
    if (x & 1) != sign:
        x = _P - x
    return (x, y, 1, x * y % _P)


def _secret_expand(seed: bytes) -> Tuple[int, bytes]:
    if len(seed) != 32:
        die("Ed25519 种子必须是 32 字节")
    h = hashlib.sha512(seed).digest()
    a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8
    a |= 1 << 254
    return a, h[32:]


def ed25519_pubkey(seed: bytes) -> bytes:
    a, _ = _secret_expand(seed)
    return _compress(_mult(_B, a))


def ed25519_sign(seed: bytes, msg: bytes) -> bytes:
    a, prefix = _secret_expand(seed)
    pub = _compress(_mult(_B, a))
    r = int.from_bytes(hashlib.sha512(prefix + msg).digest(), "little") % _L
    r_point = _compress(_mult(_B, r))
    h = int.from_bytes(
        hashlib.sha512(r_point + pub + msg).digest(), "little"
    ) % _L
    s = (r + h * a) % _L
    return r_point + int.to_bytes(s, 32, "little")


def ed25519_verify(pub: bytes, msg: bytes, sig: bytes) -> bool:
    if len(pub) != 32 or len(sig) != 64:
        return False
    a_point = _decompress(pub)
    if a_point is None:
        return False
    r_point = _decompress(sig[:32])
    if r_point is None:
        return False
    s = int.from_bytes(sig[32:], "little")
    if s >= _L:
        return False
    h = int.from_bytes(
        hashlib.sha512(sig[:32] + pub + msg).digest(), "little"
    ) % _L
    left = _mult(_B, s)
    right = _add(r_point, _mult(a_point, h))
    return _compress(left) == _compress(right)


# ---------------------------------------------------------------- 密钥文件解析

_PKCS8_PREFIX = bytes.fromhex("302e020100300506032b657004220420")
_SPKI_PREFIX = bytes.fromhex("302a300506032b6570032100")


def _pem_der_body(text: str) -> bytes:
    b64 = "".join(line for line in text.splitlines() if not line.startswith("-----"))
    try:
        return base64.b64decode("".join(b64.split()))
    except Exception as exc:
        die(f"PEM 内容无法解码：{exc}")


def load_private_seed(path: Path) -> bytes:
    """从 PEM（PKCS#8）或裸种子文件（base64 / hex / 二进制）读出 32 字节种子。"""
    raw = path.read_bytes()
    text = raw.decode("ascii", "ignore").strip()
    if "-----BEGIN" in text:
        der = _pem_der_body(text)
        if der.startswith(_PKCS8_PREFIX) and len(der) == len(_PKCS8_PREFIX) + 32:
            return der[-32:]
        die("私钥不是 Ed25519 PKCS#8 PEM（期望 32 字节种子结构）")
    if len(raw) == 32:
        return raw
    try:
        decoded = base64.b64decode(text, validate=True)
        if len(decoded) == 32:
            return decoded
    except Exception:
        pass
    try:
        decoded = bytes.fromhex(text)
        if len(decoded) == 32:
            return decoded
    except Exception:
        pass
    die(f"私钥格式无法识别（需要 Ed25519 PKCS#8 PEM 或 32 字节种子）：{path}")


def load_public_bytes(path: Path) -> bytes:
    """从 PEM（SPKI）或裸公钥文件（base64 / hex / 二进制）读出 32 字节公钥。"""
    raw = path.read_bytes()
    text = raw.decode("ascii", "ignore").strip()
    if "-----BEGIN" in text:
        der = _pem_der_body(text)
        if der.startswith(_SPKI_PREFIX) and len(der) == len(_SPKI_PREFIX) + 32:
            return der[-32:]
        die("公钥不是 Ed25519 SPKI PEM（期望 32 字节公钥结构）")
    if len(raw) == 32:
        return raw
    try:
        decoded = base64.b64decode(text, validate=True)
        if len(decoded) == 32:
            return decoded
    except Exception:
        pass
    try:
        decoded = bytes.fromhex(text)
        if len(decoded) == 32:
            return decoded
    except Exception:
        pass
    die(f"公钥格式无法识别（需要 Ed25519 SPKI PEM 或 32 字节公钥）：{path}")


def write_pem_pair(seed: bytes, dir_path: Path, stem: str) -> Tuple[Path, Path]:
    """把种子导出成标准 PKCS#8 / SPKI PEM（客户端编译 trust anchor 用）。"""
    def b64_lines(data: bytes) -> str:
        enc = base64.b64encode(data).decode("ascii")
        return "\n".join(enc[i:i + 64] for i in range(0, len(enc), 64))

    priv = dir_path / f"{stem}.pem"
    pub = dir_path / f"{stem}_pub.pem"
    priv_der = _PKCS8_PREFIX + seed
    pub_der = _SPKI_PREFIX + ed25519_pubkey(seed)
    priv.write_text(
        "-----BEGIN PRIVATE KEY-----\n" + b64_lines(priv_der)
        + "\n-----END PRIVATE KEY-----\n",
        encoding="ascii",
    )
    pub.write_text(
        "-----BEGIN PUBLIC KEY-----\n" + b64_lines(pub_der)
        + "\n-----END PUBLIC KEY-----\n",
        encoding="ascii",
    )
    return priv, pub


# ---------------------------------------------------------------- 清单结构与签名

TOP_FIELDS = {"manifest_version", "product", "channel", "key_id", "updated_at",
              "platforms", "signature"}


def canonical_bytes(body: Dict[str, Any]) -> bytes:
    """去掉 signature 后按键排序、无空白、UTF-8（与 v1 小兔工具同一算法）。"""
    payload = {k: v for k, v in body.items() if k != "signature"}
    return json.dumps(payload, ensure_ascii=False, sort_keys=True,
                      separators=(",", ":")).encode("utf-8")


def sign_manifest(body: Dict[str, Any], seed: bytes) -> Dict[str, Any]:
    signed = dict(body)
    signed["signature"] = base64.b64encode(
        ed25519_sign(seed, canonical_bytes(body))
    ).decode("ascii")
    return signed


def verify_manifest(body: Dict[str, Any], pub: bytes) -> None:
    signature = body.get("signature")
    if not isinstance(signature, str) or not signature:
        die("清单缺少 signature")
    missing = TOP_FIELDS - set(body)
    if missing:
        die(f"清单缺少字段：{', '.join(sorted(missing))}")
    if not ed25519_verify(pub, canonical_bytes(body), base64.b64decode(signature)):
        die("清单签名验证失败（公钥不匹配或清单被改动）")


def resign_manifest(body: Dict[str, Any], seed: bytes, rollout=None,
                    platform: str = "all",
                    notes_url=None) -> Dict[str, Any]:
    """改 rollout_percent 和/或 notes_url 后整体重签。

    灰度放量：只改 platforms.<platform>.rollout_percent；
    补发说明页：只改 platforms.<platform>.notes_url（绝对 http(s) 地址）。
    platform="all" 表示同时改所有平台段。版本、产物 URL / size / sha256
    与其他字段一律保持原样；输入必须是已签名的 v2 清单。
    """
    if body.get("manifest_version") != SCHEMA_VERSION:
        die(f"输入清单不是 v2（manifest_version={body.get('manifest_version')}）")
    if not isinstance(body.get("signature"), str) or not body["signature"]:
        die("输入清单缺少 signature，拒绝重签")
    platforms = body.get("platforms")
    if not isinstance(platforms, dict) or not platforms:
        die("清单缺少 platforms")
    if rollout is None and notes_url is None:
        die("resign 至少需要 --rollout 或 --notes-url 之一")
    if rollout is not None and not 1 <= int(rollout) <= 100:
        die("rollout 必须在 1–100 之间")
    if notes_url is not None and not re.match(r"^https?://", str(notes_url)):
        die(f"notes_url 必须是绝对 http(s) 地址：{notes_url}")
    targets = sorted(platforms) if platform == "all" else [platform]
    for name in targets:
        if not isinstance(platforms.get(name), dict):
            die(f"清单缺少 platforms.{name}")

    updated = json.loads(json.dumps(body))
    updated.pop("signature", None)
    for name in targets:
        if rollout is not None:
            updated["platforms"][name]["rollout_percent"] = int(rollout)
        if notes_url is not None:
            updated["platforms"][name]["notes_url"] = str(notes_url)
    if notes_url is not None:
        # 清单内容变了，时间戳随之更新；rollout 放量不算内容变化，保持原值
        updated["updated_at"] = now_iso()
    signed = sign_manifest(updated, seed)
    verify_manifest(signed, ed25519_pubkey(seed))
    return signed


def command_resign(args: argparse.Namespace) -> int:
    path = Path(args.manifest)
    body = json.loads(path.read_text(encoding="utf-8"))
    signed = resign_manifest(body, load_private_seed(Path(args.key)), args.rollout,
                             args.platform, args.notes_url)
    out = Path(args.out) if args.out else path
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(
        json.dumps(signed, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    changes = []
    if args.rollout is not None:
        changes.append(f"rollout={args.rollout}%")
    if args.notes_url is not None:
        changes.append(f"notes_url={args.notes_url}")
    print(f"已重签：{out}（{' · '.join(changes)} · {args.platform}）")
    return 0


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def v2_artifact_name(product: str, version: str, role: str) -> str:
    return f"{product}-{version}-{ROLE_EXT[role]}"


def check_artifact_name(name: str, product: str, version: str, role: str) -> None:
    want = v2_artifact_name(product, version, role)
    if name != want:
        die(f"{role} 远端文件名不符合 v2 命名规则：{name}\n    应为 {want}")


def check_artifact_entry(entry: Any, label: str) -> Tuple[str, int, str]:
    if not isinstance(entry, dict):
        die(f"artifacts.{label} 不是对象")
    url = str(entry.get("url", ""))
    size = entry.get("size")
    sha = str(entry.get("sha256", ""))
    if not url.startswith("https://"):
        die(f"artifacts.{label}.url 必须是 https：{url}")
    if not isinstance(size, int) or size <= 0:
        die(f"artifacts.{label}.size 非法：{size!r}")
    if not re.fullmatch(r"[0-9a-f]{64}", sha):
        die(f"artifacts.{label}.sha256 不是 64 位小写十六进制")
    return url, size, sha


def now_iso() -> str:
    return dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def build_platform_section(args: argparse.Namespace) -> Dict[str, Any]:
    version = args.version
    if not VERSION_RE.fullmatch(version):
        die(f"版本号必须是 x.y.z：{version}")
    build = int(args.build)
    if build <= 0:
        die(f"{args.platform} 平台必须提供 --build（>0，纯数字单调递增）")

    artifacts: Dict[str, Any] = {}
    for item in args.artifact or []:
        role, sep, path_str = item.partition("=")
        if not sep or role not in ROLE_EXT:
            die(f"--artifact 格式应为 role=path，role ∈ {sorted(ROLE_EXT)}：{item}")
        if args.platform not in PLATFORM_ROLES or role not in PLATFORM_ROLES[args.platform]:
            die(f"{args.platform} 平台不接受产物类型 {role}（允许 {PLATFORM_ROLES[args.platform]}）")
        path = Path(path_str)
        if not path.is_file():
            die(f"{role} 产物不存在：{path}")
        remote = v2_artifact_name(args.product, version, role)
        url = f"{args.base.rstrip('/')}/{args.product}/{remote}"
        artifacts[role] = {
            "url": url,
            "size": path.stat().st_size,
            "sha256": sha256_file(path),
        }

    if args.platform == "macos" and "dmg" not in artifacts:
        die("macos 平台缺少必需产物 dmg")
    if args.platform == "windows" and not ("setup" in artifacts or "portable" in artifacts):
        die("windows 平台至少需要 setup 或 portable 之一")

    section: Dict[str, Any] = {
        "version": version,
        "build": build,
        "min_os": args.min_os or "",
        "artifacts": artifacts,
    }
    if args.notes_url:
        if not args.notes_url.startswith("https://"):
            die("notes_url 必须是 https 绝对地址")
        section["notes_url"] = args.notes_url
    rollout = int(args.rollout)
    if not 1 <= rollout <= 100:
        die("rollout 必须在 1–100 之间")
    section["rollout_percent"] = rollout
    if args.mandatory:
        if args.mandatory_reason not in MANDATORY_REASONS:
            die(f"强制更新必须给出理由之一：{MANDATORY_REASONS}")
        section["mandatory"] = True
        section["mandatory_reason"] = args.mandatory_reason
        if args.mandatory_deadline:
            section["mandatory_deadline"] = args.mandatory_deadline
    return section


def command_generate(args: argparse.Namespace) -> int:
    seed = load_private_seed(Path(args.key))
    section = build_platform_section(args)

    body: Dict[str, Any] = {}
    if args.prev:
        prev_path = Path(args.prev)
        if not prev_path.is_file():
            die(f"--prev 清单不存在：{prev_path}")
        body = json.loads(prev_path.read_text(encoding="utf-8"))
        if body.get("manifest_version") != SCHEMA_VERSION:
            die(f"--prev 清单不是 v2（manifest_version={body.get('manifest_version')}）")
        body.pop("signature", None)
        if body.get("product") != args.product or body.get("channel") != args.channel:
            die("--prev 清单的 product/channel 与本次参数不一致")
    else:
        body = {
            "manifest_version": SCHEMA_VERSION,
            "product": args.product,
            "channel": args.channel,
        }
    if args.key_id:
        body["key_id"] = args.key_id
    elif "key_id" not in body:
        body["key_id"] = KEY_ID_DEFAULT
    body.setdefault("product", args.product)
    body.setdefault("channel", args.channel)
    body["updated_at"] = now_iso()
    body.setdefault("platforms", {})
    body["platforms"][args.platform] = section

    signed = sign_manifest(body, seed)
    pub = ed25519_pubkey(seed)
    verify_manifest(signed, pub)  # 自校验：签名必须能被自己验证
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(
        json.dumps(signed, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(f"清单已生成并签名：{out}")
    for role, entry in section["artifacts"].items():
        print(f"  {role:8s} {entry['url']}  {entry['size']} B  sha256={entry['sha256'][:16]}…")
    return 0


def command_verify(args: argparse.Namespace) -> int:
    path = Path(args.manifest)
    body = json.loads(path.read_text(encoding="utf-8"))
    if args.pub:
        pub = load_public_bytes(Path(args.pub))
    elif args.key:
        pub = ed25519_pubkey(load_private_seed(Path(args.key)))
    else:
        die("verify 需要 --pub 或 --key 之一")
    verify_manifest(body, pub)
    print(f"清单签名有效：{path}")
    print(f"  product={body.get('product')} channel={body.get('channel')} "
          f"key_id={body.get('key_id')} updated_at={body.get('updated_at')}")
    platforms = body.get("platforms") or {}
    for name, section in sorted(platforms.items()):
        arts = " ".join(f"{role}({entry.get('size')}B)" for role, entry in sorted((section.get("artifacts") or {}).items()))
        print(f"  {name:8s} version={section.get('version')} build={section.get('build')}  {arts}")
    for item in args.artifact or []:
        role, sep, path_str = item.partition("=")
        if not sep or role not in ROLE_EXT:
            die(f"--artifact 格式应为 role=path：{item}")
        entry = None
        for section in platforms.values():
            entry = (section.get("artifacts") or {}).get(role)
            if entry:
                break
        if not entry:
            die(f"清单中没有 {role} 产物")
        path2 = Path(path_str)
        if not path2.is_file():
            die(f"产物不存在：{path2}")
        actual = sha256_file(path2)
        if actual != entry.get("sha256"):
            die(f"{role} sha256 不符：清单 {entry.get('sha256')}，文件 {actual}")
        if path2.stat().st_size != entry.get("size"):
            die(f"{role} size 不符：清单 {entry.get('size')}，文件 {path2.stat().st_size}")
        print(f"  {role} 与本地产物一致（{path2.name}）")
    return 0


def command_pubkey(args: argparse.Namespace) -> int:
    seed = load_private_seed(Path(args.key))
    print(f"公钥（raw hex，32 字节）：{ed25519_pubkey(seed).hex()}")
    if args.export:
        priv, pub = write_pem_pair(seed, Path(args.export), Path(args.key).stem)
        print(f"已导出：{priv}")
        print(f"        {pub}")
    return 0


# ---------------------------------------------------------------- 自检

_SELFTEST_SEED = bytes.fromhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")
_SELFTEST_PUB = bytes.fromhex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a")
_SELFTEST_MSG = b"manifest-v2-selftest"
_SELFTEST_SIG = bytes.fromhex(
    "e642309f0f791045d3d8e112e507c33f89b5d7b0cb79b4ca58412cc4f58f2c76"
    "2e2f1cabf5a2fac05e633b5203880ba8a9de7601474722d7e15f31463660860d"
)


def cmd_selftest(args: argparse.Namespace) -> int:
    n = 0

    # 1) RFC 8032 风格固定向量（与 cryptography 库交叉验证后固化）
    assert ed25519_pubkey(_SELFTEST_SEED) == _SELFTEST_PUB; n += 1
    assert ed25519_sign(_SELFTEST_SEED, _SELFTEST_MSG) == _SELFTEST_SIG; n += 1
    assert ed25519_verify(_SELFTEST_PUB, _SELFTEST_MSG, _SELFTEST_SIG); n += 1

    # 2) 篡改必须失败
    assert not ed25519_verify(_SELFTEST_PUB, _SELFTEST_MSG + b"x", _SELFTEST_SIG); n += 1
    bad_sig = bytearray(_SELFTEST_SIG); bad_sig[0] ^= 1
    assert not ed25519_verify(_SELFTEST_PUB, _SELFTEST_MSG, bytes(bad_sig)); n += 1
    bad_pub = bytearray(_SELFTEST_PUB); bad_pub[0] ^= 1
    assert not ed25519_verify(bytes(bad_pub), _SELFTEST_MSG, _SELFTEST_SIG); n += 1

    # 3) canonical_bytes：排序、无空白、剔除 signature
    body = {"b": 1, "a": "x", "signature": "sig"}
    assert canonical_bytes(body) == b'{"a":"x","b":1}'; n += 1

    # 4) PEM 解析：种子 → PKCS#8 → 读回一致；公钥 SPKI 同理
    with tempfile.TemporaryDirectory() as td:
        priv, pub = write_pem_pair(_SELFTEST_SEED, Path(td), "testkey")
        assert load_private_seed(priv) == _SELFTEST_SEED; n += 1
        assert load_public_bytes(pub) == _SELFTEST_PUB; n += 1
        # 5) 裸种子 base64 文件
        raw = Path(td) / "seed.b64"
        raw.write_text(base64.b64encode(_SELFTEST_SEED).decode(), encoding="ascii")
        assert load_private_seed(raw) == _SELFTEST_SEED; n += 1

        # 6) generate → verify 全流程（含 --prev 合并）
        dmg = Path(td) / "mote-1.0.3-macos-arm64.dmg"
        zipf = Path(td) / "mote-1.0.3-win64-portable.zip"
        dmg.write_bytes(b"fake-dmg")
        zipf.write_bytes(b"fake-zip")
        key = Path(td) / "k.pem"
        key.write_text(priv.read_text(), encoding="ascii")
        out1 = Path(td) / "m1.json"
        main(["generate", "--product", "mote", "--channel", "stable",
              "--platform", "macos", "--version", "1.0.3", "--build", "202611180001",
              "--min-os", "14.0",
              "--artifact", f"dmg={dmg}", "--key", str(key), "--out", str(out1)])
        m1 = json.loads(out1.read_text(encoding="utf-8"))
        got_sha = m1["platforms"]["macos"]["artifacts"]["dmg"]["sha256"]
        assert got_sha == hashlib.sha256(b"fake-dmg").hexdigest(); n += 1
        main(["verify", "--manifest", str(out1), "--pub", str(pub)]); n += 1

        out2 = Path(td) / "m2.json"
        main(["generate", "--product", "mote", "--channel", "stable",
              "--platform", "windows", "--version", "1.0.3", "--build", "202611180001",
              "--artifact", f"portable={zipf}", "--key", str(key),
              "--prev", str(out1), "--out", str(out2)])
        m2 = json.loads(out2.read_text(encoding="utf-8"))
        assert set(m2["platforms"]) == {"macos", "windows"}; n += 1
        assert m2["platforms"]["macos"]["version"] == "1.0.3"; n += 1
        main(["verify", "--manifest", str(out2), "--pub", str(pub)]); n += 1

        # 6.1) resign：只改指定平台 rollout，产物与其他平台保持不变
        resigned = resign_manifest(m2, _SELFTEST_SEED, 5, "windows")
        assert resigned["platforms"]["windows"]["rollout_percent"] == 5; n += 1
        assert resigned["platforms"]["macos"]["rollout_percent"] == 100; n += 1
        assert (resigned["platforms"]["macos"]["artifacts"]
                == m2["platforms"]["macos"]["artifacts"]); n += 1
        all5 = resign_manifest(m2, _SELFTEST_SEED, 7, "all")
        assert (all5["platforms"]["macos"]["rollout_percent"] == 7
                and all5["platforms"]["windows"]["rollout_percent"] == 7); n += 1
        # 6.2) resign：补发说明页只动 notes_url 与 updated_at，产物不变
        noted = resign_manifest(m2, _SELFTEST_SEED, None, "all",
                                "https://example.com/n.html")
        assert noted["platforms"]["macos"]["notes_url"] == "https://example.com/n.html"; n += 1
        assert noted["platforms"]["windows"]["notes_url"] == "https://example.com/n.html"; n += 1
        assert (noted["platforms"]["windows"]["artifacts"]
                == m2["platforms"]["windows"]["artifacts"]); n += 1
        verify_manifest(noted, ed25519_pubkey(_SELFTEST_SEED)); n += 1

        # 7) 命名规则
        check_artifact_name(v2_artifact_name("mote", "1.0.3", "setup"),
                            "mote", "1.0.3", "setup"); n += 1
        try:
            check_artifact_name("Mote-Setup-1.0.3.exe", "mote", "1.0.3", "setup")
            raise AssertionError("旧命名应被拒绝")
        except ManifestError as exc:
            assert "v2 命名规则" in str(exc); n += 1

        # 8) 签名篡改在清单层被拦截
        tampered = json.loads(out1.read_text(encoding="utf-8"))
        tampered["platforms"]["macos"]["version"] = "9.9.9"
        try:
            verify_manifest(tampered, _SELFTEST_PUB if False else load_public_bytes(pub))
            raise AssertionError("篡改清单应验签失败")
        except ManifestError as exc:
            assert "签名验证失败" in str(exc); n += 1

    print(f"manifest_v2 selftest 通过（{n} 项）")
    return 0


# ---------------------------------------------------------------- CLI

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="更新清单 Schema v2 工具")
    sub = p.add_subparsers(dest="command", required=True)

    gen = sub.add_parser("generate", help="生成并签名 manifest.json")
    gen.add_argument("--product", required=True, choices=PRODUCTS)
    gen.add_argument("--channel", default="stable", choices=["stable", "beta"])
    gen.add_argument("--platform", required=True, choices=["macos", "windows"])
    gen.add_argument("--version", required=True)
    gen.add_argument("--build", required=True, type=int, help="构建号（纯数字、单调递增，macOS/Windows 都必填）")
    gen.add_argument("--min-os", default="")
    gen.add_argument("--artifact", action="append", default=[],
                     help="产物，格式 role=path，role ∈ dmg/setup/portable，可重复")
    gen.add_argument("--notes-url", default="")
    gen.add_argument("--rollout", default=100, type=int)
    gen.add_argument("--mandatory", action="store_true")
    gen.add_argument("--mandatory-reason", default="",
                     choices=list(MANDATORY_REASONS))
    gen.add_argument("--mandatory-deadline", default="")
    gen.add_argument("--base", default=DEFAULT_BASE)
    gen.add_argument("--key-id", default=KEY_ID_DEFAULT)
    gen.add_argument("--key", required=True, help="Ed25519 私钥（PEM 或 32 字节种子）")
    gen.add_argument("--prev", default="", help="已有 v2 清单；本次平台段合并进去后整体重签")
    gen.add_argument("--out", required=True)
    gen.set_defaults(func=command_generate)

    res = sub.add_parser("resign", help="改 rollout_percent / notes_url 后重新签名")
    res.add_argument("--manifest", required=True)
    res.add_argument("--key", required=True, help="Ed25519 私钥（PEM 或 32 字节种子）")
    res.add_argument("--rollout", type=int, default=None, help="1–100；不改则不传")
    res.add_argument("--notes-url", default=None,
                     help="发布说明页绝对地址；补发说明页时使用")
    res.add_argument("--platform", default="all", choices=["all", "macos", "windows"])
    res.add_argument("--out", default="", help="输出路径；默认覆盖输入清单")
    res.set_defaults(func=command_resign)

    ver = sub.add_parser("verify", help="校验清单签名与结构，可附带产物比对")
    ver.add_argument("--manifest", required=True)
    ver.add_argument("--pub", default="", help="Ed25519 公钥（PEM 或 raw）")
    ver.add_argument("--key", default="", help="没有公钥文件时用私钥推导")
    ver.add_argument("--artifact", action="append", default=[],
                     help="产物比对，格式 role=path，可重复")
    ver.set_defaults(func=command_verify)

    pk = sub.add_parser("pubkey", help="从私钥导出公钥 / PEM 对")
    pk.add_argument("--key", required=True)
    pk.add_argument("--export", default="", help="导出 PEM 到该目录")
    pk.set_defaults(func=command_pubkey)

    st = sub.add_parser("selftest", help="离线自检")
    st.set_defaults(func=cmd_selftest)
    return p


def main(argv: Optional[List[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except ManifestError as e:
        print(f"manifest_v2: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
