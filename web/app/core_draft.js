/* app/core_draft.js：离线提示条与表单草稿（自动保存、读取、清理）。
 * 由 core.js 拆出；草稿只存业务字段，口令、token、CSRF 一律不进本地存储。
 */
import { main } from "./core_state.js";

  export const DRAFT_INTERVAL_MS = 30000;
  export const DRAFT_PREFIX = "xiaotu.draft.v1.";

  // 断网提示：占正文流常驻在页面顶部，不遮挡操作；恢复联网后自动隐藏。
  // 沿用现有 .notice.warn 配色，不额外引入颜色。
  export let networkBanner = null;
  export function ensureNetworkBanner() {
    if (networkBanner && networkBanner.isConnected) return networkBanner;
    networkBanner = document.createElement("div");
    networkBanner.id = "network-warning";
    networkBanner.className = "notice warn";
    networkBanner.setAttribute("role", "status");
    networkBanner.textContent = "网络已断开，操作可能失败";
    networkBanner.hidden = true;
    main.prepend(networkBanner);
    return networkBanner;
  }
  export function updateNetworkBanner() {
    ensureNetworkBanner().hidden = navigator.onLine !== false;
  }

  window.addEventListener("online", updateNetworkBanner);
  window.addEventListener("offline", updateNetworkBanner);
  updateNetworkBanner();

  // 当前录入页注册的「立即写草稿」回调：切页、关窗、401 跳登录前都先补一次。
  export let draftFlush = null;
  export let draftTimer = 0;
  export function stopDraftTimer() {
    if (draftTimer) {
      clearInterval(draftTimer);
      draftTimer = 0;
    }
    draftFlush = null;
  }
  export function flushActiveDraft() {
    if (draftFlush) draftFlush();
  }
  export function startDraftTimer(flush) {
    stopDraftTimer();
    draftFlush = flush;
    draftTimer = setInterval(flush, DRAFT_INTERVAL_MS);
  }

  // 草稿只存业务字段，按单据类型分开；口令、token、CSRF 一律不进本地存储。
  export function readDraft(docType) {
    try {
      const raw = localStorage.getItem(DRAFT_PREFIX + docType);
      if (!raw) return null;
      const draft = JSON.parse(raw);
      return draft && typeof draft === "object" && draft.form ? draft : null;
    } catch (error) {
      return null;
    }
  }
  export function writeDraft(docType, draft) {
    try {
      localStorage.setItem(DRAFT_PREFIX + docType, JSON.stringify(draft));
    } catch (error) {
      // 本地存储不可用（隐私模式 / 配额满）时不写草稿，不阻塞录入。
    }
  }
  export function clearDraft(docType) {
    try {
      localStorage.removeItem(DRAFT_PREFIX + docType);
    } catch (error) {
      // 删不掉也不影响录入。
    }
  }
