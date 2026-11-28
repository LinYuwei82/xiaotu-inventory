/* app/update_normalize.js：更新状态归一与纯格式化辅助（无 DOM 依赖）。
 * 字段契约见 tests/update_web_normalize_test.mjs。
 */
  export function formatBytes(bytes) {
    const value = Number(bytes || 0);
    if (value < 1024) return value + " B";
    if (value < 1024 * 1024) return (value / 1024).toFixed(1) + " KB";
    if (value < 1024 * 1024 * 1024) return (value / 1024 / 1024).toFixed(1) + " MB";
    return (value / 1024 / 1024 / 1024).toFixed(2) + " GB";
  }

  // 强制升级原因文案：清单 mandatory_reason 与服务端 MIN_SUPPORTED 共用。
  export function mandatoryReasonText(reason) {
    const labels = {
      SECURITY: "安全漏洞修复",
      PROTOCOL: "协议不兼容",
      REGULATORY: "法规或税控要求",
      MIN_SUPPORTED: "低于服务端最低支持版本",
    };
    return labels[reason] || "";
  }

  // 强制是否已生效：未设期限 → 立即强制；设了期限 → 到期才强制（截止前可稍后）。
  // 期限解析失败按「已到期」处理，避免格式问题成为绕过手段；三端同一口径。
  export function mandatoryActive(status) {
    if (!status || !status.mandatory || !status.update_available) return false;
    const raw = status.pending_deadline || status.mandatory_deadline || "";
    if (!raw) return true;
    const at = Date.parse(raw);
    return !Number.isFinite(at) || Date.now() >= at;
  }

  // macOS v2 清单没有构建号（build 固定 0）：没有构建号时只显示版本号。
  export function versionWithBuild(version, build) {
    const text = version || "新版本";
    return build ? `${text}（构建 ${build}）` : text;
  }

  // P3 统一状态归一（方案 §2.4）：两壳都输出引擎统一 status_json
  // （mac 另带平台扩展键），这里归一成 renderNativeUpdate 期望的内部形状。
  // platform："macos" | "windows"（mac v2 清单无构建号，界面固定按 0 隐藏）。
  export function mapEngineLastResult(raw, hasPending) {
    if (!raw) return "";
    if (raw === "CHANNEL_MISMATCH" || raw === "PLATFORM_MISMATCH") return "NO_UPDATE";
    if (
      raw === "DOWNLOAD_STARTED" ||
      raw === "DOWNLOADED" ||
      raw === "CANCELED" ||
      raw === "VERIFY_FAILED" ||
      raw === "INSTALL_FAILED" ||
      raw === "MANUAL" ||
      raw === "UPDATE_REQUIRED"
    ) {
      return "UPDATE_AVAILABLE";
    }
    if (
      raw === "MANIFEST_JSON_INVALID" ||
      raw === "MANIFEST_SIGNATURE_INVALID" ||
      raw === "MANIFEST_INVALID" ||
      raw === "MANIFEST_URL_EMPTY" ||
      raw === "NO_TRANSPORT" ||
      raw === "VERSION_INVALID" ||
      raw === "MANIFEST_PLATFORM_MISSING" ||
      raw === "MANIFEST_ARTIFACTS_MISSING"
    ) {
      return "CHECK_FAILED";
    }
    // 手动冷却已移除；此映射仅为兼容旧壳落盘的 last_result=COOLDOWN。
    if (raw === "COOLDOWN") return hasPending ? "UPDATE_AVAILABLE" : "NO_UPDATE";
    return raw;  // NO_UPDATE / UPDATE_AVAILABLE / SKIPPED / NOT_IN_ROLLOUT /
                 // OS_UNSUPPORTED / INCOMPATIBLE_* / CHECK_FAILED 等同名透传
  }
  export function normalizeUpdateStatus(raw, platform) {
    const s = raw || {};
    const manifestVersion = s.manifest_version || s.version || "";
    const updateAvailable = !!manifestVersion;
    const state = s.state || "IDLE";
    // prefetch_state 由引擎 state 派生（两端同口径；mac 安装走 Sparkle，
    // 引擎不会进入 DOWNLOADING/DONE，恒为 idle）。
    let prefetchState;
    if (state === "DOWNLOADING") {
      prefetchState = "running";
    } else if (state === "DONE" || state === "INSTALLING") {
      prefetchState = "done";
    } else if (state === "FAILED") {
      prefetchState = s.last_result === "CANCELED" ? "canceled" : "failed";
    } else {
      prefetchState = "idle";
    }
    const pendingPath = s.pending_path || "";
    const pendingBase = pendingPath ? pendingPath.split(/[\\/]/).pop() : "";
    return {
      channel: s.channel || "stable",
      last_check_at: s.last_check_epoch || 0,
      next_check_at: s.next_check_epoch || 0,
      check_failures: s.consecutive_failures || 0,
      last_result: mapEngineLastResult(s.last_result || "", updateAvailable),
      update_available: updateAvailable,
      pending_version: manifestVersion,
      pending_build: platform === "macos" ? 0 : s.manifest_build || 0,
      pending_size: s.manifest_size || s.pending_size || 0,
      pending_deadline: s.mandatory_deadline || "",
      notes_url: s.notes_url || s.release_notes_url || "",
      mandatory: !!s.mandatory && updateAvailable,
      reason: s.mandatory_reason || "",
      auto_check: s.auto_check !== false,
      prefetch: s.prefetch !== false,
      skipped_version: s.skipped_version || "",
      skip_until: s.skip_until_epoch || 0,
      prefetch_state: prefetchState,
      prefetch_bytes: s.bytes_done || 0,
      prefetch_total: s.bytes_total || s.manifest_size || 0,
      prefetch_version: s.prefetch_version || manifestVersion,
      prefetch_file: pendingBase,
      // mac 唯一在用的平台扩展键（sparkle/package）；win 无此键按 package 处理。
      install_engine: s.install_engine || "package",
    };
  }

  // 把 Windows 壳的检查结果码映射成界面文案表的键
  export function mapWinCheckResult(code) {
    if (code === "UPDATE_AVAILABLE" || code === "NO_UPDATE") return code;
    if (code === "UPDATE_UNAVAILABLE") return "NOT_CONFIGURED";
    return "CHECK_FAILED";
  }

  // 只有这些码表示「检查结果」；壳在下载/安装时也会改写 last_result，
  // 不能让它把「检查结果」一栏覆盖成下载状态。
  export function isWinCheckResult(code) {
    return (
      code === "UPDATE_AVAILABLE" ||
      code === "NO_UPDATE" ||
      code === "NOT_CONFIGURED" ||
      code === "CHECK_FAILED"
    );
  }

  // 壳返回的失败码：统一按失败处理，否则前端会静默吞掉（点下载/安装没反应）。
  export function isWinFailureCode(code) {
    return /FAILED|INVALID|MISMATCH|PARSE_ERROR|UNAVAILABLE|NO_PENDING_UPDATE|OPEN_FAILED/.test(
      code || ""
    );
  }
  export function winFailureText(code) {
    const map = {
      NO_PENDING_UPDATE: "没有可用的更新包，请先点「检查更新」",
      OPEN_FAILED: "安装程序启动失败",
      DOWNLOAD_FAILED: "安装包下载失败",
      ALREADY_RUNNING: "下载已在进行中，请稍候",
      NOT_RUNNING: "当前没有进行中的下载",
      FETCH_FAILED: "清单获取失败",
      MANIFEST_INVALID: "清单内容不完整",
      SIGNATURE_INVALID: "清单签名校验失败",
    };
    return map[code] || code || "未知错误";
  }
  export function parseWebviewMessage(event) {
    let msg = event && event.data;
    if (typeof msg === "string") {
      try {
        msg = JSON.parse(msg);
      } catch (error) {
        return null;
      }
    }
    if (!msg || typeof msg !== "object") return null;
    // 兼容旧版壳：status 可能是 JSON 文本而不是对象。
    if (typeof msg.status === "string") {
      try {
        msg.status = JSON.parse(msg.status);
      } catch (error) {
        msg.status = null;
      }
    }
    return msg;
  }
