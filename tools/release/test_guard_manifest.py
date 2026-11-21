#!/usr/bin/env python3
"""guard.check_manifest_v2 单测：v2 清单字段、跨平台构建号同源与强制更新门禁。

纯离线：不请求网络、不校验签名（pub=None），只验证发布前的清单一致性规则。
"""

import importlib.util
import sys
import unittest
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location("guard", _HERE / "guard.py")
guard = importlib.util.module_from_spec(_spec)
sys.modules["guard"] = guard
_spec.loader.exec_module(guard)

PRODUCT = "xiaotu"
VERSION = "1.0.7"
BASE = "https://dl.xiaotu.online"
CHANNEL = "stable"
BUILD = 7


def _artifact(role: str) -> "guard.Artifact":
    return guard.Artifact(role=role, path=Path("/nonexistent"), name=role,
                          size=3, sha256="a" * 64)


def _url(version: str, ext: str) -> str:
    return f"{BASE}/{PRODUCT}/{PRODUCT}-{version}-{ext}"


def _manifest() -> dict:
    exts = {"dmg": "macos-arm64.dmg", "setup": "win64-setup.exe",
            "portable": "win64-portable.zip"}
    platforms = {}
    for name, roles in (("macos", ("dmg",)), ("windows", ("setup", "portable"))):
        platforms[name] = {
            "version": VERSION,
            "build": BUILD,
            "artifacts": {
                role: {"size": 3, "sha256": "a" * 64,
                       "url": _url(VERSION, exts[role])}
                for role in roles
            },
        }
    return {
        "manifest_version": 2,
        "product": PRODUCT,
        "channel": CHANNEL,
        "key_id": "test-key",
        "updated_at": "2026-01-01T00:00:00Z",
        "platforms": platforms,
    }


def _check(data: dict, kind: str = "mac", *, build: int = BUILD,
           artifacts=None, allow_build_drift: bool = False) -> None:
    guard.check_manifest_v2(
        data, PRODUCT, kind, artifacts or [_artifact("dmg")], VERSION, BASE,
        CHANNEL, build=build, pub=None, allow_build_drift=allow_build_drift)


class GuardManifestTest(unittest.TestCase):
    def _assert_die(self, data: dict, needle: str, **kwargs) -> None:
        with self.assertRaises(guard.GuardError) as ctx:
            _check(data, **kwargs)
        self.assertIn(needle, str(ctx.exception))

    def test_valid_manifest_passes(self):
        _check(_manifest())

    def test_manifest_version_must_be_2(self):
        data = _manifest()
        data["manifest_version"] = 1
        self._assert_die(data, "manifest_version=1")

    def test_product_and_channel_mismatch(self):
        data = _manifest()
        data["product"] = "other"
        self._assert_die(data, "清单 product=other")
        data = _manifest()
        data["channel"] = "beta"
        self._assert_die(data, "清单 channel=beta")

    def test_missing_platform_section(self):
        data = _manifest()
        del data["platforms"]["macos"]
        self._assert_die(data, "platforms 缺少")

    def test_version_mismatch(self):
        data = _manifest()
        data["platforms"]["macos"]["version"] = "1.0.6"
        self._assert_die(data, f"与产物 {VERSION} 不一致")

    def test_build_must_match_artifact(self):
        data = _manifest()
        data["platforms"]["macos"]["build"] = BUILD + 1
        self._assert_die(data, "与产物构建号 7 不一致")

    def test_same_version_cross_platform_build_must_match(self):
        data = _manifest()
        data["platforms"]["windows"]["build"] = BUILD + 1
        self._assert_die(data, "两平台 build 不同源")
        # 显式放行时允许单平台热修
        _check(data, allow_build_drift=True)

    def test_other_platform_zero_build_is_tolerated(self):
        data = _manifest()
        data["platforms"]["windows"]["build"] = 0
        _check(data)

    def test_artifact_entry_mismatch(self):
        data = _manifest()
        data["platforms"]["macos"]["artifacts"]["dmg"]["size"] = 99
        self._assert_die(data, "size=99")

        data = _manifest()
        data["platforms"]["macos"]["artifacts"]["dmg"]["sha256"] = "b" * 64
        self._assert_die(data, "sha256 与本地 SHA256 不一致")

        data = _manifest()
        data["platforms"]["macos"]["artifacts"]["dmg"]["url"] = _url(VERSION, "x.dmg")
        self._assert_die(data, "v2 命名不一致")

        data = _manifest()
        del data["platforms"]["macos"]["artifacts"]["dmg"]
        self._assert_die(data, "artifacts 为空")

    @unittest.skipUnless(PRODUCT == "devcard", "仅 DevCard 要求双平台同时存在")
    def test_devcard_requires_both_platforms(self):
        data = _manifest()
        del data["platforms"]["windows"]
        with self.assertRaises(guard.GuardError) as ctx:
            _check(data)
        self.assertIn("两平台必须同时存在", str(ctx.exception))


if __name__ == "__main__":
    unittest.main(verbosity=2)
