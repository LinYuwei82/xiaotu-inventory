// P3 前端统一归一的回归测试（重构方案 §2.4）：
// 从 web/app.js 源码中按花括号配对提取归一相关纯函数并求值，保证测的是生产代码本体。
// 覆盖：mapEngineLastResult 全分支、normalizeUpdateStatus 双平台映射、
// 引擎 status_json 字段契约（读取键必须落在 引擎字段 ∪ mac 扩展键 ∪ 旧格式迁移键 内）、
// parseWebviewMessage / win 检查码辅助函数。
// 运行：node tests/update_web_normalize_test.mjs
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const repoRoot = join(dirname(fileURLToPath(import.meta.url)), "..");
const src = readFileSync(join(repoRoot, "web", "app.js"), "utf8");

let passed = 0;
let failed = 0;
function check(cond, name) {
  if (cond) passed++;
  else {
    failed++;
    console.error(`FAIL: ${name}`);
  }
}
function eq(actual, expected, name) {
  const ok = JSON.stringify(actual) === JSON.stringify(expected);
  if (!ok) console.error(`  期望 ${JSON.stringify(expected)}，实际 ${JSON.stringify(actual)}`);
  check(ok, name);
}

// ---- 源码提取 ----
function extract(name) {
  const marker = `function ${name}(`;
  const start = src.indexOf(marker);
  if (start < 0) throw new Error(`app.js 中找不到函数 ${name}`);
  const braceStart = src.indexOf("{", start + marker.length);
  let depth = 0;
  for (let i = braceStart; i < src.length; i++) {
    if (src[i] === "{") depth++;
    else if (src[i] === "}") {
      depth--;
      if (depth === 0) return src.slice(start, i + 1);
    }
  }
  throw new Error(`函数 ${name} 花括号不配对`);
}

const FN_NAMES = [
  "mapEngineLastResult",
  "normalizeUpdateStatus",
  "mapWinCheckResult",
  "isWinCheckResult",
  "isWinFailureCode",
  "winFailureText",
  "parseWebviewMessage",
  "nativeUpdatePlatform",
];
const extracted = FN_NAMES.map((n) => extract(n));
const make = new Function(
  "window",
  `${extracted.join("\n")}; return {${FN_NAMES.map((n) => `${n}:${n}`).join(",")}};`
);

// ---- 用例 1：mapEngineLastResult 全分支 ----
{
  const { mapEngineLastResult: m } = make({});
  eq(m("CHANNEL_MISMATCH", true), "NO_UPDATE", "map: CHANNEL_MISMATCH→NO_UPDATE");
  eq(m("PLATFORM_MISMATCH", true), "NO_UPDATE", "map: PLATFORM_MISMATCH→NO_UPDATE");
  for (const c of ["DOWNLOAD_STARTED", "DOWNLOADED", "CANCELED", "VERIFY_FAILED", "INSTALL_FAILED", "UPDATE_REQUIRED"]) {
    eq(m(c, false), "UPDATE_AVAILABLE", `map: ${c}→UPDATE_AVAILABLE`);
  }
  for (const c of ["MANIFEST_JSON_INVALID", "MANIFEST_SIGNATURE_INVALID", "MANIFEST_INVALID", "MANIFEST_URL_EMPTY", "NO_TRANSPORT", "VERSION_INVALID", "MANIFEST_PLATFORM_MISSING", "MANIFEST_ARTIFACTS_MISSING"]) {
    eq(m(c, true), "CHECK_FAILED", `map: ${c}→CHECK_FAILED`);
  }
  eq(m("COOLDOWN", true), "UPDATE_AVAILABLE", "map: COOLDOWN+pending→UPDATE_AVAILABLE");
  eq(m("COOLDOWN", false), "NO_UPDATE", "map: COOLDOWN 无 pending→NO_UPDATE");
  for (const c of ["NO_UPDATE", "UPDATE_AVAILABLE", "SKIPPED", "NOT_IN_ROLLOUT", "OS_UNSUPPORTED", "CHECK_FAILED", "INCOMPATIBLE_OS"]) {
    eq(m(c, false), c, `map: ${c} 透传`);
  }
  eq(m("", false), "", "map: 空码返回空");
  eq(m(null, false), "", "map: null 返回空");
}

