#!/usr/bin/env python3
"""check_version_bump.py 的单元测试（离线，用 --manifest-file 注入线上清单）。"""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "release" / "check_version_bump.py"


class VersionBumpTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.version_file = self.dir / "VERSION.txt"
        self.version_file.write_text("1.2.3\n", encoding="utf-8")

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def manifest(self, mac_version: str, mac_build: int,
                 win_version: str, win_build: int) -> Path:
        path = self.dir / "manifest.json"
        path.write_text(json.dumps({
            "manifest_version": 2,
            "product": "demo",
            "channel": "stable",
            "platforms": {
                "macos": {"version": mac_version, "build": mac_build},
                "windows": {"version": win_version, "build": win_build},
            },
        }), encoding="utf-8")
        return path

    def run_check(self, build: str, *extra: str):
        return subprocess.run(
            [sys.executable, str(SCRIPT), "--version-file", str(self.version_file),
             "--build", build, *extra],
            capture_output=True, text=True, check=False,
        )

    def test_new_version_passes(self) -> None:
        manifest = self.manifest("1.2.3", 100, "1.2.3", 100)
        self.version_file.write_text("1.3.0", encoding="utf-8")
        self.assertEqual(self.run_check("50", "--manifest-file", str(manifest)).returncode, 0)

    def test_same_version_with_newer_build_passes(self) -> None:
        manifest = self.manifest("1.2.3", 100, "1.2.3", 120)
        result = self.run_check("121", "--manifest-file", str(manifest))
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_same_version_with_same_build_fails(self) -> None:
        manifest = self.manifest("1.2.3", 100, "1.2.3", 120)
        result = self.run_check("120", "--manifest-file", str(manifest))
        self.assertEqual(result.returncode, 2)

    def test_same_version_with_older_build_fails(self) -> None:
        manifest = self.manifest("1.2.3", 100, "1.2.3", 120)
        self.assertEqual(self.run_check("119", "--manifest-file", str(manifest)).returncode, 2)

    def test_version_rollback_fails(self) -> None:
        manifest = self.manifest("1.2.3", 100, "1.2.3", 100)
        self.version_file.write_text("1.2.2", encoding="utf-8")
        self.assertEqual(self.run_check("999", "--manifest-file", str(manifest)).returncode, 2)

    def test_build_must_beat_all_platforms(self) -> None:
        manifest = self.manifest("1.2.3", 200, "1.2.3", 100)
        self.assertEqual(self.run_check("150", "--manifest-file", str(manifest)).returncode, 2)
        self.assertEqual(self.run_check("201", "--manifest-file", str(manifest)).returncode, 0)

    def test_missing_manifest_fails_by_default(self) -> None:
        result = self.run_check("999", "--manifest-file", str(self.dir / "nope.json"))
        self.assertEqual(result.returncode, 2)

    def test_missing_manifest_allowed_for_first_release(self) -> None:
        result = self.run_check("999", "--manifest-file", str(self.dir / "nope.json"),
                                "--allow-missing")
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_invalid_build_rejected(self) -> None:
        manifest = self.manifest("1.2.3", 100, "1.2.3", 100)
        self.assertEqual(self.run_check("2026-11", "--manifest-file", str(manifest)).returncode, 2)

    def test_invalid_version_rejected(self) -> None:
        manifest = self.manifest("1.2.3", 100, "1.2.3", 100)
        self.version_file.write_text("v1.2.3", encoding="utf-8")
        self.assertEqual(self.run_check("999", "--manifest-file", str(manifest)).returncode, 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
