#!/usr/bin/env python3
"""三产品发布门禁（单一规则源）。

只依赖 Python 标准库：不读 OSS 凭据，所有线上检查都走公开 HTTP。
macOS / Windows / CI 用同一份代码，避免三套脚本各自漂移。

子命令：
  selftest       离线自检（CI 每次跑）
  doctor         检查发布凭据/密钥/工具是否就绪
  preflight      上传前：文件名、版本、目标键、覆盖风险、内嵌地址
  verify-live    上传后：清单字段、大小、SHA256、Content-Type
  version        打印 GUARD_VERSION

约定见 mote/docs/OSS-发布布局.md。
"""

from __future__ import annotations

import argparse
import base64
import glob
import hashlib
import json
import os
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

# Windows 控制台默认 cp1252/GBK，输出 ✓ 会直接抛 UnicodeEncodeError
for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        try:
            _stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass

GUARD_VERSION = "1.2.0"  # +doctor 子命令, +glob/shutil/os imports
DEFAULT_BASE = "https://dl.xiaotu.online"

PRODUCTS: Dict[str, Dict[str, Any]] = {
    "xiaotu": {
        "dmg": r"xiaotu-inventory-(?P<v>\d+\.\d+\.\d+)\.dmg",
        "setup": r"xiaotu-inventory-(?P<v>\d+\.\d+\.\d+)-win64-setup\.exe",
        "zip": r"xiaotu-inventory-(?P<v>\d+\.\d+\.\d+)-win64\.zip",
        "win_embed_exe": "xiaotu-shell.exe",
        "mac_extra": {"XiaotuUpdateManifestURL": "latest.json"},
    },
    "devcard": {
        "dmg": r"DevCard-(?P<v>\d+\.\d+\.\d+)-macos-(?:arm64|x64)\.dmg",
        "setup": r"DevCard-(?P<v>\d+\.\d+\.\d+)-setup\.exe",
        "zip": r"DevCard-(?P<v>\d+\.\d+\.\d+)-win64-portable\.zip",
        "win_embed_exe": "DevCard.exe",
        "mac_extra": {},
        "mac_sidecar": True,
    },
    "mote": {
        "dmg": r"Mote-macOS-native-(?P<v>\d+\.\d+\.\d+)\.dmg",
        "setup": r"Mote-Setup-(?P<v>\d+\.\d+\.\d+)\.exe",
        "zip": r"Mote-(?P<v>\d+\.\d+\.\d+)-win64-portable\.zip",
        "win_embed_exe": "Mote.exe",
        "mac_extra": {},
    },
}

HEADERS = {
    "dmg": ("application/octet-stream", "immutable"),
    "setup": ("application/octet-stream", "immutable"),
    "zip": ("application/zip", "immutable"),
    "latest.json": ("application/json", "max-age=60"),
    "windows.json": ("application/json", "max-age=60"),
    "manifest.json": ("application/json", "max-age=60"),
    "appcast.xml": (("application/xml", "application/rss+xml"), "max-age=60"),
    "edSignature": ("text/plain", "max-age=300"),
}


class GuardError(Exception):
    pass


def die(msg: str) -> None:
    raise GuardError(msg)


def ok(msg: str) -> None:
    print(f"  ✓ {msg}")


def warn(msg: str) -> None:
    print(f"  ! {msg}")


# ---------------------------------------------------------------- HTTP

def _request(url: str, method: str = "GET", timeout: int = 20):
    req = urllib.request.Request(url, method=method, headers={"User-Agent": f"release-guard/{GUARD_VERSION}"})
    try:
        return urllib.request.urlopen(req, timeout=timeout)
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return None
        raise GuardError(f"{method} {url} → HTTP {e.code}") from e
    except urllib.error.URLError as e:
        raise GuardError(f"{method} {url} 失败：{e.reason}") from e


def head(url: str) -> Optional[Dict[str, str]]:
    resp = _request(url, "HEAD")
    if resp is None:
        return None
    with resp:
        return {k.lower(): v for k, v in resp.headers.items()}


def get_json(url: str, bust: bool = False) -> Optional[dict]:
    if bust:
        url = f"{url}{'&' if '?' in url else '?'}guard={int(time.time() * 1000)}"
    resp = _request(url)
    if resp is None:
        return None
    with resp:
        raw = resp.read()
    try:
        return json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as e:
        raise GuardError(f"{url} 不是合法 JSON：{e}") from e


def get_text(url: str) -> Optional[str]:
    resp = _request(url)
    if resp is None:
        return None
    with resp:
        return resp.read().decode("utf-8", "replace")


# ---------------------------------------------------------------- Ed25519 验签（RFC 8032，纯标准库）

# v2 清单签名验证只依赖标准库。与 manifest_v2.py 的实现同源，
# 双方 selftest 都固化了同一组固定向量（与 cryptography 库交叉验证过），
# 任何一份漂移都会在 selftest 被发现。
_ED_P = 2 ** 255 - 19
_ED_L = 2 ** 252 + 27742317777372353535851937790883648493


def _ed_inv(x: int) -> int:
    return pow(x, _ED_P - 2, _ED_P)