// ---- 用例 2：引擎 status_json 字段契约 + win 全量映射 ----
// 引擎 status_json 字段集合快照（与 tests/update_engine_smoke.cpp 的 expected 保持一致）。
const ENGINE_FIELDS = [
  "auto_check", "bytes_done", "bytes_total", "build", "channel", "consecutive_failures",
  "enabled", "install_id", "last_check_epoch", "last_error", "last_manual_check_epoch",
  "last_result", "manifest_setup_url", "manifest_sha256", "manifest_build", "manifest_size",
  "manifest_version", "mandatory", "mandatory_deadline", "mandatory_reason", "next_check_epoch",
  "notes_url", "pending_path", "pending_size", "prefetch", "retry_after", "skipped_version",
  "skip_until_epoch", "state", "version",
];
// mac 壳扩展键（mote 模式后只剩 install_engine；恢复/预取扩展键已删）
// 与旧格式迁移键（保留至 1.1）。
const MAC_EXT_KEYS = ["install_engine"];
const LEGACY_KEYS = ["release_notes_url", "prefetch_version"];

const fullEngineStatus = {
  auto_check: true, bytes_done: 1048576, bytes_total: 5242880, build: 100,
  channel: "stable", consecutive_failures: 0, enabled: true, install_id: "iid-1",
  last_check_epoch: 1764000000, last_error: "", last_manual_check_epoch: 1763999000,
  last_result: "DOWNLOADED", manifest_setup_url: "https://x/setup.exe",
  manifest_sha256: "ab".repeat(32), manifest_build: 2026111801, manifest_size: 5242880,
  manifest_version: "1.4.3", mandatory: true, mandatory_deadline: "2026-12-01",
  mandatory_reason: "安全修复", next_check_epoch: 1764003600,
  notes_url: "https://x/notes", pending_path: "C:\\Users\\lin\\upd\\setup-1.4.3.exe",
  pending_size: 5242880, prefetch: true, retry_after: 0, skipped_version: "1.4.2",
  skip_until_epoch: 1764100000, state: "DONE", version: "1.4.3",
};

{
  const { normalizeUpdateStatus: n } = make({});
  // 契约：normalize 读取的每个 s.<键> 都必须来自引擎字段 / mac 扩展键 / 旧格式迁移键。
  const readKeys = new Set();
  for (const mm of extracted[1].matchAll(/\bs\.([a-z_][a-z0-9_]*)/g)) readKeys.add(mm[1]);
  const allowed = new Set([...ENGINE_FIELDS, ...MAC_EXT_KEYS, ...LEGACY_KEYS]);
  const unknown = [...readKeys].filter((k) => !allowed.has(k));
  eq(unknown, [], "契约: normalize 未读取契约外的键");
  check(ENGINE_FIELDS.length === 30, "契约: 引擎字段快照共 30 个");

  const w = n(fullEngineStatus, "windows");
  eq(w.channel, "stable", "win: channel");
  eq(w.last_check_at, 1764000000, "win: last_check_at←last_check_epoch");
  eq(w.next_check_at, 1764003600, "win: next_check_at←next_check_epoch");
  eq(w.check_failures, 0, "win: check_failures←consecutive_failures");
  eq(w.last_result, "UPDATE_AVAILABLE", "win: DOWNLOADED 经 map 归一");
  eq(w.update_available, true, "win: update_available←manifest_version");
  eq(w.pending_version, "1.4.3", "win: pending_version");
  eq(w.pending_build, 2026111801, "win: pending_build←manifest_build");
  eq(w.pending_size, 5242880, "win: pending_size←manifest_size");
  eq(w.pending_deadline, "2026-12-01", "win: pending_deadline←mandatory_deadline");
  eq(w.notes_url, "https://x/notes", "win: notes_url");
  eq(w.mandatory, true, "win: mandatory");
  eq(w.reason, "安全修复", "win: reason←mandatory_reason");
  eq(w.auto_check, true, "win: auto_check");
  eq(w.prefetch, true, "win: prefetch");
  eq(w.skipped_version, "1.4.2", "win: skipped_version");
  eq(w.skip_until, 1764100000, "win: skip_until←skip_until_epoch");
  eq(w.prefetch_state, "done", "win: DONE→prefetch_state=done（state 派生）");
  eq(w.prefetch_bytes, 1048576, "win: prefetch_bytes←bytes_done");
  eq(w.prefetch_total, 5242880, "win: prefetch_total←bytes_total");
  eq(w.prefetch_version, "1.4.3", "win: prefetch_version 缺省回落 manifest_version");
  eq(w.prefetch_file, "setup-1.4.3.exe", "win: pending_path 反斜杠取 basename");
  eq(w.install_engine, "package", "win: 无扩展键缺省 package");

  // mac：同一份引擎 JSON + install_engine 扩展键合并后归一。
  const macStatus = { ...fullEngineStatus,
    pending_path: "/Users/lin/upd/xiaotu-1.4.3.dmg", install_engine: "sparkle" };
  const m2 = n(macStatus, "macos");
  eq(m2.pending_build, 0, "mac: pending_build 固定 0（v2 清单无构建号）");
  eq(m2.install_engine, "sparkle", "mac: install_engine 透传");
  eq(m2.prefetch_state, "done", "mac: prefetch_state 由 state 派生（无扩展键优先）");
  eq(m2.prefetch_file, "xiaotu-1.4.3.dmg", "mac: pending_path 正斜杠取 basename");
}

