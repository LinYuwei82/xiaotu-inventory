"""发布脚本 shell 层测试。

覆盖：publish_oss.sh / publish_windows.sh / make_dmg.sh / cleanup_oss_versions.sh。
全部用 OSS_SKIP=true（dry-run）或 NOTES_ONLY=true，不碰真实 OSS 凭据、签名、公证、oss2 venv。
拒绝路径传坏参数，期望脚本以非 0 退出码拒绝并 stderr 含关键字。
合法路径传正确参数（用最小 mock 产物），期望脚本走完 dry-run 流程。

用法：
  tools/package/.venv/bin/python -m unittest tools/package/test_release_scripts.py -v
  # 或直接执行
  tools/package/.venv/bin/python tools/package/test_release_scripts.py
"""

from __future__ import annotations

import base64
import hashlib
import os
import subprocess
import sys
import tempfile
import unittest
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
PKG = REPO / "tools" / "package"
# 小兔 Ed25519 测试种子（RFC 8032 固定向量）
TEST_SEED_HEX = "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"
TEST_SEED = bytes.fromhex(TEST_SEED_HEX)
VERSION = (REPO / "VERSION.txt").read_text().strip()


# --------------------------------------------------------------------------- 辅助工具

def _generate_key_pair() -> tuple[Path, Path]:
    """用 venv 里的 cryptography 生成临时 Ed25519 PKCS#8 / SPKI PEM。"""
    priv = Path(tempfile.mkstemp(suffix=".pem")[1])
    pub = Path(tempfile.mkstemp(suffix=".pub.pem")[1])
    cmd = [
        str(REPO / "tools" / "package" / ".venv" / "bin" / "python"), "-c",
        f"""
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
import sys
seed = bytes.fromhex("{TEST_SEED_HEX}")
pk = Ed25519PrivateKey.from_private_bytes(seed)
with open(sys.argv[1], "wb") as f:
    f.write(pk.private_bytes(serialization.Encoding.PEM,
                             serialization.PrivateFormat.PKCS8,
                             serialization.NoEncryption()))
with open(sys.argv[2], "wb") as f:
    f.write(pk.public_key().public_bytes(
        serialization.Encoding.PEM,
        serialization.PublicFormat.SubjectPublicKeyInfo))
""",
        str(priv), str(pub),
    ]
    subprocess.run(cmd, check=True, capture_output=True)
    return priv, pub


def _has_network(url: str, timeout: float = 3.0) -> bool:
    try:
        urllib.request.urlopen(url, timeout=timeout).read(1)
        return True
    except Exception:
        return False


def _find_openssl() -> str:
    for cand in ("/opt/homebrew/opt/openssl@3/bin/openssl",
                 "/usr/local/opt/openssl@3/bin/openssl"):
        if os.path.exists(cand):
            return cand
    return ""


