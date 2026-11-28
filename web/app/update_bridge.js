/* app/update_bridge.js：桌面壳更新桥（macOS WKWebView / Windows WebView2）与强制升级拦截。
 */
import { showNotice } from "./core_state.js";
import { askDialog } from "./core_dialog.js";
import { isWinWebview } from "./core_export.js";
import { formatBytes, isWinCheckResult, isWinFailureCode, mandatoryActive, mandatoryReasonText, mapWinCheckResult, normalizeUpdateStatus, parseWebviewMessage, versionWithBuild } from "./update_normalize.js";

  // ---------- 桌面壳更新桥 ----------
  // macOS：WKWebView messageHandlers（方法名 xiaotuUpdateXxx，结果由原生调 window.xxxDone）
  // Windows：WebView2 HostObject updater_win（方法名 checkNow/statusJson…，结果走 chrome.webview
  // 的 message 事件）。两边字段名也不同，这里统一适配成 renderNativeUpdate 期望的形状。
  export let winUpdaterBridgeInstance = null;
  export let winUpdaterListenerInstalled = false;
  export let winUpdaterStatusCache = null;

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
