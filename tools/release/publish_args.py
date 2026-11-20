#!/usr/bin/env python3
"""发布参数校验（灰度 / 强制更新）—— mote / 小兔 / DevCard 三仓库共享的唯一实现。

由 mote/tools/release/sync_guard.sh 同步到另外两个仓库（不要在各仓库单独改）。

用法（shell 脚本里 eval 出变量）：
  eval "$(python3 tools/release/publish_args.py validate \\
            --product mote \\
            --rollout "${ROLLOUT_PERCENT:-100}" \\
            --mandatory "${MANDATORY:-0}" \\
            --mandatory-reason "${MANDATORY_REASON:-}" \\
            --mandatory-deadline "${MANDATORY_DEADLINE:-}" \\
            --emit sh)"

可用变量（--emit sh 时输出）：
  ROLLOUT_PERCENT   规范化后的灰度百分比（1–100）
  MANDATORY_ON      0 / 1
  MANDATORY_REASON  强制更新的原因（SECURITY/PROTOCOL/REGULATORY/MIN_SUPPORTED）
  MANDATORY_DEADLINE  期限（ISO 8601 UTC，可为空）

退出码：0 通过；2 参数非法（中文错误写 stderr）。
"""

from __future__ import annotations

import argparse
import re
import sys

REASONS = ("SECURITY", "PROTOCOL", "REGULATORY", "MIN_SUPPORTED")
DEADLINE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$")
ROLLOUT_RE = re.compile(r"^[0-9]+$")
TRUE_VALUES = {"1", "true"}
FALSE_VALUES = {"", "0", "false"}
EMIT_KEYS = ("ROLLOUT_PERCENT", "MANDATORY_ON", "MANDATORY_REASON", "MANDATORY_DEADLINE")


class ValidationError(Exception):
    """参数校验失败（对应 exit 2）。"""


def _fail(product: str, message: str) -> None:
    prefix = f"[{product}] " if product else ""
    raise ValidationError(prefix + message)


def parse_mandatory(product: str, raw: str) -> bool:
    """0/1/true/false/空 → bool；其它值直接拒绝。"""
    value = (raw or "").strip().lower()
    if value in TRUE_VALUES:
        return True
    if value in FALSE_VALUES:
        return False
    _fail(product, f"MANDATORY 只能是 0/1/true/false（或留空）：{raw}")
    return False  # 不可达，仅为类型检查


def validate(product: str = "", rollout: str = "100", mandatory: str = "",
             reason: str = "", deadline: str = "",
             default_reason: str = "", default_deadline: str = "") -> dict[str, str]:
    """校验并规范化全部参数，返回 {变量名: 值}。失败抛 ValidationError。"""
    rollout_raw = str(rollout if rollout is not None else "").strip()
    if not ROLLOUT_RE.match(rollout_raw) or not 1 <= int(rollout_raw) <= 100:
        _fail(product, f"ROLLOUT_PERCENT 必须是 1–100 的整数：{rollout_raw}")
    rollout_value = int(rollout_raw)

    mandatory_on = parse_mandatory(product, mandatory)
    reason_value = (reason or "").strip()
    deadline_value = (deadline or "").strip()

    if mandatory_on:
        # 历史兼容：只传 MANDATORY=true 时用调用方给的默认原因 / 期限补齐
        if not reason_value:
            reason_value = (default_reason or "").strip()
        if not reason_value:
            _fail(product,
                  "MANDATORY=1 时必须指定 MANDATORY_REASON"
                  "（SECURITY/PROTOCOL/REGULATORY/MIN_SUPPORTED）")
        if reason_value not in REASONS:
            _fail(product,
                  "MANDATORY_REASON 必须是 SECURITY/PROTOCOL/REGULATORY/"
                  f"MIN_SUPPORTED 之一：{reason_value}")
        if not deadline_value:
            deadline_value = (default_deadline or "").strip()
        if deadline_value and not DEADLINE_RE.match(deadline_value):
            _fail(product,
                  "MANDATORY_DEADLINE 必须是 ISO 8601 UTC"
                  f"（如 2026-12-01T00:00:00Z）：{deadline_value}")
    elif reason_value or deadline_value:
        _fail(product,
              "MANDATORY_REASON / MANDATORY_DEADLINE 只在 MANDATORY=1 时有效")

    return {
        "ROLLOUT_PERCENT": str(rollout_value),
        "MANDATORY_ON": "1" if mandatory_on else "0",
        "MANDATORY_REASON": reason_value,
        "MANDATORY_DEADLINE": deadline_value,
    }


