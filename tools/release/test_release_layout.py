#!/usr/bin/env python3
"""发布脚本布局与构建号注入口径回归测试（三仓同一份模板）。

覆盖：
- 打包 / 发布脚本的规范路径存在，旧路径与旧名不再出现；
- 打包脚本缺省调用 gen_build.py，并显式开启 <PREFIX>_RELEASE_BUILD；
- 发布脚本按约定链路确定构建号（CMakeCache / 绿色包 exe / gen_build.py）；
- 根 CMakeLists 的严格开关、开发占位值与 unset 语义；
- 三仓共享的 gen_build.py 哈希一致（与 sync_guard.sh 对齐）；
- 共享脚本的 .sha256 台账与仓库内文件哈希一致；
- CMake preset 的发布配置统一：macos-release→build/release、windows-release→build/windows；
- 本地产物/清单统一落 dist/，打包与发布脚本不再引用旧构建目录；
- CI / 打包入口统一走 macos-release / windows-release 预设。

运行：python3 tools/release/test_release_layout.py
"""
from __future__ import annotations

import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
PRODUCT = "xiaotu"
PREFIX = "XIAOTU"
CMAKE_FILE = "CMakeLists.txt"

PACKAGE_SCRIPTS = [
    "tools/package/package_macos.sh",
    "tools/package/package_windows.ps1",
]
# 每个发布脚本里必须出现的构建号来源标记
PUBLISH_SCRIPTS = {
    "tools/release/publish_macos.sh": ["gen_build.py"],
    "tools/release/publish_windows.sh": ["gen_build.py"],
    "tools/release/publish_windows.ps1": ["CMakeCache", "gen_build.py"],
}
LEGACY_PATHS = [
    "tools/package/make_dmg.sh",
    "tools/package/publish_oss.sh",
    "tools/package/publish_windows.sh",
    "tools/package/publish_windows.ps1",
    "tools/package/package_windows.cmd",
    "packaging/win/build-release.ps1",
    "packaging/win/build-release.cmd",
]
LEGACY_NAMES = ["publish_oss.sh", "make_dmg.sh", "build-release.ps1", "build-release.cmd"]
GEN_BUILD_MD5 = "fa077f04f9dd8d7e2f05692178e1ed02"
# 共享脚本的哈希台账：每行 `<sha256>  <repo 相对路径>`
SHARED_HASH_FILES = [
    "guard.sha256",
    "manifest_v2.sha256",
    "release_tag.sha256",
    "publish_release.sha256",
]
PRESETS_FILE = "CMakePresets.json"
# 目录统一后，打包 / 发布脚本里不应再出现的旧目录
LEGACY_DIR_PATTERNS = [r"build/package", r"packaging/win/out"]

failures = 0


def check(ok: bool, message: str) -> None:
    global failures
    if not ok:
        failures += 1
        print(f"FAIL: {message}")


def read(rel: str) -> str:
    path = REPO / rel
    check(path.is_file(), f"缺少 {rel}")
    return path.read_text(encoding="utf-8", errors="ignore") if path.is_file() else ""


def test_layout() -> None:
    for rel in LEGACY_PATHS:
        check(not (REPO / rel).exists(), f"旧脚本路径仍存在：{rel}")
    for rel in PACKAGE_SCRIPTS:
        check((REPO / rel).is_file(), f"缺少打包脚本 {rel}")


def test_no_legacy_references() -> None:
    pattern = "|".join(name.replace(".", r"\.") for name in LEGACY_NAMES)
    try:
        out = subprocess.run(
            ["git", "grep", "-l", "-E", pattern, "--", ".",
             ":(exclude)tools/release/test_release_layout.py"],
            cwd=REPO, capture_output=True, text=True, check=False,
        )
    except FileNotFoundError:
        print("SKIP: 未安装 git，跳过旧名引用扫描")
        return
    hits = [line.strip() for line in out.stdout.splitlines() if line.strip()]
    check(not hits, "仍有文件引用旧脚本名：" + ", ".join(hits))


def test_packaging() -> None:
    for rel in PACKAGE_SCRIPTS:
        text = read(rel)
        check("gen_build.py" in text, f"{rel} 未调用 gen_build.py 兜底构建号")
        check(f"-D{PREFIX}_RELEASE_BUILD=ON" in text,
              f"{rel} 未显式开启 -D{PREFIX}_RELEASE_BUILD=ON")


def test_publish_chain() -> None:
    for rel, markers in PUBLISH_SCRIPTS.items():
        text = read(rel)
        for marker in markers:
            check(marker in text, f"{rel} 缺少构建号来源标记 {marker}")


def test_cmake_strict_switch() -> None:
    text = read(CMAKE_FILE)
    check(f"{PREFIX}_BUILD" in text, f"{CMAKE_FILE} 缺少 {PREFIX}_BUILD 变量")
    check(f"{PREFIX}_RELEASE_BUILD" in text, f"{CMAKE_FILE} 缺少 {PREFIX}_RELEASE_BUILD 开关")
    check("999999999" in text, f"{CMAKE_FILE} 缺少开发占位值 999999999")
    check(f"unset({PREFIX}_RELEASE_BUILD CACHE)" in text,
          f"{CMAKE_FILE} 未在配置末尾清理 {PREFIX}_RELEASE_BUILD 缓存")


def test_gen_build_synced() -> None:
    path = REPO / "tools/release/gen_build.py"
    if not path.is_file():
        check(False, "缺少 tools/release/gen_build.py")
        return
    # Windows 检出会把 .py 转成 CRLF，先归一化再算哈希，否则固定 MD5 永远对不上
    digest = hashlib.md5(path.read_bytes().replace(b"\r\n", b"\n")).hexdigest()
    check(digest == GEN_BUILD_MD5,
          f"gen_build.py 与三仓同步版本不一致（{digest}，期望 {GEN_BUILD_MD5}）")