// ---- 用例 3：normalizeUpdateStatus 边界 ----
{
  const { normalizeUpdateStatus: n } = make({});
  eq(n(undefined, "windows").prefetch_state, "idle", "边界: raw 为空对象兜底 IDLE→idle");
  eq(n({}, "windows").last_result, "", "边界: 空 status 各字段缺省");
  eq(n({}, "windows").auto_check, true, "边界: auto_check 缺省 true");
  eq(n({ auto_check: false }, "windows").auto_check, false, "边界: auto_check 显式 false");
  eq(n({ prefetch: false }, "windows").prefetch, false, "边界: prefetch 显式 false");
  eq(n({ version: "1.4.3" }, "windows").pending_version, "1.4.3", "边界: version 回落（≡manifest_version）");
  eq(n({ manifest_version: "1.4.3", mandatory: true }, "windows").mandatory, true, "边界: mandatory 需有更新");
  eq(n({ mandatory: true }, "windows").mandatory, false, "边界: 无版本时 mandatory 归 false");
  eq(n({ manifest_version: "1.4.3", pending_size: 999 }, "windows").pending_size, 999, "边界: pending_size 回落");
  eq(n({ release_notes_url: "https://old" }, "windows").notes_url, "https://old", "边界: 旧键 release_notes_url 迁移");
  eq(n({ prefetch_version: "1.4.0" }, "windows").prefetch_version, "1.4.0", "边界: 旧键 prefetch_version 迁移");
  for (const [state, want] of [["DOWNLOADING", "running"], ["DONE", "done"], ["INSTALLING", "done"], ["IDLE", "idle"], ["CHECKING", "idle"]]) {
    eq(n({ state }, "windows").prefetch_state, want, `边界: state=${state}→${want}`);
  }
  eq(n({ state: "FAILED", last_result: "CANCELED" }, "windows").prefetch_state, "canceled", "边界: FAILED+CANCELED→canceled");
  eq(n({ state: "FAILED" }, "windows").prefetch_state, "failed", "边界: FAILED→failed");
}

// ---- 用例 4：win 检查码辅助 + 消息解析 ----
{
  const { mapWinCheckResult: mc, isWinCheckResult: ic, isWinFailureCode: fc, winFailureText: ft, parseWebviewMessage: pm } = make({});
  eq(mc("UPDATE_AVAILABLE"), "UPDATE_AVAILABLE", "mc: UPDATE_AVAILABLE 原样");
  eq(mc("NO_UPDATE"), "NO_UPDATE", "mc: NO_UPDATE 原样");
  eq(mc("UPDATE_UNAVAILABLE"), "NOT_CONFIGURED", "mc: UNAVAILABLE→NOT_CONFIGURED");
  eq(mc("FETCH_FAILED"), "CHECK_FAILED", "mc: 其余→CHECK_FAILED");
  check(ic("CHECK_FAILED") && ic("NOT_CONFIGURED"), "ic: 检查类码判定 true");
  check(!ic("DOWNLOADED") && !ic(""), "ic: 下载态/空判定 false");
  check(fc("DOWNLOAD_FAILED") && fc("MANIFEST_INVALID") && fc("NO_PENDING_UPDATE") && fc("OPEN_FAILED"), "fc: 失败码判定");
  check(!fc("DOWNLOADED") && !fc("OK"), "fc: 非失败码判定");
  eq(ft("NO_PENDING_UPDATE"), "没有可用的更新包，请先点「检查更新」", "ft: 已知码文案");
  eq(ft("WEIRD_CODE"), "WEIRD_CODE", "ft: 未知码原样回显");
  eq(ft(""), "未知错误", "ft: 空码兜底");
  eq(pm({ data: '{"event":"checkNow","status":"{\\"state\\":\\"DONE\\"}"}' }).status.state, "DONE", "pm: status 字符串二次解析");
  eq(pm({ data: { event: "x", status: { state: "IDLE" } } }).status.state, "IDLE", "pm: 对象直接透传");
  eq(pm({ data: "not json" }), null, "pm: 非法 JSON→null");
  eq(pm(null), null, "pm: 空事件→null");
}

// ---- 用例 5：nativeUpdatePlatform 平台判定 ----
{
  const mac = make({ webkit: { messageHandlers: {} } });
  const win = make({ chrome: { webview: {} } });
  eq(mac.nativeUpdatePlatform(), "macos", "platform: webkit→macos");
  eq(win.nativeUpdatePlatform(), "windows", "platform: 非 webkit→windows");
}

console.log(`\nnormalize 回归: ${passed} 通过, ${failed} 失败`);
process.exit(failed ? 1 : 0);
