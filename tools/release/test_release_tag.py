#!/usr/bin/env python3
"""release_tag.py 的单元测试：tag 解析、版本一致性、序号递增与不可重打。"""
from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "release" / "release_tag.py"


class ParseTest(unittest.TestCase):
    def run_parse(self, tag: str, version: str = "1.3.2", base: str | None = None):
        cmd = [sys.executable, str(SCRIPT), "parse", "--tag", tag, "--version", version]
        if base:
            cmd += ["--base", base]
        return subprocess.run(cmd, capture_output=True, text=True, check=False)

    def test_build_is_base_plus_seq(self) -> None:
        result = self.run_parse("v1.3.2+240")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("build=240", result.stdout)

    def test_sh_output(self) -> None:
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "parse", "--tag", "v1.3.2+7",
             "--version", "1.3.2", "--emit", "sh"],
            capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("VERSION=1.3.2", result.stdout)
        self.assertIn("RELEASE_SEQ=7", result.stdout)
        self.assertIn("BUILD=7", result.stdout)

    def test_version_mismatch_rejected(self) -> None:
        self.assertEqual(self.run_parse("v1.3.2+10", version="1.3.1").returncode, 2)

    def test_bad_tag_rejected(self) -> None:
        for tag in ("release-240", "v1.3.2", "v1.3.2+0", "v1.3.2+abc"):
            self.assertEqual(self.run_parse(tag).returncode, 2, tag)


class CreateTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self.tmp.name)
        self.git("init", "-q")
        (self.repo / "VERSION.txt").write_text("1.0.1\n", encoding="utf-8")
        self.git("add", ".")
        self.git("-c", "user.email=t@t", "-c", "user.name=t", "commit", "-q", "-m", "init")
        self.git("tag", "-a", "v1.0.0+7", "-m", "old")

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def git(self, *args: str) -> str:
        result = subprocess.run(["git", "-C", str(self.repo), *args],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout.strip()

    def create(self, *extra: str):
        return subprocess.run(
            [sys.executable, str(SCRIPT), "create", "--repo-root", str(self.repo), *extra],
            capture_output=True, text=True, check=False)

    def test_next_seq_from_existing_tags(self) -> None:
        result = self.create()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("v1.0.1+8", self.git("tag", "-l", "v1.0.1+8"))

    def test_explicit_lower_seq_rejected(self) -> None:
        self.assertEqual(self.create("--seq", "7").returncode, 2)

    def test_existing_tag_not_reused(self) -> None:
        self.assertEqual(self.create("--seq", "8").returncode, 0)
        self.assertEqual(self.create("--seq", "8").returncode, 2)

    def test_dry_run_creates_nothing(self) -> None:
        result = self.create("--dry-run")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("v1.0.1+8", self.git("tag", "-l"))

    def test_version_file_mismatch_ignored_with_explicit_version(self) -> None:
        result = self.create("--version", "1.0.2")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("v1.0.2+8", self.git("tag", "-l", "v1.0.2+8"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
