#!/usr/bin/env python3
"""guard._extract_build_from_bytes 构建号提取回归测试（三仓同一份模板）。

覆盖：
- ASCII 标记 <PREFIX>_BUILD=<digits> 能提取数字；
- UTF-16LE 编码的 <PREFIX>_BUILD=<digits> 能提取
  （回归：此前实现要求至少 4 位，tag 序号 1~3 位提取失败）；
- 1~12 位数字在两种编码下都能完整提取；
- 传入其它产品名返回 None；
- 只有前缀没有数字返回 None；
- 无关二进制返回 None。

运行：python3 tools/release/test_guard.py -v
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
PRODUCT = "xiaotu"
PREFIX = "XIAOTU"
OTHER_PRODUCT = "mote"

sys.path.insert(0, str(REPO / "tools" / "release"))
import guard  # noqa: E402

MARKER = f"{PREFIX}_BUILD="


class ExtractBuildFromBytesTest(unittest.TestCase):
    def test_ascii_single_digit(self) -> None:
        data = b"\x00\x00" + MARKER.encode("ascii") + b"7" + b"\x00\x00"
        self.assertEqual(guard._extract_build_from_bytes(data, PRODUCT), "7")

    def test_ascii_two_digits(self) -> None:
        data = b"\x7fELF" + MARKER.encode("ascii") + b"12"
        self.assertEqual(guard._extract_build_from_bytes(data, PRODUCT), "12")

    def test_ascii_full_timestamp(self) -> None:
        data = b"prefix" + MARKER.encode("ascii") + b"2026111901" + b"suffix"
        self.assertEqual(guard._extract_build_from_bytes(data, PRODUCT), "2026111901")

    def test_utf16le_single_digit(self) -> None:
        data = b"\xff\xfe" + (MARKER + "7").encode("utf-16-le")
        self.assertEqual(guard._extract_build_from_bytes(data, PRODUCT), "7")

    def test_utf16le_two_digits(self) -> None:
        data = b"\xff\xfe" + (MARKER + "12").encode("utf-16-le")
        self.assertEqual(guard._extract_build_from_bytes(data, PRODUCT), "12")

    def test_utf16le_three_digits(self) -> None:
        data = b"\xff\xfe" + (MARKER + "123").encode("utf-16-le")
        self.assertEqual(guard._extract_build_from_bytes(data, PRODUCT), "123")

    def test_digit_lengths_one_to_twelve(self) -> None:
        digits_source = "123456789012"
        for length in range(1, 13):
            digits = digits_source[:length]
            with self.subTest(length=length, encoding="ascii"):
                data = b"\x00" + (MARKER + digits).encode("ascii") + b"\x00"
                self.assertEqual(guard._extract_build_from_bytes(data, PRODUCT), digits)
            with self.subTest(length=length, encoding="utf-16-le"):
                data = b"\xff\xfe" + (MARKER + digits).encode("utf-16-le")
                self.assertEqual(guard._extract_build_from_bytes(data, PRODUCT), digits)

    def test_wrong_product_returns_none(self) -> None:
        data = b"\x00" + MARKER.encode("ascii") + b"7\x00"
        self.assertIsNone(guard._extract_build_from_bytes(data, OTHER_PRODUCT))

    def test_prefix_without_digits_returns_none(self) -> None:
        data = b"\x00" + MARKER.encode("ascii") + b"\x00tail"
        self.assertIsNone(guard._extract_build_from_bytes(data, PRODUCT))

    def test_unrelated_binary_returns_none(self) -> None:
        data = b"\x00\x01\x02 unrelated binary payload \x03\x04"
        self.assertIsNone(guard._extract_build_from_bytes(data, PRODUCT))


if __name__ == "__main__":
    unittest.main()