class Runner:
    def __init__(self):
        self.env = {**os.environ, "OSS_SKIP": "true", "HOME": tempfile.mkdtemp(prefix="home-")}
        self._setup_signing_key()
        self.ossl = _find_openssl()
        if self.ossl:
            self.env["OPENSSL_BIN"] = self.ossl

    def _setup_signing_key(self) -> None:
        cfg = Path(self.env["HOME"]) / ".config" / "xiaotu-updates"
        cfg.mkdir(parents=True)
        priv, pub = _generate_key_pair()
        (cfg / "update_signing_2026.pem").write_bytes(priv.read_bytes())
        (cfg / "update_signing_2026.pub.pem").write_bytes(pub.read_bytes())
        (cfg / "ed25519_seed.b64").write_text(base64.b64encode(TEST_SEED).decode())

    def run(self, script: str, args: list[str] | None = None, *,
            expect_code: int = 0, expect_stderr_contains: str | None = None,
            env_override: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
        cmd = [script] + (args or [])
        env = {**self.env, **(env_override or {})}
        result = subprocess.run(
            cmd, capture_output=True, text=True, env=env,
            cwd=str(REPO), timeout=60,
        )
        got = result.returncode
        if got != expect_code:
            detail = f"exit {got}, want {expect_code}\n"
            if result.stderr:
                detail += f"  stderr: {result.stderr[-500:]}\n"
            if result.stdout:
                detail += f"  stdout: {result.stdout[-300:]}"
            raise AssertionError(detail)
        if expect_stderr_contains and expect_stderr_contains not in result.stderr:
            raise AssertionError(f"stderr 不含 '{expect_stderr_contains}': {result.stderr[-400:]}")
        return result


# --------------------------------------------------------------------------- 测试类

class TestPublishOss(unittest.TestCase):
    """publish_oss.sh：小兔 macOS 发布脚本的拒绝路径 + NOTES_ONLY 行为。"""

    @classmethod
    def setUpClass(cls):
        cls.r = Runner()
        cls.script = str(PKG / "publish_oss.sh")

    def test_help_zero_exit(self):
        self.r.run(self.script, ["--help"], expect_code=0)

    def test_unknown_flag_rejected(self):
        self.r.run(self.script, ["--unknown-flag"], expect_code=2,
                   expect_stderr_contains="未知参数")

    def test_product_must_be_xiaotu(self):
        """PRODUCT 只能是 xiaotu，devcard/mote 已迁出本仓库。"""
        self.r.run(self.script, env_override={"PRODUCT": "devcard"},
                   expect_code=1, expect_stderr_contains="devcard")
        self.r.run(self.script, env_override={"PRODUCT": "mote"},
                   expect_code=1, expect_stderr_contains="mote")
        self.r.run(self.script, env_override={"PRODUCT": "foo"},
                   expect_code=1, expect_stderr_contains="必须是 xiaotu")

    def test_channel_must_be_stable_or_beta(self):
        self.r.run(self.script, env_override={"CHANNEL": "nightly"},
                   expect_code=1, expect_stderr_contains="必须是 stable/beta")

    def test_release_notes_url_must_be_absolute_http_or_https(self):
        """相对路径会变成死链。"""
        if not self.r.ossl:
            self.skipTest("需要 OpenSSL 3.x")
        fake_dmg = self._make_fake_dmg()
        self.r.run(self.script, env_override={
            "RELEASE_NOTES_URL": "/xiaotu/release-notes.html",
            "DMG_PATH": str(fake_dmg),
        }, expect_code=2, expect_stderr_contains="绝对 http(s)")

    def test_ssl_certificate_domain_prefix(self):
        """CDN_DOMAIN（dry-run 不验证）——非 dry-run 会校验必须 http(s):// 前缀。"""
        # dry-run 跳过 CDN_DOMAIN 前缀校验，所以这里只能测 NOTES_ONLY 分支
        # NOTES_ONLY + RELEASE_NOTES_URL 非绝对地址 → 拒绝
        if _has_network("https://dl.xiaotu.online/xiaotu/updates/stable/manifest.json"):
            self.r.run(self.script, env_override={
                "NOTES_ONLY": "true",
                "RELEASE_NOTES_URL": "relative/path.html",
            }, expect_code=2, expect_stderr_contains="绝对 http(s)")
        else:
            self.skipTest("需要网络访问 dl.xiaotu.online")

    def test_notes_only_requires_release_notes_file(self):
        """NOTES_ONLY 必须提供存在的说明页文件。"""
        if not _has_network("https://dl.xiaotu.online/xiaotu/updates/stable/manifest.json"):
            self.skipTest("需要网络访问 dl.xiaotu.online")
        missing = str(Path(tempfile.mkdtemp()) / "does-not-exist.html")
        self.r.run(self.script, env_override={
            "NOTES_ONLY": "true",
            "OSS_SKIP": "true",
            "RELEASE_NOTES_FILE": missing,
        }, expect_code=1, expect_stderr_contains="发行说明不存在")

    # ---- helpers ----

    def _make_fake_dmg(self) -> Path:
        """伪造带 build 号文件名的 dmg，让脚本走文件名推断 build 的分支。"""
        fake = Path(tempfile.mkstemp(suffix=".dmg")[1])
        fake.write_bytes(b"fake-dmg-bytes")
        target = fake.with_name(f"xiaotu-inventory-{VERSION}-2026111901.dmg")
        fake.rename(target)
        return target


class TestPublishWindows(unittest.TestCase):
    """publish_windows.sh：小兔 Windows 发布脚本的拒绝路径。"""

    @classmethod
    def setUpClass(cls):
        cls.r = Runner()
        cls.script = str(PKG / "publish_windows.sh")

    def test_help_zero_exit(self):
        self.r.run(self.script, ["--help"], expect_code=0)

    def test_unknown_flag_rejected(self):
        self.r.run(self.script, ["--unknown"], expect_code=2,
                   expect_stderr_contains="未知参数")

    def test_build_required_and_numeric(self):
        """BUILD 可省略（自动 gen_build）；显式传入时必须纯数字、≥4 位。"""
        self.r.run(self.script, env_override={"BUILD": "12"},
                   expect_code=1, expect_stderr_contains="BUILD")  # 长度不够
        self.r.run(self.script, env_override={"BUILD": "20261119xx"},
                   expect_code=1, expect_stderr_contains="BUILD")  # 有字母
        self.r.run(self.script, env_override={"BUILD": "abcdefgh"},
                   expect_code=1, expect_stderr_contains="BUILD")

    def test_build_allows_valid_numeric(self):
        """BUILD 合法纯数字时通过校验（后续校验 setup.exe 文件名）。"""
        setup, _ = self._make_fake_artifacts()
        self.r.run(self.script, env_override={
            "BUILD": "2026111901",
            "SETUP_PATH": str(setup),
            "OSS_SKIP": "true",
        }, expect_code=0)

    def test_setup_exe_filename_must_match_convention(self):
        """必须命名为 xiaotu-inventory-<v>-win64-setup.exe。"""
        wrong = Path(tempfile.mkstemp(suffix=".exe")[1])
        wrong.write_bytes(b"MZ-fake")
        self.r.run(self.script, env_override={
            "BUILD": "2026111901",
            "SETUP_PATH": str(wrong),
            "OSS_SKIP": "true",
        }, expect_code=1, expect_stderr_contains="必须命名为")

    def test_notes_only_skips_build_and_artifact(self):
        """NOTES_ONLY 模式不需要 BUILD 和安装包（需要拉线上清单）。"""
        if not _has_network("https://dl.xiaotu.online/xiaotu/updates/stable/manifest.json"):
            self.skipTest("需要网络访问 dl.xiaotu.online")
        release_notes = REPO / "docs" / f"release-notes-{VERSION}.html"
        if not release_notes.exists():
            self.skipTest(f"docs/release-notes-{VERSION}.html 不存在")
        self.r.run(self.script, env_override={
            "NOTES_ONLY": "true",
            "BUILD": "",  # 空 BUILD 也应过
            "OSS_SKIP": "true",
            "RELEASE_NOTES_FILE": str(release_notes),
        }, expect_code=0)

    def test_notes_only_rejects_missing_release_notes(self):
        if not _has_network("https://dl.xiaotu.online/xiaotu/updates/stable/manifest.json"):
            self.skipTest("需要网络访问 dl.xiaotu.online")
        missing = str(Path(tempfile.mkdtemp()) / "does-not-exist.html")
        self.r.run(self.script, env_override={
            "NOTES_ONLY": "true",
            "OSS_SKIP": "true",
            "RELEASE_NOTES_FILE": missing,
        }, expect_code=1, expect_stderr_contains="发行说明不存在")

    # ---- helpers ----

    def _make_fake_artifacts(self) -> tuple[Path, Path]:
        out = Path(tempfile.mkdtemp())
        setup = out / f"xiaotu-inventory-{VERSION}-win64-setup.exe"
        zipp = out / f"xiaotu-inventory-{VERSION}-win64.zip"
        setup.write_bytes(b"MZ-fake")
        zipp.write_bytes(b"PK\x03\x04")
        return setup, zipp


class TestMakeDmg(unittest.TestCase):
    """make_dmg.sh：小兔 dmg 打包脚本的参数校验。"""

    @classmethod
    def setUpClass(cls):
        cls.r = Runner()
        cls.script = str(PKG / "make_dmg.sh")

    def _valid_pubkey_b64(self) -> str:
        pub = subprocess.run(
            [str(REPO / "tools" / "package" / ".venv" / "bin" / "python"), "-c",
             f"import base64; print(base64.b64encode(bytes.fromhex('{TEST_SEED_HEX}')).decode())"],
            capture_output=True, text=True, check=True,
        ).stdout.strip()
        return pub

    def test_help_zero_exit(self):
        self.r.run(self.script, ["--help"], expect_code=0)

    def test_build_optional_but_numeric(self):
        """BUILD 可省略（自动 gen_build）；显式传入时必须纯数字。"""
        self.r.run(self.script, env_override={"XIAOTU_BUILD": "abc"},
                   expect_code=1, expect_stderr_contains="必须是纯数字")

    def test_update_triple_must_be_all_or_none(self):
        """更新三项（MANIFEST_URL / FEED_URL / PUBKEY）不能只给部分。"""
        pub = self._valid_pubkey_b64()
        # 只给 MANIFEST_URL
        self.r.run(self.script, env_override={
            "XIAOTU_BUILD": "2026111901",
            "XIAOTU_UPDATE_MANIFEST_URL": "https://a",
        }, expect_code=1, expect_stderr_contains="必须同时提供")
        # 给两项
        self.r.run(self.script, env_override={
            "XIAOTU_BUILD": "2026111901",
            "XIAOTU_UPDATE_MANIFEST_URL": "https://a",
            "XIAOTU_UPDATE_FEED_URL": "https://b",
        }, expect_code=1, expect_stderr_contains="必须同时提供")

    def test_manifest_url_must_be_https(self):
        pub = self._valid_pubkey_b64()
        self.r.run(self.script, env_override={
            "XIAOTU_BUILD": "2026111901",
            "XIAOTU_UPDATE_MANIFEST_URL": "http://a",
            "XIAOTU_UPDATE_FEED_URL": "https://b",
            "XIAOTU_UPDATE_PUBKEY": pub,
        }, expect_code=1, expect_stderr_contains="必须是 https")

    def test_feed_url_must_be_https(self):
        pub = self._valid_pubkey_b64()
        self.r.run(self.script, env_override={
            "XIAOTU_BUILD": "2026111901",
            "XIAOTU_UPDATE_MANIFEST_URL": "https://a",
            "XIAOTU_UPDATE_FEED_URL": "http://b",
            "XIAOTU_UPDATE_PUBKEY": pub,
        }, expect_code=1, expect_stderr_contains="必须是 https")

    def test_pubkey_must_be_base64_32_bytes(self):
        self.r.run(self.script, env_override={
            "XIAOTU_BUILD": "2026111901",
            "XIAOTU_UPDATE_MANIFEST_URL": "https://a",
            "XIAOTU_UPDATE_FEED_URL": "https://b",
            "XIAOTU_UPDATE_PUBKEY": "!!!not-base64-wrong-length!!!",
        }, expect_code=1, expect_stderr_contains="不是合法的 Ed25519")

    def test_sparkle_dir_must_contain_framework(self):
        self.r.run(self.script, env_override={
            "XIAOTU_BUILD": "2026111901",
            "XIAOTU_SPARKLE_DIR": "/nonexistent/path",
        }, expect_code=1, expect_stderr_contains="Sparkle.framework")


class TestCleanupOssParams(unittest.TestCase):
    """cleanup_oss_versions.sh：清理脚本的参数校验（纯本地不碰 OSS）。"""

    @classmethod
    def setUpClass(cls):
        cls.r = Runner()
        cls.script = str(PKG / "cleanup_oss_versions.sh")

    def test_keep_must_be_positive_int(self):
        self.r.run(self.script, ["--keep", "0", "--dry-run"],
                   expect_code=1, expect_stderr_contains="≥ 1")
        self.r.run(self.script, ["--keep", "-1", "--dry-run"],
                   expect_code=1, expect_stderr_contains="≥ 1")
        self.r.run(self.script, ["--keep", "abc", "--dry-run"],
                   expect_code=1, expect_stderr_contains="≥ 1")

    def test_non_interactive_requires_force_or_dryrun(self):
        """CI / 管道环境必须显式 --force 或 --dry-run，防止静默误删。"""
        self.r.run(self.script, expect_code=1,
                   expect_stderr_contains="非交互环境")


# --------------------------------------------------------------------------- main

def main() -> int:
    # 先跑共享脚本的 selftest（Python 标准库，纯离线）
    print("\n=== manifest_v2 selftest ===")
    r1 = subprocess.run(
        ["python3", str(REPO / "tools" / "release" / "manifest_v2.py"), "selftest"],
        check=False,
    )
    print(f"\n=== guard selftest ===")
    r2 = subprocess.run(
        ["python3", str(REPO / "tools" / "release" / "guard.py"), "selftest"],
        check=False,
    )
    print(f"\n=== update_manifest (v1) unittest ===")
    r3 = subprocess.run(
        [str(REPO / "tools" / "package" / ".venv" / "bin" / "python"),
         "-m", "unittest", "tools.package.test_update_manifest", "-v"],
        check=False, cwd=str(REPO),
    )
    print(f"\n=== release_scripts unittest ===")
    module_name = "tools.package.test_release_scripts"
    r4 = subprocess.run(
        [str(REPO / "tools" / "package" / ".venv" / "bin" / "python"),
         "-m", "unittest", module_name, "-v"],
        check=False, cwd=str(REPO),
    )
    ok = all([r1.returncode == 0, r2.returncode == 0, r3.returncode == 0, r4.returncode == 0])
    codes = f"manifest_v2={r1.returncode}, guard={r2.returncode}, update_manifest={r3.returncode}, shell={r4.returncode}"
    print(f"\n=== 汇总 exit codes: {codes} ===")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
