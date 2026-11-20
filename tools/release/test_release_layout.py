#!/usr/bin/env python3
"""发布脚本布局与构建号注入口径回归测试（三仓同一份模板）。

覆盖：
- 打包 / 发布脚本的规范路径存在，旧路径与旧名不再出现；
- 打包脚本缺省调用 gen_build.py，并显式开启 <PREFIX>_RELEASE_BUILD；
- 发布脚本按约定链路确定构建号（CMakeCache / 绿色包 exe / gen_build.py）；
- 根 CMakeLists 的严格开关、开发占位值与 unset 语义；
- 三仓共享的 gen_build.py 哈希一致（与 sync_guard.sh 对齐）。

运行：python3 tools/release/test_release_layout.py
"""
from __future__ import annotations

import hashlib
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
    "packaging/win/build-release.ps1",
    "packaging/win/build-release.cmd",
]
LEGACY_NAMES = ["publish_oss.sh", "make_dmg.sh", "build-release.ps1", "build-release.cmd"]
GEN_BUILD_MD5 = "fa077f04f9dd8d7e2f05692178e1ed02"

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
    digest = hashlib.md5(path.read_bytes()).hexdigest()
    check(digest == GEN_BUILD_MD5,
          f"gen_build.py 与三仓同步版本不一致（{digest}，期望 {GEN_BUILD_MD5}）")


for test in (test_layout, test_no_legacy_references, test_packaging,
             test_publish_chain, test_cmake_strict_switch, test_gen_build_synced):
    test()

print(f"SUMMARY failures={failures}")
sys.exit(1 if failures else 0)
