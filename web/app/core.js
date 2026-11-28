/* app/core.js：录入页未保存守卫、桌面壳状态推送、系统钥匙串与离开确认。
 * 由原 web/app.js 按职责拆出；格式化、弹窗、草稿、请求与导出分别见 core_*.js。
 */
import { askConfirm } from "./core_dialog.js";
import { flushActiveDraft, stopDraftTimer } from "./core_draft.js";
import { pendingWrites } from "./core_api.js";

  // 录入页注册「是否有未保存内容」；只有注册过的页面才会拦导航与关闭窗口。
  export let unsavedGuard = null;
  export function setUnsavedGuard(guard) {
    // 切页时旧录入页已确认离开：先把当前表单补写成本地草稿，再停掉自动保存定时器。
    if (!guard) {
      flushActiveDraft();
      stopDraftTimer();
    }
    unsavedGuard = guard;
  }
  export function hasUnsaved() {
    return Boolean(unsavedGuard && unsavedGuard());
  }

  // 桌面壳退出时要知道「当前有没有未保存内容 / 提交是否在途」，否则会直接退出。
  // 浏览器里没有这个通道，postMessage 不存在时静默跳过。
  // windowShouldClose 必须同步返回，而 evaluateJavaScript 是异步的，所以由页面
  // 主动推送状态，壳只缓存最近一次结果。
  export function notifyShellDirty() {
    const handlers =
      window.webkit && window.webkit.messageHandlers
        ? window.webkit.messageHandlers.xiaotuState
        : null;
    if (!handlers) return;
    const dirty = pendingWrites > 0 || hasUnsaved();
    if (dirty === notifyShellDirty.lastSent) return;
    notifyShellDirty.lastSent = dirty;
    handlers.postMessage({ dirty });
  }
  notifyShellDirty.lastSent = null;

  setInterval(notifyShellDirty, 500);

  // 单机版「本机免输」：桌面壳用 WKScriptMessageHandlerWithReply 提供钥匙串桥，
  // 浏览器里没有这个通道。钥匙串只存服务端签发的随机 token，不存口令。
  export function keychainBridge() {
    return window.webkit && window.webkit.messageHandlers
      ? window.webkit.messageHandlers.xiaotuKeychain
      : null;
  }
  export function keychainAvailable() {
    return Boolean(keychainBridge());
  }
  export async function keychainCall(action, payload) {
    const bridge = keychainBridge();
    if (!bridge) return { ok: false, error: "当前环境不支持系统钥匙串" };
    try {
      const result = await bridge.postMessage(Object.assign({ action }, payload || {}));
      return result || { ok: false, error: "系统钥匙串没有返回结果" };
    } catch (error) {
      return { ok: false, error: error && error.message ? error.message : String(error) };
    }
  }
  export async function saveRememberToken(token) {
    if (!token || !keychainAvailable()) return false;
    const result = await keychainCall("set", { token });
    return result.ok === true;
  }
  export async function clearRememberToken() {
    if (!keychainAvailable()) return;
    await keychainCall("delete");
  }

  export async function confirmLeavePage() {
    if (!hasUnsaved()) return true;
    return askConfirm("当前单据还有未保存的内容，离开后这些内容会丢失。", {
      okText: "离开",
      cancelText: "留在此页",
    });
  }
