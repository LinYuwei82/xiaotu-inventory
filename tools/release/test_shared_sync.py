#!/usr/bin/env python3
"""三仓共享发布文件一致性回归测试（三仓同一份模板）。

覆盖：
- guard.py / manifest_v2.py / release_tag.py / publish_release.sh / gen_build.py /
  publish_args.py / update_compare_vectors.json 及各自 .sha256 台账，
  在兄弟仓库之间逐字节一致（跨仓应完全一致，不做换行归一化）；
- 本地目录名可能是 mote 或 mote-md，两个候选都会尝试；
- 一个兄弟仓库都找不到（单仓 CI 环境）时整体跳过。

运行：python3 tools/release/test_shared_sync.py -v
"""
from __future__ import annotations

import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

SOURCE_FILES = [
    "guard.py",
    "manifest_v2.py",
    "release_tag.py",
    "publish_release.sh",
    "gen_build.py",
    "publish_args.py",
    "update_compare_vectors.json",
]
# 7 个源文件 + 各自的 .sha256 台账（台账名由 sync_guard.sh 约定，不带 .json 等扩展名）
HASH_FILES = [
    "guard.sha256",
    "manifest_v2.sha256",
    "release_tag.sha256",
    "publish_release.sha256",
    "gen_build.sha256",
    "publish_args.sha256",
    "update_compare_vectors.sha256",
]
SHARED_FILES = SOURCE_FILES + HASH_FILES

# 兄弟仓库候选目录名（Mote 本地目录可能是 mote 或 mote-md）
SIBLING_CANDIDATES = (
    ("xiaotu-inventory",),
    ("mote", "mote-md"),
    ("dev_card_cpp",),
)


def _sibling_repos() -> list[Path]:
    repos: list[Path] = []
    for candidates in SIBLING_CANDIDATES:
        for name in candidates:
            path = REPO.parent / name
            if path.is_dir() and path.resolve() != REPO:
                repos.append(path)
                break
    return repos


def _first_diff(ours: bytes, theirs: bytes) -> int:
    for offset, (a, b) in enumerate(zip(ours, theirs)):
        if a != b:
            return offset
    return min(len(ours), len(theirs))


class SharedSyncTest(unittest.TestCase):
    def test_shared_files_identical(self) -> None:
        siblings = _sibling_repos()
        if not siblings:
            raise unittest.SkipTest("未找到兄弟仓库，跳过跨仓一致性检查")
        for sibling in siblings:
            for name in SHARED_FILES:
                with self.subTest(repo=sibling.name, file=name):
                    local = REPO / "tools" / "release" / name
                    remote = sibling / "tools" / "release" / name
                    self.assertTrue(local.is_file(), f"本仓缺少 tools/release/{name}")
                    self.assertTrue(remote.is_file(),
                                    f"{sibling.name} 缺少 tools/release/{name}")
                    ours = local.read_bytes()
                    theirs = remote.read_bytes()
                    if ours == theirs:
                        continue
                    offset = _first_diff(ours, theirs)
                    self.fail(
                        f"{sibling.name}/tools/release/{name} 与本仓不一致："
                        f"首个差异偏移 {offset}"
                        f"（本仓 {len(ours)} 字节，对方 {len(theirs)} 字节）"
                    )


if __name__ == "__main__":
    unittest.main()