_ED_D = -121665 * _ed_inv(121666) % _ED_P
_ED_I = pow(2, (_ED_P - 1) // 4, _ED_P)


def _ed_xrecover(y: int) -> int:
    xx = (y * y - 1) * _ed_inv(_ED_D * y * y + 1)
    x = pow(xx, (_ED_P + 3) // 8, _ED_P)
    if (x * x - xx) % _ED_P != 0:
        x = (x * _ED_I) % _ED_P
    if x % 2 != 0:
        x = _ED_P - x
    return x


_ED_By = 4 * _ed_inv(5) % _ED_P
_ED_Bx = _ed_xrecover(_ED_By)
_ED_B = (_ED_Bx % _ED_P, _ED_By % _ED_P, 1, (_ED_Bx * _ED_By) % _ED_P)
_ED_IDENT = (0, 1, 1, 0)


def _ed_add(p1, p2):
    x1, y1, z1, t1 = p1
    x2, y2, z2, t2 = p2
    a = (y1 - x1) * (y2 - x2) % _ED_P
    b = (y1 + x1) * (y2 + x2) % _ED_P
    c = 2 * t1 * t2 * _ED_D % _ED_P
    d = 2 * z1 * z2 % _ED_P
    e = b - a
    f = d - c
    g = d + c
    h = b + a
    return (e * f % _ED_P, g * h % _ED_P, f * g % _ED_P, e * h % _ED_P)


def _ed_mult(point, e: int):
    result = _ED_IDENT
    while e > 0:
        if e & 1:
            result = _ed_add(result, point)
        point = _ed_add(point, point)
        e >>= 1
    return result


def _ed_compress(point) -> bytes:
    x, y, z, _ = point
    zi = _ed_inv(z)
    x = x * zi % _ED_P
    y = y * zi % _ED_P
    return int.to_bytes(y | ((x & 1) << 255), 32, "little")


def _ed_decompress(data: bytes):
    if len(data) != 32:
        return None
    y = int.from_bytes(data, "little")
    sign = y >> 255
    y &= (1 << 255) - 1
    if y >= _ED_P:
        return None
    xx = (y * y - 1) * _ed_inv(_ED_D * y * y + 1) % _ED_P
    x = pow(xx, (_ED_P + 3) // 8, _ED_P)
    if (x * x - xx) % _ED_P != 0:
        x = (x * _ED_I) % _ED_P
    if (x * x - xx) % _ED_P != 0:
        return None
    if (x & 1) != sign:
        x = _ED_P - x
    return (x, y, 1, x * y % _ED_P)


def ed25519_verify(pub: bytes, msg: bytes, sig: bytes) -> bool:
    if len(pub) != 32 or len(sig) != 64:
        return False
    a_point = _ed_decompress(pub)
    if a_point is None:
        return False
    r_point = _ed_decompress(sig[:32])
    if r_point is None:
        return False
    s = int.from_bytes(sig[32:], "little")
    if s >= _ED_L:
        return False
    h = int.from_bytes(
        hashlib.sha512(sig[:32] + pub + msg).digest(), "little"
    ) % _ED_L
    left = _ed_mult(_ED_B, s)
    right = _ed_add(r_point, _ed_mult(a_point, h))
    return _ed_compress(left) == _ed_compress(right)


# ---------------------------------------------------------------- 基础规则

def product_spec(product: str) -> Dict[str, Any]:
    if product not in PRODUCTS:
        die(f"product 必须是 {sorted(PRODUCTS)}，收到 {product!r}")
    return PRODUCTS[product]


def check_channel(channel: str) -> str:
    if channel not in ("stable", "beta"):
        die(f"channel 必须是 stable/beta，收到 {channel!r}")
    return channel


def parse_version(filename: str, kind: str, product: str) -> str:
    spec = product_spec(product)
    pattern = spec.get(kind)
    if not pattern:
        die(f"{product} 不支持类型 {kind}")
    m = re.fullmatch(pattern, filename)
    if not m:
        die(f"{filename} 不符合 {product}/{kind} 命名规则：{pattern}")
    return m.group("v")


def vtuple(v: str) -> Tuple[int, ...]:
    parts = [int(x) for x in re.findall(r"\d+", v)]
    while len(parts) < 3:
        parts.append(0)
    return tuple(parts[:3])


def version_cmp(a: str, b: str) -> int:
    ta, tb = vtuple(a), vtuple(b)
    return (ta > tb) - (ta < tb)


def artifact_key(product: str, filename: str) -> str:
    return f"{product}/{filename}"


def manifest_key(product: str, channel: str, kind: str) -> str:
    name = "latest.json" if kind == "mac" else "windows.json"
    return f"{product}/updates/{channel}/{name}"


def appcast_key(product: str, channel: str) -> str:
    return f"{product}/updates/{channel}/appcast.xml"


# ------------------------------------------------------ Schema v2（更新清单Schema-v2.md）

# v2 安装包命名：<product>-<version>-<os>-<arch>-<type>.<ext>
V2_ROLE_EXT = {
    "dmg": "macos-arm64.dmg",
    "setup": "win64-setup.exe",
    "portable": "win64-portable.zip",
}
# guard 内部 role → v2 artifacts key
V2_ROLE_MAP = {"dmg": "dmg", "setup": "setup", "zip": "portable"}
V2_PLATFORM = {"mac": "macos", "win": "windows"}
V2_ISO_RE = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$")


def manifest_v2_key(product: str, channel: str) -> str:
    return f"{product}/updates/{channel}/manifest.json"


def parse_version_v2(filename: str, role: str, product: str) -> str:
    ext = V2_ROLE_EXT[V2_ROLE_MAP.get(role, role)]
    m = re.fullmatch(rf"{re.escape(product)}-(\d+\.\d+\.\d+)-{re.escape(ext)}", filename)
    if not m:
        die(f"{filename} 不符合 v2 命名规则：{product}-<版本>-{ext}")
    return m.group(1)


def v2_canonical_bytes(body: Dict[str, Any]) -> bytes:
    payload = {k: v for k, v in body.items() if k != "signature"}
    return json.dumps(payload, ensure_ascii=False, sort_keys=True,
                      separators=(",", ":")).encode("utf-8")


def live_version_v2(product: str, channel: str, base: str, kind: str) -> Optional[Tuple[str, int]]:
    """线上 manifest.json 中本平台当前的 (version, build)；清单不存在返回 None。"""
    data = get_json(f"{base}/{manifest_v2_key(product, channel)}", bust=True)
    if not data:
        return None
    section = (data.get("platforms") or {}).get(V2_PLATFORM[kind])
    if not section:
        return None
    return (str(section.get("version", "")), int(section.get("build", 0)))


def check_manifest_v2(data: Dict[str, Any], product: str, kind: str,
                      artifacts: List[Artifact], version: str, base: str,
                      channel: str, build: Optional[int] = None,
                      pub: Optional[bytes] = None) -> None:
    if data.get("manifest_version") != 2:
        die(f"manifest_version={data.get('manifest_version')}，应为 2")
    if data.get("product") != product:
        die(f"清单 product={data.get('product')}，应为 {product}")
    if data.get("channel") != channel:
        die(f"清单 channel={data.get('channel')}，应为 {channel}")
    if not data.get("key_id"):
        die("清单缺少 key_id")
    if not V2_ISO_RE.fullmatch(str(data.get("updated_at", ""))):
        die(f"updated_at 不是 ISO 8601 UTC 格式：{data.get('updated_at')!r}")
    if pub is not None:
        sig = str(data.get("signature", ""))
        if not sig:
            die("清单缺少 signature")
        try:
            raw_sig = base64.b64decode(sig)
        except Exception as e:
            die(f"signature 不是合法 base64：{e}")
        if not ed25519_verify(pub, v2_canonical_bytes(data), raw_sig):
            die("清单签名验证失败（公钥不匹配或清单被改动）")
        ok("清单签名验证通过")
    platform = V2_PLATFORM[kind]
    platforms = data.get("platforms")
    if not isinstance(platforms, dict) or platform not in platforms:
        die(f"清单 platforms 缺少 {platform}")
    section = platforms[platform]
    if section.get("version") != version:
        die(f"清单 {platform}.version={section.get('version')} 与产物 {version} 不一致")
    if build is not None and int(section.get("build", 0)) != build:
        die(f"清单 {platform}.build={section.get('build')} 与产物构建号 {build} 不一致")
    arts = section.get("artifacts")
    if not isinstance(arts, dict) or not arts:
        die(f"清单 {platform}.artifacts 为空")
    for art in artifacts:
        role2 = V2_ROLE_MAP[art.role]
        entry = arts.get(role2)
        if not isinstance(entry, dict):
            die(f"清单 {platform}.artifacts 缺少 {role2}")
        if entry.get("size") != art.size:
            die(f"清单 {role2}.size={entry.get('size')}，实际 {art.size}")
        if entry.get("sha256") != art.sha256:
            die(f"清单 {role2}.sha256 与本地 SHA256 不一致")
        url = str(entry.get("url", ""))
        check_url(url, product, base)
        remote = f"{product}-{version}-{V2_ROLE_EXT[role2]}"
        if not url.endswith(f"/{product}/{remote}"):
            die(f"清单 {role2}.url 与 v2 命名不一致：{url}\n    应为 …/{product}/{remote}")
    notes = section.get("notes_url")
    if notes and not str(notes).startswith("https://"):
        die(f"notes_url 必须是 https：{notes}")
    ok(f"manifest.json（{platform}）字段与产物一致")


def load_pub_bytes_arg(pub_hex: str, pub_path: Optional[str]) -> Optional[bytes]:
    if pub_hex:
        try:
            data = bytes.fromhex(pub_hex)
        except Exception as e:
            die(f"--pub-hex 不是合法十六进制：{e}")
        if len(data) != 32:
            die(f"--pub-hex 必须是 32 字节公钥，收到 {len(data)} 字节")
        return data
    if pub_path:
        p = Path(pub_path)
        if not p.is_file():
            die(f"公钥文件不存在：{p}")
        raw = p.read_bytes()
        text = raw.decode("ascii", "ignore").strip()
        if "-----BEGIN" in text:
            b64 = "".join(line for line in text.splitlines() if not line.startswith("-----"))
            try:
                der = base64.b64decode("".join(b64.split()))
            except Exception as e:
                die(f"公钥 PEM 解码失败：{e}")
            spki = bytes.fromhex("302a300506032b6570032100")
            if der.startswith(spki) and len(der) == len(spki) + 32:
                return der[-32:]
            die("公钥不是 Ed25519 SPKI PEM")
        if len(raw) == 32:
            return raw
        try:
            data = bytes.fromhex(text)
            if len(data) == 32:
                return data
        except Exception:
            pass
        try:
            data = base64.b64decode(text, validate=True)
            if len(data) == 32:
                return data
        except Exception:
            pass
        die(f"公钥格式无法识别（PEM / hex / base64 / 32 字节二进制）：{p}")
    return None


def file_digests(path: Path) -> Tuple[int, str, str]:
    sha, md5 = hashlib.sha256(), hashlib.md5()
    size = 0
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            size += len(chunk)
            sha.update(chunk)
            md5.update(chunk)
    return size, sha.hexdigest(), md5.hexdigest()


def check_url(url: str, product: str, base: str) -> None:
    prefix = f"{base.rstrip('/')}/{product}/"
    if not url.startswith(prefix):
        die(f"URL 不在 {prefix} 下：{url}")
    if ".." in url or not url.startswith("https://"):
        die(f"URL 必须是 https 且不含 ..：{url}")


def live_version(product: str, channel: str, kind: str, base: str) -> Optional[str]:
    data = get_json(f"{base}/{manifest_key(product, channel, kind)}", bust=True)
    if not data:
        return None
    return data.get("version") or data.get("latest")


def existing_artifact_ok(url: str, size: int, md5: str) -> None:
    """同名键已存在时，只允许内容完全相同（幂等重传）。"""
    h = head(url)
    if h is None:
        return
    remote_size = int(h.get("content-length", -1))
    if remote_size != size:
        die(f"目标已存在且大小不同（远端 {remote_size}，本地 {size}）：{url}\n"
            "    安装包是 immutable，请升版本号，不要覆盖同名文件")
    etag = h.get("etag", "").strip('"')
    if etag and "-" not in etag and etag.lower() != md5:
        die(f"目标已存在且内容不同（ETag {etag}）：{url}\n"
            "    安装包是 immutable，请升版本号，不要覆盖同名文件")


def check_mac_embed(path: Path, product: str, channel: str, base: str, version: str,
                    schema: str = "v1") -> None:
    if sys.platform != "darwin":
        warn("非 macOS 环境，跳过 DMG 内嵌地址检查")
        return
    with tempfile.TemporaryDirectory() as mnt:
        subprocess.run(
            ["hdiutil", "attach", "-nobrowse", "-readonly", "-mountpoint", mnt, str(path)],
            check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        try:
            plists = list(Path(mnt).glob("*.app/Contents/Info.plist"))
            if not plists:
                die(f"DMG 内没有 .app/Contents/Info.plist：{path}")
            with plists[0].open("rb") as f:
                info = plistlib.load(f)
        finally:
            subprocess.run(["hdiutil", "detach", mnt, "-quiet"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    got = str(info.get("CFBundleShortVersionString", ""))
    if got != version:
        die(f"DMG 内版本 {got} 与文件名版本 {version} 不一致")
    feed = str(info.get("SUFeedURL", ""))
    want_feed = f"{base}/{appcast_key(product, channel)}"
    if feed != want_feed:
        die(f"DMG 内 SUFeedURL 与发布地址不一致：\n    包内 {feed or '(空)'}\n    应为 {want_feed}")
    if not info.get("SUPublicEDKey"):
        die("DMG 内 App 缺少 SUPublicEDKey")
    spec = product_spec(product)
    if "XiaotuUpdateManifestURL" in spec.get("mac_extra", {}):
        want = f"{base}/{manifest_v2_key(product, channel) if schema == 'v2' else manifest_key(product, channel, 'mac')}"
        got_url = str(info.get("XiaotuUpdateManifestURL", ""))
        if got_url != want:
            die(f"DMG 内 XiaotuUpdateManifestURL 与发布地址不一致：{got_url or '(空)'} != {want}")
    ok(f"DMG 内嵌地址正确（{feed}）")


def check_win_embed(zip_path: Path, product: str, channel: str, base: str,
                    schema: str = "v1") -> None:
    spec = product_spec(product)
    exe_name = spec["win_embed_exe"].lower()
    url = f"{base}/{manifest_v2_key(product, channel) if schema == 'v2' else manifest_key(product, channel, 'win')}"
    # Qt/原生 Windows 程序常用宽字符（UTF-16LE）存 URL，其他用 UTF-8/ASCII。
    needles = (url.encode(), url.encode("utf-16-le"))
    with zipfile.ZipFile(zip_path) as z:
        names = [n for n in z.namelist()
                 if n.replace("\\", "/").lower().endswith("/" + exe_name) or
                 n.lower().endswith(exe_name)]
        if not names:
            die(f"绿色包内没有 {spec['win_embed_exe']}")
        if not any(any(x in z.read(n) for x in needles) for n in names):
            die(f"绿色包内 {spec['win_embed_exe']} 未包含清单地址：\n    {url}\n"
                "    这是旧构建产物，请用当前源码重建后再打包")
    ok(f"绿色包内嵌地址正确（{url}）")


# ---------------------------------------------------------------- Windows build 二次校验（防清单/编译值漂移）

# 产品→二进制内可搜索的构建号字符串前缀映射。
# Mote CMakeLists.txt 同时以数字和字符串形式注入 MOTE_BUILD / MOTE_BUILD_STR，
# platform/win/updater.cpp 里放了一条静态字符串 "MOTE_BUILD=<digits>"；
# 小兔同理 XIAOTU_BUILD。DevCard 无本机构建号，跳过。
_BUILD_STAMP_PREFIX: Dict[str, Optional[bytes]] = {
    "mote": b"MOTE_BUILD=",
    "xiaotu": b"XIAOTU_BUILD=",
    "devcard": None,
}


def _extract_build_from_bytes(data: bytes, product: str) -> Optional[str]:
    """在二进制数据里搜 "<PREFIX><digits>"，返回数字部分。"""
    prefix = _BUILD_STAMP_PREFIX.get(product)
    if not prefix:
        return None
    # UTF-8：直接搜 b'PREFIX\\d+'
    match = re.search(re.escape(prefix) + rb'(\d{4,})', data)
    if match:
        return match.group(1).decode("ascii")
    # UTF-16LE：每个 ASCII 字符后跟 \\x00
    utf16_prefix = prefix.decode("ascii").encode("utf-16-le")
    match = re.search(re.escape(utf16_prefix) + rb'(\d\x00){4,}', data)
    if match:
        raw = match.group(0)[len(utf16_prefix):]
        return bytes(b for i, b in enumerate(raw) if i % 2 == 0 and b != 0).decode("ascii")
    return None


def _read_exe_bytes(artifact_path: Path, win_exe_name: str) -> bytes:
    """Windows 产物可能是绿色包 zip（Mote.exe 在里面）或 setup.exe 本身。"""
    if artifact_path.suffix.lower() == ".zip":
        with zipfile.ZipFile(str(artifact_path)) as z:
            names = [n for n in z.namelist()
                     if n.replace("\\", "/").lower().endswith("/" + win_exe_name) or
                     n.lower().endswith(win_exe_name)]
            if not names:
                raise GuardError(f"zip 内没有 {win_exe_name}")
            return z.read(names[0])
    return artifact_path.read_bytes()


def check_win_build_stamp(artifact_path: Path, expected_build: Optional[int],
                          product: str) -> None:
    """从绿色包 zip 或 setup.exe 里读出编译时的构建号，与参数 --build 比对。

    防止 publish_windows.ps1 的 MOTE_BUILD_STAMP / CMakeCache 读取出错导致
    清单 build 与编译值不一致——客户端同版本反复提示同一版本
    （2026-11-19 mote 1.0.4 即因此重发）。
    """
    if expected_build is None:
        warn("未提供 --build，跳过 EXE 内构建号校验")
        return
    prefix = _BUILD_STAMP_PREFIX.get(product)
    if not prefix:
        return  # devcard 等无本机构建号，跳过

    spec = product_spec(product)
    win_exe_name = spec["win_embed_exe"].lower()
    try:
        exe_data = _read_exe_bytes(artifact_path, win_exe_name)
    except (zipfile.BadZipFile, GuardError) as exc:
        warn(f"无法从 {artifact_path.name} 提取 {win_exe_name}：{exc}，跳过 build 校验")
        return

    found = _extract_build_from_bytes(exe_data, product)
    if found is None:
        warn(f"二进制内未找到 {prefix.decode()}<digits> 标记，跳过 build 二次校验"
             f"（可能是旧构建产物或未启用 MOTE_BUILD_STR）")
        return

    found_int = int(found)
    if found_int != expected_build:
        die(f"EXE 内构建号 {found_int} 与 --build {expected_build} 不一致！\n"
            f"    清单 build 大于编译值会让客户端反复提示同一版本"
            f"（2026-11-19 mote 1.0.4 即因此重发）")
    ok(f"EXE 内构建号与参数一致：{found_int}")


# ---------------------------------------------------------------- 清单校验

@dataclass
class Artifact:
    role: str  # dmg / setup / zip
    path: Path
    name: str
    size: int
    sha256: str


def check_devcard_manifest(data: dict, artifacts: List[Artifact], version: str,
                           base: str, channel: str) -> None:
    if data.get("latest") != version:
        die(f"清单 latest={data.get('latest')} 与产物版本 {version} 不一致")
    assets = {a.get("name"): a for a in (data.get("assets") or [])}
    for art in artifacts:
        a = assets.get(art.name)
        if not a:
            die(f"清单 assets 缺少 {art.name}")
        if a.get("size") != art.size:
            die(f"清单 {art.name} size={a.get('size')}，实际 {art.size}")
        digest = str(a.get("digest", ""))
        if digest != f"sha256:{art.sha256}":
            die(f"清单 {art.name} digest 与本地 SHA256 不一致")
        check_url(str(a.get("download_url", "")), "devcard", base)
        if art.role == "dmg" and not a.get("signature"):
            die(f"清单 {art.name} 缺少 signature")
    endpoint = str(data.get("check_endpoint", ""))
    if endpoint:
        check_url(endpoint, "devcard", base)
    ok("devcard 清单字段与产物一致")


def check_xiaotu_manifest(data: dict, artifacts: List[Artifact], version: str,
                          base: str, channel: str, kind: str) -> None:
    expect_platform = "macos" if kind == "mac" else "windows"
    if data.get("platform") != expect_platform:
        die(f"清单 platform={data.get('platform')}，应为 {expect_platform}")
    if data.get("channel") != channel:
        die(f"清单 channel={data.get('channel')}，应为 {channel}")
    if data.get("version") != version:
        die(f"清单 version={data.get('version')} 与产物 {version} 不一致")
    if not data.get("signature"):
        die("清单缺少 signature")
    main = next((a for a in artifacts if a.role != "zip"), None)
    portable = next((a for a in artifacts if a.role == "zip"), None)
    if main:
        if data.get("sha256") != main.sha256:
            die("清单 sha256 与本地安装包不一致")
        if data.get("size") != main.size:
            die(f"清单 size={data.get('size')}，实际 {main.size}")
        check_url(str(data.get("url", "")), "xiaotu", base)
    if portable:
        if data.get("portable_sha256") != portable.sha256:
            die("清单 portable_sha256 与本地绿色包不一致")
        if data.get("portable_size") != portable.size:
            die("清单 portable_size 与本地绿色包不一致")
        check_url(str(data.get("portable_url", "")), "xiaotu", base)
    ok("xiaotu 清单字段与产物一致")


def check_mote_manifest(data: dict, artifacts: List[Artifact], version: str,
                        base: str, channel: str, kind: str) -> None:
    if data.get("version") != version:
        die(f"清单 version={data.get('version')} 与产物 {version} 不一致")
    if kind == "mac":
        art = next((a for a in artifacts if a.role == "dmg"), None)
        if art:
            if data.get("sha256") != art.sha256 or data.get("size") != art.size:
                die("清单 sha256/size 与 dmg 不一致")
            check_url(str(data.get("url", "")), "mote", base)
    else:
        portable = next((a for a in artifacts if a.role == "zip"), None)
        setup = next((a for a in artifacts if a.role == "setup"), None)
        if portable:
            if data.get("sha256") != portable.sha256 or data.get("size") != portable.size:
                die("清单 sha256/size 与绿色包不一致")
            check_url(str(data.get("url", "")), "mote", base)
        if setup and data.get("setup_url"):
            if data.get("setup_sha256") != setup.sha256 or data.get("setup_size") != setup.size:
                die("清单 setup_sha256/setup_size 与安装包不一致")
            check_url(str(data.get("setup_url", "")), "mote", base)
    ok("mote 清单字段与产物一致")


def check_manifest(data: dict, product: str, kind: str, artifacts: List[Artifact],
                   version: str, base: str, channel: str) -> None:
    if product == "devcard":
        check_devcard_manifest(data, artifacts, version, base, channel)
    elif product == "xiaotu":
        check_xiaotu_manifest(data, artifacts, version, base, channel, kind)
    else:
        check_mote_manifest(data, artifacts, version, base, channel, kind)


# ---------------------------------------------------------------- 子命令

def collect_artifacts(product: str, kind: str, file: Path, zip_file: Optional[Path],
                      name: Optional[str] = None, zip_name: Optional[str] = None,
                      schema: str = "v1") -> List[Artifact]:
    if not file.is_file():
        die(f"文件不存在：{file}")
    if kind == "mac":
        role = "dmg"
    elif file.suffix.lower() == ".zip":
        role = "zip"
    else:
        role = "setup"
    pairs = [(file, role, name)]
    if zip_file:
        pairs.append((zip_file, "zip", zip_name))
    artifacts: List[Artifact] = []
    versions = set()
    for path, r, override in pairs:
        if not path.is_file():
            die(f"文件不存在：{path}")
        display = override or path.name
        parse = parse_version_v2 if schema == "v2" else parse_version
        versions.add(parse(display, r, product))
        size, sha, _ = file_digests(path)
        artifacts.append(Artifact(r, path, display, size, sha))
    if len(versions) != 1:
        die(f"产物版本不一致：{sorted(versions)}")
    return artifacts


def cmd_preflight(args: argparse.Namespace) -> int:
    check_channel(args.channel)
    base = args.base.rstrip("/")
    schema = "v2"  # v1 清单已于 2026-11-18 全量删除，强制 v2
    artifacts = collect_artifacts(args.product, args.kind, Path(args.file),
                                  Path(args.zip) if args.zip else None,
                                  args.name, args.zip_name, schema)
    parse = parse_version_v2 if schema == "v2" else parse_version
    version = parse(artifacts[0].name, artifacts[0].role, args.product)
    if args.version and args.version != version:
        die(f"--version {args.version} 与文件名版本 {version} 不一致")
    print(f"guard preflight · {args.product} / {args.kind} / {version}"
          + (f" · schema={schema}" if schema == "v2" else ""))
    if schema == "v2":
        pub = load_pub_bytes_arg(args.pub_hex, args.pub)
        if args.manifest:
            mpath = Path(args.manifest)
            if not mpath.is_file():
                die(f"清单不存在：{mpath}")
            data = json.loads(mpath.read_text(encoding="utf-8"))
            check_manifest_v2(data, args.product, args.kind, artifacts, version,
                              base, args.channel, build=args.build, pub=pub)
        elif args.kind == "win" and args.build is None:
            warn("v2 未提供 --build，跳过构建号相关检查")
        current = None if args.offline else live_version_v2(args.product, args.channel, base, args.kind)
        if current:
            c = version_cmp(version, current[0])
            if c < 0:
                die(f"版本回退：本地 {version} < 线上 {current[0]}")
            if c == 0 and args.kind == "win" and args.build is not None:
                if args.build < current[1]:
                    die(f"构建号回退：本地 {args.build} < 线上 {current[1]}")
            if c == 0:
                warn(f"版本与线上相同（{version}），将按幂等重传校验")
    else:
        current = None if args.offline else live_version(args.product, args.channel, args.kind, base)
        if current:
            c = version_cmp(version, current)
            if c < 0:
                die(f"版本回退：本地 {version} < 线上 {current}")
            if c == 0:
                warn(f"版本与线上相同（{version}），将按幂等重传校验")
    for art in artifacts:
        key = artifact_key(args.product, art.name)
        want_ct, want_cc = HEADERS[art.role]
        print(f"  {art.role}: {key}  {art.size} B  sha256={art.sha256[:16]}…  [{want_ct}, {want_cc}]")
        if not args.offline:
            existing_artifact_ok(f"{base}/{key}", art.size, file_digests(art.path)[2])
    if args.kind == "mac":
        check_mac_embed(Path(args.file), args.product, args.channel, base, version, schema)
    else:
        zip_path = Path(args.zip) if args.zip else (Path(args.file) if args.file.lower().endswith(".zip") else None)
        if zip_path:
            check_win_embed(zip_path, args.product, args.channel, base, schema)
        else:
            warn("未提供绿色包，跳过壳内嵌地址检查")
        # 对每个 Windows 产物做 EXE 内 build stamp 二次校验：
        # 绿色包 / setup.exe 里的 Mote.exe 都应该含 "MOTE_BUILD=<digits>" 字符串
        if args.build is not None:
            for art in artifacts:
                if art.role in ("zip", "setup"):
                    check_win_build_stamp(art.path, args.build, args.product)
    print("preflight 通过")
    return 0


def cmd_verify_live(args: argparse.Namespace) -> int:
    check_channel(args.channel)
    base = args.base.rstrip("/")
    schema = "v2"  # v1 清单已于 2026-11-18 全量删除，强制 v2
    artifacts = collect_artifacts(args.product, args.kind, Path(args.file),
                                  Path(args.zip) if args.zip else None,
                                  args.name, args.zip_name, schema)
    parse = parse_version_v2 if schema == "v2" else parse_version
    version = parse(artifacts[0].name, artifacts[0].role, args.product)
    print(f"guard verify-live · {args.product} / {args.kind} / {version}"
          + (f" · schema={schema}" if schema == "v2" else ""))
    mkey = manifest_v2_key(args.product, args.channel) if schema == "v2" \
        else manifest_key(args.product, args.channel, args.kind)
    data = get_json(f"{base}/{mkey}", bust=True)
    if data is None:
        die(f"清单不存在：{base}/{mkey}")
    if schema == "v2":
        pub = load_pub_bytes_arg(args.pub_hex, args.pub)
        check_manifest_v2(data, args.product, args.kind, artifacts, version,
                          base, args.channel, build=args.build, pub=pub)
    else:
        check_manifest(data, args.product, args.kind, artifacts, version, base, args.channel)

    for art in artifacts:
        url = f"{base}/{artifact_key(args.product, art.name)}"
        h = head(url)
        if h is None:
            die(f"安装包不存在：{url}")
        if int(h.get("content-length", -1)) != art.size:
            die(f"{url} 大小不符：{h.get('content-length')} != {art.size}")
        want_ct, want_cc = HEADERS[art.role]
        cts = (want_ct,) if isinstance(want_ct, str) else want_ct
        got_ct = h.get("content-type", "")
        if not any(got_ct.startswith(t) for t in cts):
            die(f"{url} Content-Type={got_ct!r}，应为 {want_ct}")
        if want_cc not in h.get("cache-control", ""):
            warn(f"{url} Cache-Control={h.get('cache-control')!r}，期望含 {want_cc!r}")
        ok(f"{url}  {art.size} B  {got_ct}")

    appcast_url = f"{base}/{appcast_key(args.product, args.channel)}"
    if args.kind == "mac":
        body = get_text(appcast_url)
        if body is None:
            die(f"appcast 不存在：{appcast_url}")
        if version not in body:
            die(f"appcast 中未出现版本 {version}")
        ok(f"appcast 已包含 {version}")

    # 带 Sparkle EdDSA 旁车签名的产品由 spec 声明，避免再写死产品名。
    if product_spec(args.product).get("mac_sidecar") and args.kind == "mac":
        sig_url = f"{base}/{artifact_key(args.product, artifacts[0].name)}.edSignature"
        h = head(sig_url)
        if h is None:
            die(f"缺少 edSignature：{sig_url}")
        ok(f"edSignature 存在（{h.get('content-length', '?')} B）")

    mh = head(f"{base}/{mkey}")
    if mh:
        if not mh.get("content-type", "").startswith("application/json"):
            warn(f"清单 Content-Type={mh.get('content-type')!r}")
        if "max-age=60" not in mh.get("cache-control", ""):
            warn(f"清单 Cache-Control={mh.get('cache-control')!r}，期望 60 秒")
    print("verify-live 通过")
    return 0


# ---------------------------------------------------------------- 自检

def _expect_error(fn, needle: str) -> None:
    try:
        fn()
    except GuardError as e:
        if needle not in str(e):
            raise AssertionError(f"错误信息不含 {needle!r}：{e}") from e
        return
    raise AssertionError(f"应当失败但通过了：{needle}")


def cmd_selftest() -> int:
    n = 0
    assert parse_version("xiaotu-inventory-1.0.0.dmg", "dmg", "xiaotu") == "1.0.0"; n += 1
    assert parse_version("DevCard-1.2.4-macos-arm64.dmg", "dmg", "devcard") == "1.2.4"; n += 1
    assert parse_version("Mote-Setup-1.0.1.exe", "setup", "mote") == "1.0.1"; n += 1
    assert parse_version("Mote-1.0.1-win64-portable.zip", "zip", "mote") == "1.0.1"; n += 1
    _expect_error(lambda: parse_version("Mote-Windows.zip", "zip", "mote"), "命名规则"); n += 1
    _expect_error(lambda: parse_version("xiaotu-inventory-1.0.0-win64.zip", "zip", "devcard"), "命名规则"); n += 1

    assert version_cmp("1.2.10", "1.2.9") > 0; n += 1
    assert version_cmp("1.0", "1.0.0") == 0; n += 1
    assert version_cmp("0.9.9", "1.0.0") < 0; n += 1

    assert manifest_key("xiaotu", "stable", "win") == "xiaotu/updates/stable/windows.json"; n += 1
    assert appcast_key("mote", "beta") == "mote/updates/beta/appcast.xml"; n += 1
    check_url("https://dl.xiaotu.online/mote/updates/stable/latest.json", "mote", DEFAULT_BASE); n += 1
    _expect_error(lambda: check_url("https://dl.xiaotu.online/xiaotu/x.dmg", "mote", DEFAULT_BASE), "不在"); n += 1
    _expect_error(lambda: check_url("http://dl.xiaotu.online/mote/x.dmg", "mote", DEFAULT_BASE), "https"); n += 1

    arts = [Artifact("dmg", Path("x"), "DevCard-1.2.4-macos-arm64.dmg", 10, "a" * 64)]
    good = {"latest": "1.2.4", "check_endpoint": "https://dl.xiaotu.online/devcard/updates/stable/latest.json",
            "assets": [{"name": arts[0].name, "platform": "macos", "size": 10,
                        "digest": "sha256:" + "a" * 64, "signature": "s",
                        "download_url": "https://dl.xiaotu.online/devcard/" + arts[0].name}]}
    check_manifest(good, "devcard", "mac", arts, "1.2.4", DEFAULT_BASE, "stable"); n += 1
    bad = json.loads(json.dumps(good)); bad["assets"][0]["digest"] = "sha256:" + "b" * 64
    _expect_error(lambda: check_manifest(bad, "devcard", "mac", arts, "1.2.4", DEFAULT_BASE, "stable"), "digest"); n += 1

    xarts = [Artifact("dmg", Path("x"), "xiaotu-inventory-1.0.0.dmg", 5, "c" * 64)]
    xm = {"version": "1.0.0", "platform": "macos", "channel": "stable", "size": 5, "sha256": "c" * 64,
          "signature": "s", "url": "https://dl.xiaotu.online/xiaotu/xiaotu-inventory-1.0.0.dmg"}
    check_manifest(xm, "xiaotu", "mac", xarts, "1.0.0", DEFAULT_BASE, "stable"); n += 1
    _expect_error(lambda: check_manifest({**xm, "platform": "windows"}, "xiaotu", "mac", xarts,
                                         "1.0.0", DEFAULT_BASE, "stable"), "platform"); n += 1

    marts = [Artifact("zip", Path("x"), "Mote-1.0.1-win64-portable.zip", 7, "d" * 64)]
    mm = {"version": "1.0.1", "size": 7, "sha256": "d" * 64,
          "url": "https://dl.xiaotu.online/mote/Mote-1.0.1-win64-portable.zip"}
    check_manifest(mm, "mote", "win", marts, "1.0.1", DEFAULT_BASE, "stable"); n += 1
    _expect_error(lambda: check_manifest({**mm, "sha256": "e" * 64}, "mote", "win", marts,
                                         "1.0.1", DEFAULT_BASE, "stable"), "不一致"); n += 1

    # 绿色包内嵌地址：真实 zip 结构，含目录层级
    with tempfile.TemporaryDirectory() as td:
        zp = Path(td) / "Mote-1.0.1-win64-portable.zip"
        with zipfile.ZipFile(zp, "w") as z:
            z.writestr("Mote-Windows/Mote.exe",
                       "junk https://dl.xiaotu.online/mote/updates/stable/windows.json junk".encode("utf-16-le"))
        check_win_embed(zp, "mote", "stable", DEFAULT_BASE); n += 1
        zp2 = Path(td) / "Mote-1.0.2-win64-portable.zip"
        with zipfile.ZipFile(zp2, "w") as z:
            z.writestr("Mote-Windows/Mote.exe", b"ascii https://dl.xiaotu.online/mote/updates/stable/windows.json")
        check_win_embed(zp2, "mote", "stable", DEFAULT_BASE); n += 1
        bad_zip = Path(td) / "bad.zip"
        with zipfile.ZipFile(bad_zip, "w") as z:
            z.writestr("Mote-Windows/Mote.exe", b"https://dl.xiaotu.online/windows/stable/latest.json")
        _expect_error(lambda: check_win_embed(bad_zip, "mote", "stable", DEFAULT_BASE), "旧构建产物"); n += 1

    # ---- Schema v2 ----
    # Ed25519 固定向量（与 manifest_v2.py、cryptography 交叉验证后固化）
    v2pub = bytes.fromhex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a")
    v2msg = b"manifest-v2-selftest"
    v2sig = bytes.fromhex(
        "e642309f0f791045d3d8e112e507c33f89b5d7b0cb79b4ca58412cc4f58f2c76"
        "2e2f1cabf5a2fac05e633b5203880ba8a9de7601474722d7e15f31463660860d")
    assert ed25519_verify(v2pub, v2msg, v2sig); n += 1
    assert not ed25519_verify(v2pub, v2msg + b"x", v2sig); n += 1
    bad = bytearray(v2sig); bad[10] ^= 1
    assert not ed25519_verify(v2pub, v2msg, bytes(bad)); n += 1
    assert v2_canonical_bytes({"b": 1, "a": "x", "signature": "sig"}) == b'{"a":"x","b":1}'; n += 1

    assert manifest_v2_key("devcard", "beta") == "devcard/updates/beta/manifest.json"; n += 1
    assert parse_version_v2("mote-1.0.3-macos-arm64.dmg", "dmg", "mote") == "1.0.3"; n += 1
    assert parse_version_v2("xiaotu-1.0.1-win64-setup.exe", "setup", "xiaotu") == "1.0.1"; n += 1
    assert parse_version_v2("mote-1.0.3-win64-portable.zip", "zip", "mote") == "1.0.3"; n += 1
    _expect_error(lambda: parse_version_v2("Mote-Setup-1.0.3.exe", "setup", "mote"), "v2 命名规则"); n += 1
    _expect_error(lambda: parse_version_v2("devcard-1.2.5-macos-arm64.dmg", "dmg", "xiaotu"), "v2 命名规则"); n += 1

    v2arts = [Artifact("zip", Path("x"), "mote-1.0.3-win64-portable.zip", 7, "d" * 64)]
    v2good = {
        "manifest_version": 2, "product": "mote", "channel": "stable",
        "key_id": "2026-11", "updated_at": "2026-11-18T04:48:54Z",
        "platforms": {"windows": {"version": "1.0.3", "build": 202611180530,
                                  "min_os": "10.0",
                                  "artifacts": {"portable": {
                                      "url": "https://dl.xiaotu.online/mote/mote-1.0.3-win64-portable.zip",
                                      "size": 7, "sha256": "d" * 64}}}},
    }
    check_manifest_v2(v2good, "mote", "win", v2arts, "1.0.3", DEFAULT_BASE, "stable",
                      build=202611180530); n += 1
    bad = json.loads(json.dumps(v2good))
    bad["platforms"]["windows"]["build"] = 1
    _expect_error(lambda: check_manifest_v2(bad, "mote", "win", v2arts, "1.0.3", DEFAULT_BASE,
                                            "stable", build=202611180530), "构建号"); n += 1
    bad = json.loads(json.dumps(v2good))
    bad["platforms"]["windows"]["artifacts"]["portable"]["sha256"] = "e" * 64
    _expect_error(lambda: check_manifest_v2(bad, "mote", "win", v2arts, "1.0.3", DEFAULT_BASE,
                                            "stable", build=202611180530), "SHA256"); n += 1
    bad = json.loads(json.dumps(v2good))
    bad["platforms"] = {"macos": bad["platforms"]["windows"]}
    _expect_error(lambda: check_manifest_v2(bad, "mote", "win", v2arts, "1.0.3", DEFAULT_BASE,
                                            "stable", build=202611180530), "缺少 windows"); n += 1
    bad = json.loads(json.dumps(v2good))
    bad["updated_at"] = "2026-11-18 04:48:54"
    _expect_error(lambda: check_manifest_v2(bad, "mote", "win", v2arts, "1.0.3", DEFAULT_BASE,
                                            "stable", build=202611180530), "updated_at"); n += 1
    bad = json.loads(json.dumps(v2good))
    bad["platforms"]["windows"]["artifacts"]["portable"]["url"] = "https://dl.xiaotu.online/mote/Mote-1.0.3.zip"
    _expect_error(lambda: check_manifest_v2(bad, "mote", "win", v2arts, "1.0.3", DEFAULT_BASE,
                                            "stable", build=202611180530), "与 v2 命名不一致"); n += 1

    # 绿色包内嵌地址（v2：manifest.json）
    with tempfile.TemporaryDirectory() as td:
        zp3 = Path(td) / "mote-1.0.3-win64-portable.zip"
        with zipfile.ZipFile(zp3, "w") as z:
            z.writestr("Mote-Windows/Mote.exe",
                       b"https://dl.xiaotu.online/mote/updates/stable/manifest.json")
        check_win_embed(zp3, "mote", "stable", DEFAULT_BASE, "v2"); n += 1
        zp4 = Path(td) / "mote-1.0.4-win64-portable.zip"
        with zipfile.ZipFile(zp4, "w") as z:
            z.writestr("Mote-Windows/Mote.exe",
                       b"https://dl.xiaotu.online/mote/updates/stable/windows.json")
        _expect_error(lambda: check_win_embed(zp4, "mote", "stable", DEFAULT_BASE, "v2"),
                      "旧构建产物"); n += 1

    # ---- EXE 内 build stamp 二次校验 ----
    with tempfile.TemporaryDirectory() as td:
        # 1) 绿色包内 Mote.exe 含正确 build（UTF-8）→ 通过
        zp5 = Path(td) / "mote-1.0.3-win64-portable.zip"
        with zipfile.ZipFile(zp5, "w") as z:
            z.writestr("Mote.exe", b"junk MOTE_BUILD=202611181542 end")
        check_win_build_stamp(zp5, 202611181542, "mote"); n += 1

        # 2) 绿色包内 Mote.exe 含正确 build（UTF-16LE）→ 通过
        zp6 = Path(td) / "mote-1.0.3-win64-portable.zip"
        with zipfile.ZipFile(zp6, "w") as z:
            stamp16 = b'M\x00O\x00T\x00E\x00_\x00B\x00U\x00I\x00L\x00D\x00=\x00' \
                      b'2\x000\x002\x006\x001\x001\x001\x008\x001\x005\x004\x002\x00'
            z.writestr("Mote.exe", stamp16 + b"junk")
        check_win_build_stamp(zp6, 202611181542, "mote"); n += 1

        # 3) build 不一致 → die
        _expect_error(lambda: check_win_build_stamp(zp5, 999999999999, "mote"),
                      "EXE 内构建号"); n += 1

        # 4) 缺少 build stamp → warn 跳过（不 die，兼容旧产物）
        zp_nobuild = Path(td) / "nobuild.zip"
        with zipfile.ZipFile(zp_nobuild, "w") as z:
            z.writestr("Mote.exe", b"no build stamp here")
        check_win_build_stamp(zp_nobuild, 202611181542, "mote"); n += 1  # 只是 warn，不抛错

        # 5) DevCard 跳过（_BUILD_STAMP_PREFIX 为 None）
        assert _extract_build_from_bytes(b"anything", "devcard") is None; n += 1

    print(f"selftest 通过（{n} 项）")
    return 0


# ---------------------------------------------------------------- doctor

def _doctor_check_file(path_str: str, desc: str, *, required: bool = True) -> Tuple[str, str]:
    """检查某个文件/工具路径是否存在。返回 (图标, 状态行)。"""
    p = Path(path_str).expanduser()
    if p.is_file() or p.is_dir():
        return "✓", f"{desc}\n    {p}"
    # 环境变量形式 —— 如果 path_str 是 $VAR 引用，查 env
    env_match = re.match(r'^\$(\w+)', path_str)
    if env_match:
        val = os.environ.get(env_match.group(1))
        if val:
            return "✓", f"{desc}（环境变量 {env_match.group(1)}）\n    {val}"
    if required:
        return "✗", f"{desc} 缺失\n    {p}"
    return "!", f"{desc}（可选，未配置）\n    {p}"


def _doctor_check_cmd(cmd: str, desc: str) -> Tuple[str, str]:
    """检查某个 shell 命令是否可用（which）。"""
    import shutil
    found = shutil.which(cmd)
    if found:
        return "✓", f"{desc}\n    {found}"
    return "✗", f"{desc}（未在 PATH 中找到 {cmd}）"


def cmd_doctor(args: argparse.Namespace) -> int:
    """检查发布环境所需的凭据、密钥、工具是否就绪。"""
    product = args.product
    plat = args.platform

    # 凭据/密钥默认路径表（与 publish_macos.sh / publish_windows.sh 保持同步）
    # key: (product, 类别) → 默认路径
    # 类别: sign_key, sparkle_key, oss_env, oss_py, sparkle_bin
    CONFIG_DIRS = {
        "mote": {
            "sign_key": "~/.config/mote-updates/ed25519_seed.b64",
            "sparkle_key": "~/.config/mote-updates/sparkle_ed25519_seed.b64",
        },
        "xiaotu": {
            "sign_key": "~/.config/xiaotu-updates/update_signing_2026.pem",
        },
        "devcard": {
            "sign_key": "~/.config/devcard/manifest-keys/ed25519_seed.b64",
            "sparkle_key": "~/.config/devcard/sparkle-keys/private_key.pem",
        },
    }
    OSS_ENV = "~/.config/xiaotu-updates/.env"  # 三产品共用
    OSS_PY = "~/.venv-oss/bin/python"          # oss2 虚拟环境

    checks: List[Tuple[str, str]] = []  # (icon, desc)

    print(f"guard doctor — {product} 发布环境检查（platform={plat}）\n")

    # ---- 签名密钥 ----
    keys = CONFIG_DIRS.get(product, {})
    if "sign_key" in keys:
        icon, line = _doctor_check_file(keys["sign_key"], "v2 清单 Ed25519 签名种子")
        checks.append((icon, line))
        print(f"  {icon} {line}")
    if plat in ("mac", "both") and "sparkle_key" in keys:
        icon, line = _doctor_check_file(keys["sparkle_key"], "Sparkle Ed25519 签名私钥", required=False)
        checks.append((icon, line))
        print(f"  {icon} {line}")

    # ---- OSS 凭据 ----
    icon, line = _doctor_check_file(OSS_ENV, "OSS 凭据文件（三产品共用）", required=False)
    checks.append((icon, line))
    print(f"  {icon} {line}")

    # ---- oss2 Python 环境 ----
    icon, line = _doctor_check_file(OSS_PY, "oss2 虚拟环境（.venv-oss/bin/python）", required=False)
    checks.append((icon, line))
    print(f"  {icon} {line}")

    # ---- Sparkle sign_update（macOS only）----
    if plat in ("mac", "both"):
        found = False
        sparkle_bin = os.environ.get("SPARKLE_BIN")
        if sparkle_bin and Path(f"{sparkle_bin}/sign_update").is_file():
            print(f"  ✓ Sparkle sign_update（SPARKLE_BIN）\n    {sparkle_bin}/sign_update")
            checks.append(("✓", f"SPARKLE_BIN={sparkle_bin}"))
            found = True
        else:
            # 搜索 ~/Downloads/Sparkle-*/bin/sign_update
            hits = glob.glob(str(Path.home() / "Downloads/Sparkle-*/bin/sign_update"))
            if hits:
                print(f"  ✓ Sparkle sign_update（自动探测）\n    {hits[0]}")
                checks.append(("✓", hits[0]))
                found = True
        if not found:
            print(f"  ! Sparkle sign_update（macOS 自动更新需要，未找到）\n    设置 SPARKLE_BIN 指向 Sparkle 发行包的 bin 目录")
            checks.append(("!", "SPARKLE_BIN 未设置"))

    # ---- 系统工具 ----
    print()
    for cmd in ("cmake", "git"):
        icon, line = _doctor_check_cmd(cmd, f"系统命令 {cmd}")
        checks.append((icon, line))
        print(f"  {icon} {line}")
    if plat in ("mac", "both"):
        for cmd in ("spctl", "codesign", "hdiutil", "xcrun"):
            icon, line = _doctor_check_cmd(cmd, f"macOS 工具 {cmd}")
            checks.append((icon, line))
            print(f"  {icon} {line}")

    # ---- 汇总 ----
    ok = sum(1 for c in checks if c[0] == "✓")
    warn = sum(1 for c in checks if c[0] == "!")
    fail = sum(1 for c in checks if c[0] == "✗")
    print()
    print(f"总计：{ok} ✓ / {warn} ! / {fail} ✗")
    if fail:
        print("有必需项缺失，补齐后才能发布。")
        return 1
    if warn:
        print("有可选项未配置（如 Sparkle / oss2），某些发布步骤会跳过。")
    return 0


# ---------------------------------------------------------------- CLI

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="三产品发布门禁")
    p.add_argument("--version", action="version", version=GUARD_VERSION)
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("selftest", help="离线自检")
    sub.add_parser("version", help="打印版本")
    for name in ("doctor",):
        s = sub.add_parser(name, help="检查发布凭据/密钥/工具是否就绪")
        s.add_argument("--product", required=True, choices=sorted(PRODUCTS))
        s.add_argument("--platform", default="mac", choices=["mac", "win", "both"],
                       help="要发布的目标平台（默认 mac）")

    for name in ("preflight", "verify-live"):
        s = sub.add_parser(name)
        s.add_argument("--product", required=True, choices=sorted(PRODUCTS))
        s.add_argument("--kind", required=True, choices=["mac", "win"])
        s.add_argument("--file", required=True, help="dmg / setup.exe / zip")
        s.add_argument("--zip", help="Windows 绿色包（与 setup 一起校验）")
        s.add_argument("--name", help="主产物远端文件名（本地名不规范时使用）")
        s.add_argument("--zip-name", help="绿色包远端文件名")
        s.add_argument("--channel", default="stable")
        s.add_argument("--base", default=DEFAULT_BASE)
        s.add_argument("--version", dest="version", help="断言文件名版本")
        s.add_argument("--offline", action="store_true", help="preflight 跳过线上检查")
        s.add_argument("--manifest", help="v2 preflight：本地已签名 manifest.json 路径")
        s.add_argument("--build", type=int, default=None,
                       help="v2：本次发布的 Windows 构建号")
        s.add_argument("--pub", help="v2：Ed25519 公钥文件（PEM/hex/base64）用于验签")
        s.add_argument("--pub-hex", help="v2：Ed25519 公钥 hex（与 --pub 二选一）")
    return p


def main(argv: Optional[List[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.cmd == "selftest":
            return cmd_selftest()
        if args.cmd == "version":
            print(GUARD_VERSION)
            return 0
        if args.cmd == "doctor":
            return cmd_doctor(args)
        if args.cmd == "preflight":
            return cmd_preflight(args)
        return cmd_verify_live(args)
    except GuardError as e:
        print(f"guard: {e}", file=sys.stderr)
        return 1
    except subprocess.CalledProcessError as e:
        print(f"guard: 外部命令失败：{e}", file=sys.stderr)
        return 1
    except zipfile.BadZipFile as e:
        print(f"guard: 不是合法的 zip 文件：{e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
