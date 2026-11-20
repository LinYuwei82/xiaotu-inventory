#!/usr/bin/env python3
"""发布脚本布局与构建号注入口径回归测试（三仓同一份模板）。

覆盖：
- 打包 / 发布脚本的规范路径存在，旧路径与旧名不再出现；
- 打包脚本缺省调用 gen_build.py，并显式开启 <PREFIX>_RELEASE_BUILD；
- 发布脚本按约定链路确定构建号（CMakeCache / 绿色包 exe / gen_build.py）；
- 根 CMakeLists 的严格开关、开发占位值与 unset 语义；
- 三仓共享的 gen_build.py 哈希一致（与 sync_guard.sh 对齐）；
- 共享脚本的 .sha256 台账与仓库内文件哈希一致；
- 更新判定向量 update_compare_vectors.json 与参考判定逐条一致；
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
    "gen_build.sha256",
    "publish_args.sha256",
    "update_compare_vectors.sha256",
]
UPDATE_VECTORS_FILE = "tools/release/update_compare_vectors.json"
VERSION_RE = re.compile(r"^\d+\.\d+\.\d+$")
VECTOR_FIELDS = ("remote_version", "remote_build", "local_version", "local_build", "newer")
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
            # Windows 检出会把文本转成 CRLF；哈希按 LF 归一化后比对
            actual = hashlib.sha256(
                target.read_bytes().replace(b"\r\n", b"\n")).hexdigest()
            check(actual == expected,
                  f"{name}: {rel} SHA-256 不一致（期望 {expected}，实际 {actual}）")


def _version_tuple(version: str) -> tuple[int, ...]:
    return tuple(int(part) for part in version.split("."))


def _reference_newer(remote_version: str, remote_build: int,
                     local_version: str, local_build: int) -> bool:
    """参考判定：版本按数字逐段比较；同版本且本机 build <= 0 时不算更新。"""
    remote = _version_tuple(remote_version)
    local = _version_tuple(local_version)
    if remote > local:
        return True
    if remote < local:
        return False
    if local_build <= 0:
        return False
    return remote_build > local_build


def test_update_vectors() -> None:
    path = REPO / UPDATE_VECTORS_FILE
    if not path.is_file():
        check(False, f"缺少 {UPDATE_VECTORS_FILE}")
        return
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        check(False, f"{UPDATE_VECTORS_FILE} 不是合法 JSON：{exc}")
        return
    cases = data.get("cases") if isinstance(data, dict) else None
    if not isinstance(cases, list):
        check(False, f"{UPDATE_VECTORS_FILE} 缺少 cases 列表")
        return
    check(len(cases) >= 8, f"cases 至少 8 条，实际 {len(cases)}")

    scenes: set[str] = set()
    for index, case in enumerate(cases):
        where = f"case[{index}]"
        if not isinstance(case, dict):
            check(False, f"{where} 不是对象")
            continue
        missing = [field for field in VECTOR_FIELDS if field not in case]
        if missing:
            check(False, f"{where} 缺少字段：{', '.join(missing)}")
            continue
        remote_version = case["remote_version"]
        remote_build = case["remote_build"]
        local_version = case["local_version"]
        local_build = case["local_build"]
        newer = case["newer"]
        valid = True
        if not (isinstance(remote_version, str) and VERSION_RE.fullmatch(remote_version)):
            check(False, f"{where} remote_version 应为 x.y.z，实际 {remote_version!r}")
            valid = False
        if not (isinstance(local_version, str) and VERSION_RE.fullmatch(local_version)):
            check(False, f"{where} local_version 应为 x.y.z，实际 {local_version!r}")
            valid = False
        if not isinstance(remote_build, int) or isinstance(remote_build, bool):
            check(False, f"{where} remote_build 应为 int，实际 {remote_build!r}")
            valid = False
        if not isinstance(local_build, int) or isinstance(local_build, bool):
            check(False, f"{where} local_build 应为 int，实际 {local_build!r}")
            valid = False
        if not isinstance(newer, bool):
            check(False, f"{where} newer 应为 bool，实际 {newer!r}")
            valid = False
        if not valid:
            continue

        expected = _reference_newer(remote_version, remote_build,
                                    local_version, local_build)
        check(newer == expected,
              f"{where} 判定不符：期望 newer={expected}，向量为 {newer}"
              f"（remote {remote_version}/{remote_build}，"
              f"local {local_version}/{local_build}）")

        # 场景覆盖只能按精确条件归类，避免 build 更低等其它向量被算进目标场景
        remote_t, local_t = _version_tuple(remote_version), _version_tuple(local_version)
        if remote_t > local_t:
            scenes.add("版本更高")
        elif remote_t < local_t:
            scenes.add("版本更低")
        else:
            if local_build == 0:
                scenes.add("local_build=0")
            elif local_build == -1:
                scenes.add("local_build=-1")
            elif local_build > 0 and remote_build > local_build:
                scenes.add("同版本 build 更高")
            elif local_build > 0 and remote_build == local_build:
                scenes.add("同版本 build 相等")

    for scene in ("版本更高", "同版本 build 更高", "同版本 build 相等",
                  "版本更低", "local_build=0", "local_build=-1"):
        check(scene in scenes, f"cases 未覆盖场景：{scene}")


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
             test_shared_hashes, test_update_vectors, test_presets,
             test_artifact_dirs,
             test_build_entry_uses_presets,
             test_release_attachments):
    test()

print(f"SUMMARY failures={failures}")
sys.exit(1 if failures else 0)
