#!/usr/bin/env python3
"""更新 OSS 桶根 index.html 里小兔进销存的三个包条目。

用法：
  python tools/package/update_index.py --fetch   # 从 OSS 拉当前 index.html 到 _oss_index_raw.html
  python tools/package/update_index.py --local   # 只本地更新（读 _oss_index_raw.html → _oss_index_v100.html）
  python tools/package/update_index.py --apply   # 完整流程：fetch → 更新 → 上传 OSS → purge CDN

--apply 需要 OSS_ACCESS_KEY_ID / OSS_ACCESS_KEY_SECRET / OSS_BUCKET / OSS_REGION 环境变量。
"""
import argparse
import hashlib
import pathlib
import re
import sys
import urllib.request

REPO = pathlib.Path(__file__).resolve().parent.parent.parent
TOOLS_PKG = REPO / "tools" / "package"
VERSION = (REPO / "VERSION").read_text().strip()

# ---- 三个包的元信息（本地 build 出来的，或等 Mac build dmg 后填入） ----
PACKAGES = {
    "mac": {
        "label": "macOS",
        "ext": "dmg",
        "file": f"xiaotu-inventory-{VERSION}.dmg",
        "size": None,          # 需在 Mac build 后填入
        "sha256": None,        # 同上
        "note": "（Mac build 后补）",
    },
    "setup": {
        "label": "Windows 安装包",
        "ext": "exe",
        "file": f"xiaotu-inventory-{VERSION}-win64-setup.exe",
        "size": 3768031,
        "sha256": "E3A7C9374695DAEC21CC19713E1DBAF9605148AE7BD7E393A01BEA504DA26C1E",
        "note": "",
    },
    "zip": {
        "label": "Windows 绿色包",
        "ext": "zip",
        "file": f"xiaotu-inventory-{VERSION}-win64.zip",
        "size": 2372656,
        "sha256": "D19F97CEBA6765437E18E5F24C4ACFB99A10C83884E0649489B75A06564F14E5",
        "note": "",
    },
}

OSS_DOMAIN = "https://dl.xiaotu.online"
PRODUCT_DIR = "xiaotu"

# ---- 辅助函数 ----
def human_mb(n: int) -> str:
    return f"{n / 1024 / 1024:.1f}"

def fetch_index() -> str:
    url = f"{OSS_DOMAIN}/"
    print(f"[fetch] {url}")
    with urllib.request.urlopen(url, timeout=15) as r:
        return r.read().decode("utf-8")

def save_raw(html: str):
    p = TOOLS_PKG / "_oss_index_raw.html"
    p.write_text(html, encoding="utf-8")
    print(f"[save] {p} ({len(html)} bytes)")

# aria-label → package key 的映射，决定每个 <section> 替换哪个包
ARIA_TO_KEY = {
    "macOS 下载": "mac",
    "Windows 安装包下载": "setup",
    "Windows 绿色包下载": "zip",
}

# aria-label → href 文件后缀（用于定位旧文件名）
ARIA_TO_HREF = {
    "macOS 下载": r"xiaotu-inventory-[\d.]+\.dmg",
    "Windows 安装包下载": r"xiaotu-inventory-[\d.]+-win64-setup\.exe",
    "Windows 绿色包下载": r"xiaotu-inventory-[\d.]+-win64\.zip",
}

def apply_updates(html: str) -> str:
    """在小兔进销存卡片内按 section 隔离替换版本号、大小、SHA256、下载链接。"""
    # 找到小兔进销存卡片边界
    card_start = html.find('<!-- ================= 小兔进销存 ================= -->')
    if card_start < 0:
        raise SystemExit("找不到小兔进销存卡片注释")
    card_end = html.find('<!-- =================', card_start + 1)
    if card_end < 0:
        raise SystemExit("找不到下一张卡片边界")

    card = html[card_start:card_end]
    updated = card

    # 逐个 <section class="dlrow"> 隔离更新
    for aria_label, pkg_key in ARIA_TO_KEY.items():
        updated = _update_dlrow(updated, aria_label, pkg_key)

    # 卡片内底部 hash 校验命令示例里的旧版本号（在 section 隔离更新之前处理过一次
    # 卡片内部的；这里再处理一次，因为可能还有漏网之鱼）
    updated = updated.replace("xiaotu-inventory-0.9.0", f"xiaotu-inventory-{VERSION}")

    result = html[:card_start] + updated + html[card_end:]

    # 卡外也有 hash 命令示例（在三个产品卡片之外的通用 footer 区域）
    result = result.replace("xiaotu-inventory-0.9.0", f"xiaotu-inventory-{VERSION}")

    return result

