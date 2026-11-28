/* app/update.js：更新检查、下载安装与升级状态展示。
 * 由原 web/app.js 按职责拆出，供本模块与入口共同加载。
 */
import { api, askConfirm, askDialog, can, escapeHtml, isWinWebview, notice, openExternalUrl, showNotice, state } from "./core.js";

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

  // ---------- 桌面壳更新桥 ----------
  // macOS：WKWebView messageHandlers（方法名 xiaotuUpdateXxx，结果由原生调 window.xxxDone）
  // Windows：WebView2 HostObject updater_win（方法名 checkNow/statusJson…，结果走 chrome.webview
  // 的 message 事件）。两边字段名也不同，这里统一适配成 renderNativeUpdate 期望的形状。
  export let winUpdaterBridgeInstance = null;
  export let winUpdaterListenerInstalled = false;
  export let winUpdaterStatusCache = null;

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
  export function winUpdateBridge() {
    if (!isWinWebview()) return null;
    const hostObjects = window.chrome.webview.hostObjects;
    if (!hostObjects || !hostObjects.updater_win) return null;
    if (winUpdaterBridgeInstance) return winUpdaterBridgeInstance;

    const updater = hostObjects.updater_win;
    let lastCheckResult = "";  // 壳内的 statusJson 不存最近一次检查结果，这里在 JS 侧记一份
    const finish = (name, payload) => {
      const fn = window[name];
      if (typeof fn === "function") fn(payload);
    };
    const readStatus = async () => {
      const raw = await updater.statusJson();
      let parsed = {};
      try {
        parsed = JSON.parse(raw);
      } catch (error) {
        parsed = {};
      }
      if (!parsed.last_result && lastCheckResult) parsed.last_result = lastCheckResult;
      if (!lastCheckResult && isWinCheckResult(parsed.last_result)) {
        lastCheckResult = parsed.last_result;
      } else if (lastCheckResult) {
        parsed.last_result = lastCheckResult;
      }
      winUpdaterStatusCache = parsed;
      return parsed;
    };

    if (!winUpdaterListenerInstalled) {
      winUpdaterListenerInstalled = true;
      window.chrome.webview.addEventListener("message", (event) => {
        const msg = parseWebviewMessage(event);
        if (!msg || !msg.event || msg.event === "printPdf") return;
        // 壳主动推送的启动快照：一次带上状态和上次安装结果（不依赖 HostObject 返回值）。
        if (msg.event === "startup") {
          if (msg.status) {
            winUpdaterStatusCache = msg.status;
            finish("xiaotuUpdateStatusDone", {
              status: msg.status,
              code: "",
              ok: true,
              update_available: !!(msg.status.manifest_version || msg.status.version),
            });
          }
          if (msg.install_result && reportInstallResult(msg.install_result)) {
            try {
              updater.ackInstallResult();
            } catch (error) {
              // 旧壳没有该方法：下次启动会再提示。
            }
          }
          return;
        }
        if (msg.status) winUpdaterStatusCache = msg.status;
        // P3：win 桥不再预归一，透传引擎原始 status，由 handler 统一 normalize。
        const status = msg.status || winUpdaterStatusCache || {};
        const handlers = {
          checkNow: "xiaotuUpdateCheckDone",
          downloadUpdate: "xiaotuUpdateDownloadDone",
          installUpdate: "xiaotuUpdateInstallDone",
          cancelPrefetch: "xiaotuUpdateCancelDone",
          setOptions: "xiaotuUpdateOptionsDone",
        };
        const done = handlers[msg.event];
        if (!done) return;
        if (msg.event === "checkNow") {
          lastCheckResult = mapWinCheckResult(msg.code);
          status.last_result = lastCheckResult;
        } else if (lastCheckResult) {
          // 非检查事件（下载/安装/设置）的 status 里没有检查结果，
          // 用 JS 侧记下的一次补上，避免「检查结果」被打回「尚未检查」。
          status.last_result = lastCheckResult;
        }
        const failed = isWinFailureCode(msg.code);
        finish(done, {
          status,
          code: msg.code,
          ok: !failed,
          update_available: !!(status.manifest_version || status.version),
        });
      });
    }

    winUpdaterBridgeInstance = {
      xiaotuUpdateStatus: {
        postMessage: () => {
          // 优先让壳用事件推状态快照，避免依赖 HostObject 返回值。
          try {
            if (updater && typeof updater.reportStartup === "function") {
              updater.reportStartup();
              return;
            }
          } catch (error) {
            // 旧壳没有该方法：回退到返回值方式。
          }
          (async () => {
            try {
              const s = await readStatus();
              finish("xiaotuUpdateStatusDone", s);
            } catch (error) {
              finish("xiaotuUpdateStatusDone", {});
            }
          })();
        },
      },
      xiaotuUpdateCheckNow: {
        postMessage: () => {
          updater.checkNow();
        },
      },
      xiaotuUpdateOptions: {
        postMessage: (opts) => {
          // 壳的 setOptions 需要两个参数，只是切换一项时用缓存里的另一项补齐
          const cache = winUpdaterStatusCache || {};
          const autoCheck =
            opts && "auto_check" in opts ? !!opts.auto_check : cache.auto_check !== false;
          const prefetch =
            opts && "prefetch" in opts ? !!opts.prefetch : cache.prefetch !== false;
          updater.setOptions(autoCheck, prefetch);
        },
      },
      xiaotuUpdateDownload: {
        postMessage: () => {
          updater.downloadUpdate();
        },
      },
      xiaotuUpdateCancelPrefetch: {
        postMessage: () => {
          updater.cancelPrefetch();
        },
      },
      xiaotuUpdateInstall: {
        postMessage: () => {
          (async () => {
            try {
              const s = winUpdaterStatusCache || (await readStatus());
              if (s.pending_path) {
                updater.installUpdate();
                return;
              }
              // 还没下载：自动预取可能已经在跑，此时 downloadUpdate 只会回
              // ALREADY_RUNNING 且壳里 cb 为空、不产生任何完成事件，所以这里
              // 改为轮询状态——pending_path 一出现就安装，避免永久卡在
              // 「正在准备安装包…」。
              showNotice("正在下载安装包，完成后会自动开始安装", "ok");
              updater.downloadUpdate();
              const deadline = Date.now() + 15 * 60 * 1000;
              let installing = false;
              const tick = async () => {
                if (installing) return;
                let latest = {};
                try {
                  latest = await readStatus();
                } catch (error) {
                  latest = {};
                }
                if (latest.pending_path) {
                  installing = true;
                  updater.installUpdate();
                  return;
                }
                if (latest.state === "FAILED" && latest.last_result === "DOWNLOAD_FAILED") {
                  finish("xiaotuUpdateInstallDone", {
                    status: latest,
                    ok: false,
                    code: "FAILED",
                  });
                  return;
                }
                if (Date.now() >= deadline) {
                  finish("xiaotuUpdateInstallDone", {
                    status: latest,
                    ok: false,
                    code: "DOWNLOAD_TIMEOUT",
                  });
                  return;
                }
                setTimeout(tick, 700);
              };
              setTimeout(tick, 700);
            } catch (error) {
              finish("xiaotuUpdateInstallDone", { ok: false, code: "BRIDGE_ERROR" });
            }
          })();
        },
      },
    };
    return winUpdaterBridgeInstance;
  }
  export function nativeUpdateBridge() {
    if (window.webkit && window.webkit.messageHandlers) return window.webkit.messageHandlers;
    return winUpdateBridge();
  }

  // 壳状态归一的平台参数（mac v2 清单无构建号，pending_build 固定 0）。
  export function nativeUpdatePlatform() {
    return window.webkit && window.webkit.messageHandlers ? "macos" : "windows";
  }

  // 强制升级拦截：壳内状态一到就弹不可跳过的提示，只能点「立即安装」。
  // blocker 只弹一次；安装由壳交给 Sparkle 完成，安装包会重新下载。
  export let mandatoryBlockOpen = false;
  export function blockMandatoryUpdate(status) {
    if (mandatoryBlockOpen || !mandatoryActive(status)) return;
    const bridge = nativeUpdateBridge();
    if (!bridge || !bridge.xiaotuUpdateInstall) return;
    mandatoryBlockOpen = true;
    const reason = mandatoryReasonText(status.reason);
    const size = status.pending_size ? `约 ${formatBytes(status.pending_size)}` : "安装包";
    const deadline = status.pending_deadline
      ? `请于 ${status.pending_deadline} 前完成升级。`
      : "";
    askDialog({
      message:
        `当前版本必须升级${reason ? "（" + reason + "）" : ""}：` +
        `${versionWithBuild(status.pending_version, status.pending_build)}。` +
        `${deadline}安装时会重新下载${size}并退出程序，升级后才能继续使用。`,
      okText: "立即安装",
      cancelText: null,
      blocking: true,
    }).then(() => {
      bridge.xiaotuUpdateInstall.postMessage({});
      mandatoryBlockOpen = false;
    });
  }

  // 启动时主动取一次壳内更新状态，用于强制升级拦截；关于页打开后会自己再取。
  export function requestNativeUpdateStatus(onStatus) {
    const bridge = nativeUpdateBridge();
    if (!bridge || !bridge.xiaotuUpdateStatus) return;
    const previous = window.xiaotuUpdateStatusDone;
    window.xiaotuUpdateStatusDone = (payload) => {
      window.xiaotuUpdateStatusDone = previous;
      const raw = payload && payload.status ? payload.status : payload;
      if (raw) {
        onStatus(normalizeUpdateStatus(raw, nativeUpdatePlatform()));
      }
    };
    bridge.xiaotuUpdateStatus.postMessage({});
  }

  // Windows 更新重启后的一次性结果提示：兼容 1.0.19–1.0.23 协助器留下的
  // apply-state.json / install-result.json（新壳安装不再产生，读到为空即静默跳过）。
  // 启动进入主界面后取一次，按成功/失败/被中断提示用户，随后 ackInstallResult 删文件，不再重复弹。
  export let installResultChecked = false;
  export function reportInstallResult(data) {
    if (!data || typeof data !== "object") return false;
    const result = data.result || null;
    const interrupted = !!data.interrupted;
    const launch = data.launch || null;
    if (interrupted) {
      showNotice("上次更新安装被中断，当前仍是旧版本；可在「设置 → 软件更新」中重试", "warn");
      return true;
    }
    if (result) {
      const unchanged =
        !!result.version_before &&
        !!result.version_after &&
        result.version_after === result.version_before;
      const failed = !!result.error || Number(result.exit_code) !== 0 || unchanged;
      if (failed) {
        showNotice("更新安装失败，当前仍是旧版本；可在「设置 → 软件更新」中重试", "error");
      } else {
        showNotice(`更新成功，当前版本 ${result.version_after || ""}`, "ok");
      }
      return true;
    }
    if (launch) {
      // 新壳：拉起 setup 前落盘的目标版本 vs 本次实际版本，判断装成没装成。
      const currentVersion = data.current_version || "";
      const targetVersion = launch.target_version || "";
      if (targetVersion && currentVersion === targetVersion) {
        showNotice(`更新成功，当前版本 ${currentVersion}`, "ok");
      } else {
        const tailLines = String(launch.setup_log_tail || "")
          .split(/\r?\n/)
          .filter((line) => line.trim())
          .slice(-3)
          .join(" ");
        const reason = (winUpdaterStatusCache && winUpdaterStatusCache.last_error) || "";
        showNotice(
          `上次更新没有完成（当前 ${currentVersion || "未知版本"}，目标 ${
            targetVersion || "未知版本"
          }）${tailLines ? "；安装日志：" + tailLines : ""}${
            reason ? "；原因：" + reason : ""
          }`,
          "error"
        );
      }
      return true;
    }
    return false;
  }
  export function maybeReportInstallResultOnce() {
    if (!isWinWebview() || installResultChecked) return;
    const hostObjects = window.chrome.webview.hostObjects;
    if (!hostObjects || !hostObjects.updater_win) return;
    installResultChecked = true;
    // 返回值型宿主调用在部分环境拿不到结果；改用 void 方法让壳用 startup 事件推送。
    try {
      hostObjects.updater_win.reportStartup();
    } catch (error) {
      // 旧壳没有该方法：静默跳过。
    }
  }

  // 软件更新：状态、下载、安装都在服务端完成；前端只展示结果与发起动作。
  // 不自动安装：下载后仍需管理员点「安装更新」并二次确认。
  // 原生自动更新卡片：由壳内 XiaotuUpdater 提供状态、检查与安装。
  export function renderNativeUpdate(card, nativeBridge) {
    // P3：壳回调里都是引擎原始 status（+平台扩展键），统一在此归一后再渲染。
    const statusPlatform = nativeUpdatePlatform();
    const resultText = (status) => {
      const labels = {
        UPDATE_AVAILABLE: "发现新版本",
        NO_UPDATE: "已是最新版本",
        CHECK_FAILED: "检查失败（网络或清单验签未通过）",
        SKIPPED: "已跳过该版本",
        INCOMPATIBLE_SCHEMA: "版本不兼容，需先升级到中间版本",
        INTERMEDIATE_REQUIRED: "需先安装中间版本",
        OS_UNSUPPORTED: "当前系统版本过低，不能安装该更新",
        INCOMPATIBLE_API: "客户端接口版本不兼容",
        NOT_IN_ROLLOUT: "该版本暂未对当前安装开放（灰度）",
        NOT_CONFIGURED: "未配置更新源",
      };
      const last = status && status.last_result;
      // 防御：后端已明确 update_available=false（pending_version ≤ current_version），
      // 但 last_result 还留着上次的 UPDATE_AVAILABLE——这种安装后残留状态会让
      // "结果文案"一直显示"发现新版本"，必须按当前状态覆盖修正。
      // 2026-11-19 在线更新 1.0.3→1.0.4 后仍显示"发现新版本"即因此。
      if (!status || status.update_available === false) {
        if (last === "UPDATE_AVAILABLE") return labels["NO_UPDATE"];
      }
      return last ? labels[last] || last : "尚未检查";
    };

    // 后台预下载状态文案：下载中显示进度，重试与失败显示原因，完成后显示包名与大小。
    function prefetchText(status) {
      const state = status && status.prefetch_state;
      const bytes = status && status.prefetch_bytes ? formatBytes(status.prefetch_bytes) : "0 B";
      const total = status && status.prefetch_total ? formatBytes(status.prefetch_total) : "";
      if (state === "running") {
        return total
          ? `正在后台下载 ${status.prefetch_version || ""}（${bytes} / ${total}）`
          : `正在后台下载 ${status.prefetch_version || ""}（${bytes}）`;
      }
      if (state === "done") {
        return `已下载并校验：${status.prefetch_file || ""}（${bytes}）`;
      }
      if (state === "failed") {
        return "后台下载失败：可手动下载";
      }
      if (state === "canceled") {
        return "已取消本次后台下载，可手动下载";
      }
      return "—";
    }

    // Windows 点「安装更新」后壳内可能先补下载；期间按钮保持禁用与提示文案。
    let installPreparing = false;
    let pollTimer = null;
    function stopPoll() {
      if (pollTimer) {
        clearTimeout(pollTimer);
        pollTimer = null;
      }
    }
    // 下载进度只在页面停留时轮询，离开关于页后定时器自行结束。
    function schedulePoll(status) {
      stopPoll();
      if (!status || status.prefetch_state !== "running") return;
      pollTimer = setTimeout(() => {
        pollTimer = null;
        if (!document.body.contains(card)) return;
        nativeBridge.xiaotuUpdateStatus.postMessage({});
      }, 1500);
    }

    function render(status) {
      status = status || { update_available: false };
      // Sparkle 安装引擎：下载 / 验签 / 替换 / 重启都在安装流程内完成，
      // 不再提供「后台预下载、下载并校验」这些自研入口。
      const sparkle = status.install_engine === "sparkle";
      const prefetchRunning = status.prefetch_state === "running";
      // Windows 与 macOS 同一套界面：只在真有下载在跑时显示进度行，
      // 不再暴露「后台下载」开关与手动下载入口（安装时壳内自动补下载）。
      const rows = [
        ["检查通道", status.channel || "stable"],
        [
          "上次检查",
          status.last_check_at ? new Date(status.last_check_at * 1000).toLocaleString() : "—",
        ],
        ["检查结果", resultText(status)],
        [
          "待安装版本",
          status.update_available
            ? versionWithBuild(status.pending_version, status.pending_build)
            : "—",
        ],
        !sparkle && prefetchRunning ? ["后台下载", prefetchText(status)] : null,
      ].filter(Boolean);
      // 强制升级：到期后阻塞式提示，不提供“跳过此版本”；截止前仍按普通更新提示。
      const reason = mandatoryReasonText(status.reason);
      const forceNow = mandatoryActive(status);
      const mandatory =
        status.update_available && status.mandatory
          ? `<div class="notice ${forceNow ? "error" : "warn"}" style="margin:10px 0">
               <strong>${forceNow ? "必须升级" : "计划升级"}</strong>：${
                 reason ? escapeHtml(reason) + "，" : ""
               }当前版本需升级到 ${escapeHtml(
                 status.pending_version || "新版本"
               )}。${
                 forceNow
                   ? "此更新已到截止时间，不能跳过。"
                   : status.pending_deadline
                     ? `请在 ${escapeHtml(status.pending_deadline)} 前完成升级，到期后将无法跳过。`
                     : ""
               }
             </div>`
          : "";
      card.innerHTML = `
        <div class="page-head" style="margin-bottom:8px">
          <div>
            <h2>软件更新</h2>
            <p class="page-sub">检查与下载均通过签名校验；安装前会再次确认并关闭程序</p>
          </div>
        </div>
        ${mandatory}
        <table class="table"><tbody>${rows
          .map(
            ([label, value]) =>
              `<tr><th style="width: 120px">${escapeHtml(label)}</th><td>${escapeHtml(
                value
              )}</td></tr>`
          )
          .join("")}</tbody></table>
        <div style="display:flex;gap:16px;flex-wrap:wrap;margin-top:10px">
          <label class="checkbox"><input type="checkbox" data-role="auto-check" ${
            status.auto_check ? "checked" : ""
          } /> 自动检查更新</label>
        </div>
        <div style="display:flex;gap:8px;flex-wrap:wrap;margin-top:12px">
          <button class="btn" type="button" data-role="check">检查更新</button>
          ${
            status.update_available && status.notes_url
              ? `<button class="btn" type="button" data-role="notes">查看更新详情</button>`
              : ""
          }
          <button class="btn primary" type="button" data-role="install" ${
            status.update_available && !installPreparing ? "" : "disabled"
          }>${installPreparing ? "正在准备安装包…" : "安装更新"}</button>
        </div>`;

      const checkButton = card.querySelector('[data-role="check"]');
      checkButton.addEventListener("click", () => {
        checkButton.disabled = true;
        checkButton.textContent = "检查中…";
        nativeBridge.xiaotuUpdateCheckNow.postMessage({});
      });
      const notesButton = card.querySelector('[data-role="notes"]');
      if (notesButton) {
        notesButton.addEventListener("click", () => openExternalUrl(status.notes_url));
      }
      const autoInput = card.querySelector('[data-role="auto-check"]');
      autoInput.addEventListener("change", () => {
        autoInput.disabled = true;
        nativeBridge.xiaotuUpdateOptions.postMessage({ auto_check: autoInput.checked });
      });
      card.querySelector('[data-role="install"]').addEventListener("click", async () => {
        const size = status.pending_size
          ? `约 ${formatBytes(status.pending_size)}`
          : "安装包";
        const downloaded = !sparkle && status.prefetch_state === "done";
        const forceNow = mandatoryActive(status);
        const deadlineNote =
          !forceNow && status.mandatory && status.pending_deadline
            ? `该版本为强制更新，请在 ${status.pending_deadline} 前完成，到期后将无法跳过。`
            : "";
        // Sparkle 负责下载、验签、原子替换并自动重启；不再有「拖拽安装」这一步。
        const installTail = sparkle
          ? `安装时由 Sparkle 下载并校验${size}，完成后自动重启。是否继续？`
          : downloaded
            ? "将使用已下载并校验的安装包，随后关闭程序并完成安装。是否继续？"
            : `安装时会先下载并校验${size}，随后关闭程序并完成安装。是否继续？`;
        const confirmed = await askConfirm(
          forceNow
            ? `此更新已到截止时间（${mandatoryReasonText(status.reason) || "服务端要求"}），不能跳过。` +
                installTail
            : `将安装 ${versionWithBuild(
                status.pending_version,
                status.pending_build
              )}。` + deadlineNote + installTail,
          { okText: "安装", cancelText: "取消" }
        );
        if (!confirmed) return;
        if (!sparkle) {
          installPreparing = true;
          render(status);
        }
        nativeBridge.xiaotuUpdateInstall.postMessage({});
      });
      schedulePoll(status);
    }

    // 状态回调直接传 status 本体；检查回调里是 {status: ...} 包装。
    window.xiaotuUpdateStatusDone = (payload) =>
      render(normalizeUpdateStatus((payload && payload.status) || payload || {}, statusPlatform));
    window.xiaotuUpdateCheckDone = (payload) => {
      const status = normalizeUpdateStatus((payload && payload.status) || {}, statusPlatform);
      render(status);
      if (payload && payload.update_available) {
        showNotice("发现新版本，可点「安装更新」完成替换", "ok");
      } else if (payload && payload.ok === false) {
        showNotice(resultText(status), "error");
      } else {
        showNotice(resultText(status), "ok");
      }
    };
    window.xiaotuUpdateInstallDone = (payload) => {
      installPreparing = false;
      if (payload && payload.ok === false) {
        const reason =
          (payload && payload.detail) ||
          (payload && payload.status && payload.status.last_error) ||
          "";
        showNotice(
          "无法启动安装流程：" +
            winFailureText(payload.code) +
            (reason ? "（" + reason + "）" : ""),
          "error"
        );
        // 补下载失败等场景：拉一次状态恢复按钮，让用户能直接重试。
        nativeBridge.xiaotuUpdateStatus.postMessage({});
      } else if (payload && (payload.code === "OPENED" || payload.code === "STARTED")) {
        showNotice("安装程序已启动，程序会自动关闭并完成安装", "ok");
      } else if (payload && payload.code === "MANUAL") {
        // 绿色版：壳已打开 zip 所在目录并退出，需用户手动解压覆盖。
        showNotice("更新包已下载并校验，已打开所在目录；关闭程序后解压覆盖原文件，重新打开即完成更新", "ok");
      }
    };
    window.xiaotuUpdateOptionsDone = (payload) => {
      if (payload && payload.ok === false) {
        showNotice("设置未保存：" + (payload.code || ""), "error");
      }
      render(normalizeUpdateStatus((payload && payload.status) || {}, statusPlatform));
    };
    window.xiaotuUpdateDownloadDone = (payload) => {
      const status = normalizeUpdateStatus((payload && payload.status) || {}, statusPlatform);
      render(status);
      if (payload && payload.ok === false) {
        showNotice(winFailureText(payload.code), "error");
      } else if (payload && payload.code === "STARTED") {
        showNotice("已开始下载，完成后会自动校验", "ok");
      }
    };
    window.xiaotuUpdateCancelDone = (payload) => {
      render(normalizeUpdateStatus((payload && payload.status) || {}, statusPlatform));
      if (payload && payload.code === "CANCELED") {
        showNotice("已取消本次后台下载", "ok");
      }
    };
    nativeBridge.xiaotuUpdateStatus.postMessage({});
  }
  export async function renderUpdateCard(mount) {
    const card = mount.querySelector('[data-role="update"]');
    if (!card) return;
    // 桌面壳内：检查走壳内清单验签（Ed25519），安装交给原生完成；
    // 浏览器访问时没有该桥，仍走服务端更新源。
    const nativeBridge = nativeUpdateBridge();
    if (nativeBridge && nativeBridge.xiaotuUpdateCheckNow) {
      renderNativeUpdate(card, nativeBridge);
      return;
    }
    const canManage = can("setting.write");
    let polling = false;

    function statusText(data) {
      if (!data.configured) return "未配置更新源（[update] manifest_url / manifest_file 为空）";
      if (data.error) return "检查失败：" + data.error.message;
      if (data.update_available && data.latest) {
        return `发现新版本 ${data.latest.version}（构建 ${data.latest.build}）`;
      }
      if (data.up_to_date) return "已是最新版本";
      return "当前无需更新（" + data.code + "）";
    }

    function render(data) {
      const latest = data.latest;
      const download = data.download || {};
      const rows = [
        ["当前版本", `${data.current.version}（构建 ${data.current.build}，schema v${data.current.schema_version}）`],
        ["最新版本", latest ? `${latest.version}（构建 ${latest.build}）` : "—"],
        ["发布日期", latest && latest.published_at ? latest.published_at : "—"],
        ["状态", statusText(data)],
        [
          "清单来源",
          data.manifest_source === "network"
            ? "厂商更新源（本次已刷新）"
            : data.manifest_source === "cache"
              ? "本地缓存" + (data.manifest_stale ? "（网络不可用，已回退）" : "")
              : data.manifest_source === "file"
                ? "本地清单文件"
                : "—",
        ],
        [
          "后台策略",
          `${data.auto_check === false ? "自动检查已关闭" : "自动检查已开启"}；` +
            `${data.prefetch === false ? "不自动下载" : "发现新版本自动下载"}（config.toml）`,
        ],
      ];
      if (data.manifest_warning) {
        rows.push(["清单提示", data.manifest_warning]);
      }
      // 强制升级：服务端 min_supported_version 或清单 mandatory 都会置位；
      // 设了期限时，截止前仍按普通更新提示，到期才阻塞。
      const mandatoryReason = mandatoryReasonText(data.mandatory_reason || "");
      const deadlineRaw = data.pending_deadline || data.mandatory_deadline || "";
      const forceNow = mandatoryActive({
        mandatory: data.mandatory,
        update_available: data.update_available,
        pending_deadline: deadlineRaw,
      });
      const mandatory =
        data.update_available && data.mandatory
          ? `<div class="notice ${forceNow ? "error" : "warn"}" style="margin:0 0 10px">
               <strong>${forceNow ? "必须升级" : "计划升级"}</strong>：${
                 mandatoryReason ? escapeHtml(mandatoryReason) + "，" : ""
               }当前版本需升级到 ${escapeHtml(
                 latest ? latest.version : "新版本"
               )}。${
                 forceNow
                   ? "此更新已到截止时间，不能跳过。"
                   : deadlineRaw
                     ? `请在 ${escapeHtml(deadlineRaw)} 前完成升级，到期后将无法跳过。`
                     : ""
               }
             </div>`
          : "";
      let downloadLine = "";
      if (download.state === "running") {
        downloadLine = "正在下载并校验安装包…";
      } else if (download.state === "done") {
        downloadLine = `已下载并校验：${download.file}（${formatBytes(download.bytes)}）`;
      } else if (download.state === "canceled") {
        downloadLine = "已取消本次下载，可重新下载";
      } else if (download.state === "failed") {
        downloadLine = `下载或校验失败：${download.message || download.code || ""}`;
      }
      card.innerHTML = `
        <div class="page-head" style="margin-bottom:8px">
          <div>
            <h2>软件更新</h2>
            <p class="page-sub">安装会退出服务、自动替换程序并重启；替换失败自动回滚旧版本</p>
          </div>
        </div>
        ${mandatory}
        <table class="table"><tbody>${rows
          .map(
            ([label, value]) =>
              `<tr><th style="width: 120px">${escapeHtml(label)}</th><td>${escapeHtml(
                value
              )}</td></tr>`
          )
          .join("")}</tbody></table>
        ${downloadLine ? `<p class="page-sub" style="margin-top:8px">${escapeHtml(downloadLine)}</p>` : ""}
        <div style="display:flex;gap:8px;flex-wrap:wrap;margin-top:12px">
          <button class="btn" type="button" data-role="check">检查更新</button>
          ${
            canManage
              ? `<button class="btn" type="button" data-role="download" ${
                  data.update_available && download.state !== "running" ? "" : "disabled"
                }>下载并校验</button>
                 ${
                   download.state === "running"
                     ? `<button class="btn" type="button" data-role="cancel">取消下载</button>`
                     : ""
                 }
                 <button class="btn primary" type="button" data-role="install" ${
                   data.update_available && download.state === "done" ? "" : "disabled"
                 }>安装更新</button>`
              : ""
          }
        </div>`;

      card.querySelector('[data-role="check"]').addEventListener("click", async () => {
        await refresh();
        showNotice("已重新检查更新", "ok");
      });
      const downloadButton = card.querySelector('[data-role="download"]');
      if (downloadButton) {
        downloadButton.addEventListener("click", async () => {
          try {
            await api("POST", "/api/update/download", {});
            showNotice("已开始下载，完成后会自动校验", "ok");
            poll();
          } catch (error) {
            showNotice(error.message, "error");
          }
        });
      }
      const cancelButton = card.querySelector('[data-role="cancel"]');
      if (cancelButton) {
        cancelButton.addEventListener("click", async () => {
          cancelButton.disabled = true;
          try {
            await api("POST", "/api/update/download/cancel", {});
            showNotice("已请求取消下载", "ok");
            poll();
          } catch (error) {
            showNotice(error.message, "error");
            cancelButton.disabled = false;
          }
        });
      }
      const installButton = card.querySelector('[data-role="install"]');
      if (installButton) {
        installButton.addEventListener("click", async () => {
          if (!latest) return;
          const confirmed = await askConfirm(
            `将安装 ${latest.version}（构建 ${latest.build}）。服务会先退出，` +
              "随后自动替换程序并重启；替换失败会回滚旧版本。是否继续？",
            { okText: "安装并重启", cancelText: "取消" }
          );
          if (!confirmed) return;
          try {
            const result = await api("POST", "/api/update/install", { confirm: true });
            installButton.disabled = true;
            showNotice(result.message || "已准备替换，服务即将重启", "ok");
          } catch (error) {
            showNotice(error.message, "error");
          }
        });
      }
    }

    async function refresh() {
      const data = await api("GET", "/api/update/status");
      render(data);
      return data;
    }

    async function poll() {
      if (polling) return;
      polling = true;
      try {
        for (;;) {
          if (!document.body.contains(card)) return;
          const data = await refresh();
          if (!data.download || data.download.state !== "running") return;
          await new Promise((resolve) => setTimeout(resolve, 2000));
        }
      } catch (error) {
        showNotice(error.message, "error");
      } finally {
        polling = false;
      }
    }

    try {
      await refresh();
    } catch (error) {
      card.innerHTML = `<p class="empty">${escapeHtml(error.message)}</p>`;
    }
  }

  // 备份与恢复：列出备份 → 预检查 → 二次确认。
  // 恢复在下次启动、打开数据库之前执行，提交后程序会退出并进入维护态。
