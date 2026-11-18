#!/usr/bin/env python3
"""三产品发布门禁（单一规则源）。

只依赖 Python 标准库：不读 OSS 凭据，所有线上检查都走公开 HTTP。
macOS / Windows / CI 用同一份代码，避免三套脚本各自漂移。

子命令：
  selftest       离线自检（CI 每次跑）
  preflight      上传前：文件名、版本、目标键、覆盖风险、内嵌地址
  verify-live    上传后：清单字段、大小、SHA256、Content-Type
  version        打印 GUARD_VERSION

约定见 mote/docs/OSS-发布布局.md。
"""

from __future__ import annotations

import argparse
import hashlib
import json
import plistlib
import re
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

GUARD_VERSION = "1.0.0"
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


def check_mac_embed(path: Path, product: str, channel: str, base: str, version: str) -> None:
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
        want = f"{base}/{manifest_key(product, channel, 'mac')}"
        got_url = str(info.get("XiaotuUpdateManifestURL", ""))
        if got_url != want:
            die(f"DMG 内 XiaotuUpdateManifestURL 与发布地址不一致：{got_url or '(空)'} != {want}")
    ok(f"DMG 内嵌地址正确（{feed}）")


def check_win_embed(zip_path: Path, product: str, channel: str, base: str) -> None:
    spec = product_spec(product)
    exe_name = spec["win_embed_exe"].lower()
    url = f"{base}/{manifest_key(product, channel, 'win')}"
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
                      name: Optional[str] = None, zip_name: Optional[str] = None) -> List[Artifact]:
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
        versions.add(parse_version(display, r, product))
        size, sha, _ = file_digests(path)
        artifacts.append(Artifact(r, path, display, size, sha))
    if len(versions) != 1:
        die(f"产物版本不一致：{sorted(versions)}")
    return artifacts


def cmd_preflight(args: argparse.Namespace) -> int:
    check_channel(args.channel)
    base = args.base.rstrip("/")
    artifacts = collect_artifacts(args.product, args.kind, Path(args.file),
                                  Path(args.zip) if args.zip else None,
                                  args.name, args.zip_name)
    version = parse_version(artifacts[0].name, artifacts[0].role, args.product)
    if args.version and args.version != version:
        die(f"--version {args.version} 与文件名版本 {version} 不一致")
    print(f"guard preflight · {args.product} / {args.kind} / {version}")
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
        check_mac_embed(Path(args.file), args.product, args.channel, base, version)
    else:
        zip_path = Path(args.zip) if args.zip else (Path(args.file) if args.file.lower().endswith(".zip") else None)
        if zip_path:
            check_win_embed(zip_path, args.product, args.channel, base)
        else:
            warn("未提供绿色包，跳过壳内嵌地址检查")
    print("preflight 通过")
    return 0


def cmd_verify_live(args: argparse.Namespace) -> int:
    check_channel(args.channel)
    base = args.base.rstrip("/")
    artifacts = collect_artifacts(args.product, args.kind, Path(args.file),
                                  Path(args.zip) if args.zip else None,
                                  args.name, args.zip_name)
    version = parse_version(artifacts[0].name, artifacts[0].role, args.product)
    print(f"guard verify-live · {args.product} / {args.kind} / {version}")
    mkey = manifest_key(args.product, args.channel, args.kind)
    data = get_json(f"{base}/{mkey}", bust=True)
    if data is None:
        die(f"清单不存在：{base}/{mkey}")
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

    if args.product == "devcard" and args.kind == "mac":
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

    print(f"selftest 通过（{n} 项）")
    return 0


# ---------------------------------------------------------------- CLI

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="三产品发布门禁")
    p.add_argument("--version", action="version", version=GUARD_VERSION)
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("selftest", help="离线自检")
    sub.add_parser("version", help="打印版本")

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
    return p


def main(argv: Optional[List[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.cmd == "selftest":
            return cmd_selftest()
        if args.cmd == "version":
            print(GUARD_VERSION)
            return 0
        if args.cmd == "preflight":
            return cmd_preflight(args)
        return cmd_verify_live(args)
    except GuardError as e:
        print(f"guard: {e}", file=sys.stderr)
        return 1
    except subprocess.CalledProcessError as e:
        print(f"guard: 外部命令失败：{e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
