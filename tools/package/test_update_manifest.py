#!/usr/bin/env python3
"""update_manifest.py 的签名、规范化与篡改检测测试。"""

import base64
import json
import sys
import unittest
from pathlib import Path

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

sys.path.insert(0, str(Path(__file__).resolve().parent))
import update_manifest as um  # noqa: E402


def sample_body() -> dict:
    return {
        "manifest_version": 1,
        "key_id": "2026-01",
        "platform": "macos",
        "channel": "stable",
        "version": "1.4.2",
        "build": 2026111301,
        "min_os": "12.0",
        "url": "https://cdn.example.com/a.dmg",
        "sha256": "9f2c" * 16,
        "size": 1024,
        "published_at": "2026-11-13T08:00:00Z",
        "release_notes_url": "https://example.com/n",
        "mandatory": False,
        "mandatory_reason": None,
        "mandatory_deadline": None,
        "schema_min": 28,
        "schema_max": 32,
        "api_min": 1,
        "api_max": 2,
        "rollout_percent": 100,
    }


def sample_v2_body() -> dict:
    """v2 统一清单：平台字段挂在 platforms.<platform> 下。"""
    return {
        "manifest_version": 2,
        "product": "xiaotu",
        "channel": "stable",
        "key_id": "2026-11",
        "updated_at": "2026-11-18T04:48:54Z",
        "platforms": {
            "macos": {
                "version": "1.4.2",
                "build": 0,
                "min_os": "12.0",
                "artifacts": {
                    "dmg": {
                        "url": "https://dl.xiaotu.online/xiaotu/xiaotu-1.4.2-macos-arm64.dmg",
                        "size": 1024,
                        "sha256": "9f2c" * 16,
                    }
                },
            }
        },
    }


class ManifestTest(unittest.TestCase):
    def setUp(self) -> None:
        self.private = Ed25519PrivateKey.generate()
        self.public = self.private.public_key()
        self.body = um.sign_manifest(sample_body(), self.private)

    def test_canonical_is_key_order_independent(self) -> None:
        left = {"b": 1, "a": {"d": 2, "c": 3}}
        right = {"a": {"c": 3, "d": 2}, "b": 1}
        self.assertEqual(um.canonical_bytes(left), um.canonical_bytes(right))

    def test_signature_roundtrip(self) -> None:
        um.verify_manifest(self.body, self.public)
        self.assertEqual(len(base64.b64decode(self.body["signature"])), 64)

    def test_tampered_field_rejected(self) -> None:
        tampered = dict(self.body)
        tampered["build"] = 2026111302
        with self.assertRaises(Exception):
            um.verify_manifest(tampered, self.public)

    def test_wrong_key_rejected(self) -> None:
        other = Ed25519PrivateKey.generate().public_key()
        with self.assertRaises(Exception):
            um.verify_manifest(self.body, other)

    def test_missing_field_rejected(self) -> None:
        broken = dict(self.body)
        del broken["sha256"]
        with self.assertRaises(SystemExit):
            um.verify_manifest(broken, self.public)

    def test_appcast_signature_covers_file(self) -> None:
        payload = b"fake dmg bytes"
        xml = um.appcast_xml(self.body, payload, self.private)
        signature = xml.split('sparkle:edSignature="')[1].split('"')[0]
        self.public.verify(base64.b64decode(signature), payload)
        self.assertIn('sparkle:version="2026111301"', xml)
        self.assertIn("sparkle:shortVersionString=\"1.4.2\"", xml)

    def test_resign_only_changes_rollout(self) -> None:
        resigned = um.resign_manifest(self.body, self.private, rollout=10)
        um.verify_manifest(resigned, self.public)
        left = {k: v for k, v in self.body.items() if k not in {"signature", "rollout_percent"}}
        right = {k: v for k, v in resigned.items() if k not in {"signature", "rollout_percent"}}
        self.assertEqual(left, right)
        self.assertEqual(resigned["rollout_percent"], 10)

    def test_resign_idempotent(self) -> None:
        first = um.resign_manifest(self.body, self.private, rollout=10)
        second = um.resign_manifest(first, self.private, rollout=10)
        dump = lambda body: json.dumps(body, ensure_ascii=False, indent=2, sort_keys=True)
        self.assertEqual(dump(first), dump(second))

    def test_resign_invalid_rollout_rejected(self) -> None:
        for value in (0, 101, -1):
            with self.assertRaises(ValueError):
                um.resign_manifest(self.body, self.private, rollout=value)

    def test_resign_invalid_channel_rejected(self) -> None:
        with self.assertRaises(ValueError):
            um.resign_manifest(self.body, self.private, channel="nightly")

    def test_resign_requires_existing_signature(self) -> None:
        unsigned = dict(self.body)
        del unsigned["signature"]
        with self.assertRaises(ValueError):
            um.resign_manifest(unsigned, self.private, rollout=10)

    def test_tamper_after_resign_is_detected(self) -> None:
        # 重签会合法化输入内容，所以预期的失败点是“重签之后再篡改”；
        # 对已篡改内容重签属于有意的重新发布，工具无法也不应阻止。
        resigned = um.resign_manifest(self.body, self.private, rollout=10)
        resigned["url"] = "https://evil.example.com/a.dmg"
        with self.assertRaises(Exception):
            um.verify_manifest(resigned, self.public)

    def test_v2_signature_roundtrip(self) -> None:
        body = um.sign_manifest(sample_v2_body(), self.private)
        um.verify_manifest(body, self.public)
        body["platforms"]["macos"]["version"] = "9.9.9"
        with self.assertRaises(Exception):
            um.verify_manifest(body, self.public)

    def test_v2_missing_top_field_rejected(self) -> None:
        body = um.sign_manifest(sample_v2_body(), self.private)
        del body["product"]
        with self.assertRaises(SystemExit):
            um.verify_manifest(body, self.public)

    def test_public_key_roundtrip(self) -> None:
        raw = self.public.public_bytes(
            serialization.Encoding.PEM,
            serialization.PublicFormat.SubjectPublicKeyInfo,
        )
        tmp = Path(self._testMethodName + ".pub.pem")
        tmp.write_bytes(raw)
        try:
            loaded = um.load_public_key(tmp)
            um.verify_manifest(self.body, loaded)
        finally:
            tmp.unlink(missing_ok=True)


if __name__ == "__main__":
    unittest.main()