def sh_quote(value: str) -> str:
    """单引号安全转义，输出可直接被 sh 的 eval 使用。"""
    return "'" + str(value).replace("'", "'\\''") + "'"


def emit_sh(values: dict[str, str]) -> str:
    return "".join(f"{key}={sh_quote(values[key])}\n" for key in EMIT_KEYS)


# --------------------------------------------------------------------------- selftest

def _selftest() -> int:
    cases: list[tuple[str, dict, bool, str]] = [
        # (用例说明, validate 关键字参数, 是否合法, 期望错误子串)
        ("默认值：全量 + 非强制", {}, True, ""),
        ("灰度 10%", {"rollout": "10"}, True, ""),
        ("灰度 1%", {"rollout": "1"}, True, ""),
        ("灰度 100%", {"rollout": "100"}, True, ""),
        ("强制 1 + SECURITY", {"mandatory": "1", "reason": "SECURITY"}, True, ""),
        ("强制 true + PROTOCOL + 期限",
         {"mandatory": "true", "reason": "PROTOCOL",
          "deadline": "2026-12-01T00:00:00Z"}, True, ""),
        ("强制 false（等同关闭）", {"mandatory": "false"}, True, ""),
        ("强制 0（等同关闭）", {"mandatory": "0"}, True, ""),
        ("灰度越界 0", {"rollout": "0"}, False, "ROLLOUT_PERCENT"),
        ("灰度越界 101", {"rollout": "101"}, False, "ROLLOUT_PERCENT"),
        ("灰度非数字 abc", {"rollout": "abc"}, False, "ROLLOUT_PERCENT"),
        ("灰度小数 10.5", {"rollout": "10.5"}, False, "ROLLOUT_PERCENT"),
        ("灰度空值", {"rollout": ""}, False, "ROLLOUT_PERCENT"),
        ("mandatory 非法值 2", {"mandatory": "2"}, False, "MANDATORY"),
        ("强制缺原因", {"mandatory": "1"}, False, "MANDATORY_REASON"),
        ("强制原因非法", {"mandatory": "1", "reason": "BECAUSE"}, False, "MANDATORY_REASON"),
        ("期限格式非法", {"mandatory": "1", "reason": "SECURITY",
                          "deadline": "2026-12-01"}, False, "ISO 8601 UTC"),
        ("期限非 UTC（带偏移）", {"mandatory": "1", "reason": "SECURITY",
                                  "deadline": "2026-12-01T00:00:00+08:00"}, False, "ISO 8601 UTC"),
        ("非强制却带原因", {"reason": "SECURITY"}, False, "只在 MANDATORY=1"),
        ("非强制却带期限", {"deadline": "2026-12-01T00:00:00Z"}, False, "只在 MANDATORY=1"),
        ("default-reason 补齐", {"mandatory": "true", "default_reason": "MIN_SUPPORTED"}, True, ""),
        ("default-reason + default-deadline 补齐",
         {"mandatory": "true", "default_reason": "MIN_SUPPORTED",
          "default_deadline": "2026-11-20T10:00:00Z"}, True, ""),
        ("显式原因优先于 default-reason",
         {"mandatory": "1", "reason": "SECURITY", "default_reason": "MIN_SUPPORTED"}, True, ""),
        ("default-reason 本身非法",
         {"mandatory": "1", "default_reason": "BECAUSE"}, False, "MANDATORY_REASON"),
        ("default-deadline 本身非法",
         {"mandatory": "1", "reason": "SECURITY", "default_deadline": "tomorrow"}, False,
         "ISO 8601 UTC"),
    ]

    failures = 0
    for desc, kwargs, ok, substr in cases:
        try:
            values = validate(**kwargs)
            got_ok, err = True, ""
        except ValidationError as exc:
            values, got_ok, err = {}, False, str(exc)

        problem = ""
        if got_ok != ok:
            problem = f"期望{'通过' if ok else '拒绝'}，实际{'通过' if got_ok else '拒绝'}"
        elif not ok and substr not in err:
            problem = f"错误信息缺少 '{substr}'：{err}"
        elif ok:
            produced = {key: values[key] for key in EMIT_KEYS}
            if set(produced) != set(EMIT_KEYS):
                problem = f"emit 变量不全：{produced}"
            elif values["ROLLOUT_PERCENT"] != str(int(values["ROLLOUT_PERCENT"])):
                problem = f"ROLLOUT_PERCENT 未规范化：{values['ROLLOUT_PERCENT']}"

        if problem:
            failures += 1
            print(f"  ✗ {desc}：{problem}")
        else:
            print(f"  ✓ {desc}")

    # 默认值补齐后的具体取值
    values = validate(mandatory="true", default_reason="MIN_SUPPORTED",
                      default_deadline="2026-11-20T10:00:00Z")
    if (values["MANDATORY_ON"], values["MANDATORY_REASON"], values["MANDATORY_DEADLINE"]) \
            != ("1", "MIN_SUPPORTED", "2026-11-20T10:00:00Z"):
        failures += 1
        print(f"  ✗ default 补齐取值不对：{values}")
    else:
        print("  ✓ default 补齐取值正确")

    # sh 转义
    if sh_quote("a'b") != "'a'\\''b'" or sh_quote("") != "''":
        failures += 1
        print(f"  ✗ sh_quote 转义不对：{sh_quote(chr(97) + chr(39) + chr(98))}")
    else:
        print("  ✓ sh_quote 单引号转义正确")

    text = emit_sh(validate(rollout="10"))
    if "ROLLOUT_PERCENT='10'" not in text or "MANDATORY_ON='0'" not in text:
        failures += 1
        print(f"  ✗ --emit sh 输出不对：{text!r}")
    else:
        print("  ✓ --emit sh 输出可直接 eval")

    total = len(cases) + 2
    if failures:
        print(f"\npublish_args selftest 失败：{failures}/{total}")
        return 1
    print(f"\npublish_args selftest 全部通过（{total} 项）")
    return 0