def _update_dlrow(card: str, aria_label: str, pkg_key: str) -> str:
    """只在指定 aria-label 的 <section> 范围内做替换。"""
    pkg = PACKAGES[pkg_key]
    section_open = f'<section class="dlrow" aria-label="{aria_label}"'
    start = card.find(section_open)
    if start < 0:
        print(f"  [warn] 找不到 section aria-label={aria_label}")
        return card

    # 找这个 section 的结束位置：下一个 <section 或 </section> 紧接 </div></div>
    next_section = card.find('<section class="dlrow"', start + 1)
    section_close = card.find('</section>', start)
    if next_section > 0 and section_close > 0 and section_close < next_section:
        end = section_close + len('</section>')
    elif next_section > 0:
        end = next_section
    elif section_close > 0:
        end = section_close + len('</section>')
    else:
        end = len(card)

    section_text = card[start:end]

    # 1. 版本号（<span class="mono">v0.9.0</span> 无空格）
    section_text = section_text.replace(">v0.9.0<", f">v{VERSION}<")

    # 2. 下载文件名（href）
    href_suffix = ARIA_TO_HREF[aria_label]
    section_text = re.sub(
        rf"(href=\"https://dl\.xiaotu\.online/{PRODUCT_DIR}/){href_suffix}(\")",
        rf"\g<1>{pkg['file']}\g<2>",
        section_text,
    )

    if pkg["size"] is None or pkg["sha256"] is None:
        print(f"  [skip] {pkg['label']}：size/SHA256 未提供（等 Mac build）")
        return card[:start] + section_text + card[end:]

    # 3. 大小
    size_re = r'<span class="mono">\d[\d,]*</span> 字节（约 \d+\.\d+ MB）'
    size_match = re.search(size_re, section_text)
    if size_match:
        new_size = f'<span class="mono">{pkg["size"]:,}</span> 字节（约 {human_mb(pkg["size"])} MB）'
        section_text = section_text[:size_match.start()] + new_size + section_text[size_match.end():]

    # 4. SHA256（保留 id 属性，更新 title 和内容）
    sha_re = r'<code class="sha-value"[^>]*>[0-9a-f]{64}</code>'
    sha_match = re.search(sha_re, section_text)
    if sha_match:
        old_sha = sha_match.group(0)
        new_sha = re.sub(
            r'title="[^"]*"',
            f'title="{pkg["sha256"].lower()}"',
            old_sha,
        )
        new_sha = re.sub(
            r'>[0-9a-f]{64}</code>',
            f'>{pkg["sha256"].lower()}</code>',
            new_sha,
        )
        section_text = section_text[:sha_match.start()] + new_sha + section_text[sha_match.end():]

    print(f"  [ok]  {pkg['label']}: v{VERSION} · {pkg['size']:,} bytes · SHA256 {pkg['sha256'][:16]}…")
    return card[:start] + section_text + card[end:]

def upload_to_oss(html: str):
    """需要 oss2 和 OSS 环境变量。"""
    import oss2
    import os

    ak = os.environ.get("OSS_ACCESS_KEY_ID")
    sk = os.environ.get("OSS_ACCESS_KEY_SECRET")
    bucket = os.environ.get("OSS_BUCKET")
    region = os.environ.get("OSS_REGION")
    if not all([ak, sk, bucket, region]):
        raise SystemExit("缺少 OSS_ACCESS_KEY_ID / OSS_ACCESS_KEY_SECRET / OSS_BUCKET / OSS_REGION")

    auth = oss2.Auth(ak, sk)
    bkt = oss2.Bucket(auth, f"https://oss-{region}.aliyuncs.com", bucket)

    # 上传新包
    for key in ("setup", "zip"):
        pkg = PACKAGES[key]
        local = REPO / ("packaging/win/out" if key == "setup" else "build/package/win") / pkg["file"]
        if local.exists():
            obj_key = f"{PRODUCT_DIR}/{pkg['file']}"
            print(f"[oss] PUT {obj_key} ({local.stat().st_size:,} bytes)")
            bkt.put_object_from_file(obj_key, str(local))
        else:
            print(f"  [warn] 本地包不存在：{local}")

    # 上传 index.html
    bkt.put_object("index.html", html.encode("utf-8"), headers={"Content-Type": "text/html; charset=utf-8"})
    print("[oss] PUT index.html 完成")

def purge_cdn():
    """阿里云 CDN 刷新：需要 aliyun CLI 或 API，这里给命令模板。"""
    print("")
    print("[purge] 请手动刷新 CDN 缓存：")
    print(f"  aliyun cdn RefreshObjectCaches --object-path 'https://dl.xiaotu.online/'")
    print(f"  aliyun cdn RefreshObjectCaches --object-path 'https://dl.xiaotu.online/{PRODUCT_DIR}/'")
    print(f"  aliyun cdn RefreshObjectCaches --object-path 'https://dl.xiaotu.online/{PRODUCT_DIR}/xiaotu-inventory-{VERSION}-win64-setup.exe'")
    print(f"  aliyun cdn RefreshObjectCaches --object-path 'https://dl.xiaotu.online/{PRODUCT_DIR}/xiaotu-inventory-{VERSION}-win64.zip'")

def main():
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--fetch", action="store_true", help="从 OSS 拉当前 index.html")
    g.add_argument("--local", action="store_true", help="只本地更新（读 _oss_index_raw.html → _oss_index_v100.html）")
    g.add_argument("--apply", action="store_true", help="fetch → 更新 → 上传 OSS → purge")
    args = ap.parse_args()

    if args.fetch:
        html = fetch_index()
        save_raw(html)
        print(f"版本号：{VERSION}")
        return

    if args.local:
        raw_path = TOOLS_PKG / "_oss_index_raw.html"
        if not raw_path.exists():
            print(f"需要先 --fetch 或手动保存到 {raw_path}", file=sys.stderr)
            sys.exit(2)
        html = raw_path.read_text(encoding="utf-8")

    if args.apply:
        html = fetch_index()

    updated_html = apply_updates(html)
    out = TOOLS_PKG / f"_oss_index_v{VERSION.replace('.', '')}.html"
    out.write_text(updated_html, encoding="utf-8")
    print(f"\n[saved] {out}")

    if args.apply:
        upload_to_oss(updated_html)
        purge_cdn()
    elif args.local:
        print("\n[local] 未上传。如需发布，在 Mac 上跑：")
        print(f"  python3 tools/package/update_index.py --apply")
        print("  或手动上传 index.html 到 OSS 桶根 + 刷新 CDN")

if __name__ == "__main__":
    main()