def test_shared_hashes() -> None:
    for name in SHARED_HASH_FILES:
        path = REPO / "tools" / "release" / name
        if not path.is_file():
            check(False, f"缺少 tools/release/{name}")
            continue
        for line in path.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line:
                continue
            parts = line.split(None, 1)
            if len(parts) != 2:
                check(False, f"{name} 行格式错误：{line!r}")
                continue
            expected, rel = parts[0].lower(), parts[1].strip()
            target = REPO / rel
            if not target.is_file():
                check(False, f"{name} 指向的文件不存在：{rel}")
                continue
            actual = hashlib.sha256(target.read_bytes()).hexdigest()
            check(actual == expected,
                  f"{name}: {rel} SHA-256 不一致（期望 {expected}，实际 {actual}）")


def _normalize_binary_dir(raw: str) -> str:
    """把 ${sourceDir}/... 与 ../build/... 归一化成仓库根相对路径。"""
    parts: list[str] = []
    for piece in raw.replace("\\", "/").replace("${sourceDir}", "").split("/"):
        if piece in ("", "."):
            continue
        if piece == "..":
            if parts:
                parts.pop()
            continue
        parts.append(piece)
    return "/".join(parts)


def test_presets() -> None:
    path = REPO / PRESETS_FILE
    if not path.is_file():
        check(False, f"缺少构建预设文件 {PRESETS_FILE}")
        return
    try:
        presets = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        check(False, f"{PRESETS_FILE} 不是合法 JSON：{exc}")
        return
    by_name = {item.get("name"): item for item in presets.get("configurePresets", [])}
    for name, suffix in (("macos-release", "build/release"),
                         ("windows-release", "build/windows")):
        preset = by_name.get(name)
        check(preset is not None, f"{PRESETS_FILE} 缺少 configurePreset {name}")
        if preset is None:
            continue
        check(preset.get("generator") == "Ninja",
              f"{name} 的 generator 应为 Ninja，实际 {preset.get('generator')!r}")
        normalized = _normalize_binary_dir(str(preset.get("binaryDir", "")))
        check(normalized.endswith(suffix),
              f"{name} 的 binaryDir 应以 {suffix} 结尾，实际 {preset.get('binaryDir')!r}")


def _script_texts() -> dict[str, str]:
    texts: dict[str, str] = {}
    for rel in list(PUBLISH_SCRIPTS) + PACKAGE_SCRIPTS:
        path = REPO / rel
        if path.is_file():
            texts[rel] = path.read_text(encoding="utf-8", errors="ignore")
    return texts


def test_artifact_dirs() -> None:
    texts = _script_texts()
    for rel in PUBLISH_SCRIPTS:
        text = texts.get(rel)
        if text is None:
            continue
        check("dist" in text, f"{rel} 未把本地产物/清单落到 dist/")
        check("manifest.json" in text, f"{rel} 未生成或读取 manifest.json")
        if rel.endswith("publish_macos.sh"):
            check("appcast.xml" in text, f"{rel} 缺少 appcast.xml")
    for rel in PACKAGE_SCRIPTS:
        text = texts.get(rel)
        if text is None:
            continue
        check("dist" in text, f"{rel} 未把产物输出到 dist/")
    for rel, text in texts.items():
        for pattern in LEGACY_DIR_PATTERNS:
            check(re.search(pattern, text) is None,
                  f"{rel} 仍引用旧目录（匹配 /{pattern}/）")


def test_build_entry_uses_presets() -> None:
    if PRODUCT == "mote":
        check("macos-release" in read("tools/package/package_macos.sh"),
              "tools/package/package_macos.sh 未使用 macos-release 预设")
        check("windows-release" in read("tools/package/package_windows.ps1"),
              "tools/package/package_windows.ps1 未使用 windows-release 预设")
    for rel, required in ((".github/workflows/release-windows.yml", True),
                          (".github/workflows/ci.yml", False)):
        path = REPO / rel
        if not path.is_file():
            check(not required, f"缺少 {rel}")
            continue
        text = path.read_text(encoding="utf-8", errors="ignore")
        check("windows-release" in text, f"{rel} 未使用 windows-release 预设")
    if PRODUCT != "mote":
        # mac 相关 workflow（存在才查；ci.yml 若存在也算 mac 侧入口）
        workflow_dir = REPO / ".github" / "workflows"
        mac_workflows = sorted(workflow_dir.glob("*mac*")) if workflow_dir.is_dir() else []
        ci = workflow_dir / "ci.yml"
        if ci.is_file():
            mac_workflows.append(ci)
        for path in mac_workflows:
            text = path.read_text(encoding="utf-8", errors="ignore")
            check("macos-release" in text, f"{path.name} 未使用 macos-release 预设")


def test_release_attachments() -> None:
    text = read("tools/release/publish_release.sh")
    check("dist/" in text, "publish_release.sh 未从 dist/ 收集附件")
    check("build/package" not in text, "publish_release.sh 仍引用旧目录 build/package")


for test in (test_layout, test_no_legacy_references, test_packaging,
             test_publish_chain, test_cmake_strict_switch, test_gen_build_synced,
             test_shared_hashes, test_presets, test_artifact_dirs,
             test_build_entry_uses_presets,
             test_release_attachments):
    test()

print(f"SUMMARY failures={failures}")
sys.exit(1 if failures else 0)
