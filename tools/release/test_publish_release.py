#!/usr/bin/env python3
"""publish_release.sh tag 选择逻辑回归测试（三仓同一份模板）。

覆盖：
- 无 tag 时拒绝并提示，且不自动创建 tag；
- 多个 v<版本>+<seq> tag 时选序号最大的；
- 显式 TAG 优先于自动选择；
- 显式 TAG 不存在时拒绝，且不创建该 tag；
- 已存在的 tag 复用，DRY_RUN 下不推送、不触网。

脚本依赖 macOS 的 stat -f 与 shasum，非 macOS 整文件跳过。

运行：python3 tools/release/test_publish_release.py -v
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
# 脚本靠仓库目录名识别产品，临时目录必须沿用真实仓库名
REPO_DIR_NAME = "xiaotu-inventory"
VERSION = "1.0.7"
ATTACHMENTS = [
    f"dist/xiaotu-inventory-{VERSION}.dmg",
    f"dist/xiaotu-inventory-{VERSION}-win64-setup.exe",
    f"dist/xiaotu-inventory-{VERSION}-win64.zip",
]

IS_DARWIN = sys.platform == "darwin"
# 外部环境里可能带着发布变量，运行时统一清掉，避免影响断言
PUBLISH_ENV_KEYS = ("TAG", "TITLE", "NOTES_FILE", "DRY_RUN",
                    "SKIP_GITEE", "SKIP_TAG_PUSH", "GITEE_TOKEN", "GITEE_ENV_FILE")


@unittest.skipUnless(IS_DARWIN, "publish_release.sh 使用 stat -f / shasum，仅在 macOS 运行")
class PublishReleaseTagTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory(prefix="publish-release-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.repo = self.root / REPO_DIR_NAME
        release_dir = self.repo / "tools" / "release"
        release_dir.mkdir(parents=True)
        shutil.copy2(REPO / "tools" / "release" / "publish_release.sh",
                     release_dir / "publish_release.sh")
        (self.repo / "VERSION.txt").write_text(VERSION + "\n", encoding="utf-8")
        self.notes = self.repo / "docs" / f"release-notes-{VERSION}.md"
        self.notes.parent.mkdir(parents=True)
        self.notes.write_text(f"# {VERSION}\n", encoding="utf-8")
        # 隔离 HOME，避免读到真实 gitee.env / gh 凭据
        self.home = self.root / "home"
        self.home.mkdir()

        self._git("init", "-q")
        self._git("config", "user.email", "test@example.com")
        self._git("config", "user.name", "test")
        self._git("add", "VERSION.txt", f"docs/release-notes-{VERSION}.md", "tools")
        self._git("commit", "-q", "-m", "init")

    def _git(self, *args: str) -> str:
        result = subprocess.run(["git", "-C", str(self.repo), *args],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout.strip()

    def _tags(self) -> list[str]:
        return [line for line in self._git("tag", "-l").splitlines() if line]

    def _make_attachments(self) -> None:
        for rel in ATTACHMENTS:
            path = self.repo / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"fake-artifact-bytes")

    def _run(self, env_extra: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
        env = {key: value for key, value in os.environ.items()
               if key not in PUBLISH_ENV_KEYS}
        env.update({"HOME": str(self.home), "SKIP_GITEE": "true"})
        env.update(env_extra or {})
        return subprocess.run(
            ["bash", "tools/release/publish_release.sh"],
            cwd=str(self.repo), capture_output=True, text=True, env=env, timeout=60,
        )

    def test_missing_tag_rejected(self) -> None:
        result = self._run()
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn(f"找不到 v{VERSION}+* tag", result.stderr)
        # 不自动造 tag
        self.assertEqual(self._tags(), [])

    def test_picks_highest_sequence_tag(self) -> None:
        for seq in (1, 3, 2):
            self._git("tag", f"v{VERSION}+{seq}")
        result = self._run()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(f"tag v{VERSION}+3", result.stdout)

    def test_explicit_tag_used(self) -> None:
        result = self._run({"TAG": f"v{VERSION}+9"})
        self.assertIn(f"tag v{VERSION}+9", result.stdout)

    def test_explicit_missing_tag_rejected(self) -> None:
        self._make_attachments()
        tag = f"v{VERSION}+9"
        result = self._run({"TAG": tag})
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("不存在", result.stderr)
        self.assertNotIn(tag, self._tags())

    def test_existing_tag_reused_without_network(self) -> None:
        self._git("tag", f"v{VERSION}+1")
        self._make_attachments()
        bin_dir = self.root / "bin"
        bin_dir.mkdir()
        gh = bin_dir / "gh"
        gh.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        gh.chmod(0o755)
        result = self._run({
            "DRY_RUN": "true",
            "PATH": f"{bin_dir}{os.pathsep}{os.environ.get('PATH', '')}",
        })
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("已存在，复用", result.stdout)
        self.assertIn("[dry-run] 创建或更新 GitHub Release", result.stdout)


if __name__ == "__main__":
    unittest.main()