# --------------------------------------------------------------------------- CLI

def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="publish_args.py",
        description="发布参数校验（灰度 / 强制更新），三仓库共享。",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    p_validate = sub.add_parser("validate", help="校验灰度 / 强制更新参数")
    p_validate.add_argument("--product", default="", help="产品名，仅用于报错前缀")
    p_validate.add_argument("--rollout", default="100", help="灰度百分比 1–100（默认 100）")
    p_validate.add_argument("--mandatory", default="", help="强制更新开关：0/1/true/false/空")
    p_validate.add_argument("--mandatory-reason", default="", help="强制更新原因")
    p_validate.add_argument("--mandatory-deadline", default="", help="强制更新期限（ISO 8601 UTC）")
    p_validate.add_argument("--default-reason",
                            help="强制开启但未给原因时使用（历史兼容）")
    p_validate.add_argument("--default-deadline",
                            help="强制开启但未给期限时使用（历史兼容）")
    p_validate.add_argument("--emit", choices=("sh", "none"), default="none",
                            help="sh：输出可直接 eval 的变量（默认 none）")

    sub.add_parser("selftest", help="自检：合法 / 非法组合")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)

    if args.command == "selftest":
        return _selftest()

    try:
        values = validate(
            product=args.product,
            rollout=args.rollout,
            mandatory=args.mandatory,
            reason=args.mandatory_reason,
            deadline=args.mandatory_deadline,
            default_reason=args.default_reason or "",
            default_deadline=args.default_deadline or "",
        )
    except ValidationError as exc:
        print(str(exc), file=sys.stderr)
        return 2

    if args.emit == "sh":
        sys.stdout.write(emit_sh(values))
    return 0


if __name__ == "__main__":
    sys.exit(main())
