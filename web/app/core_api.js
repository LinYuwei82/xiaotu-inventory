/* app/core_api.js：请求封装（统一超时、失败重试、401 处理）、幂等请求 ID、
 * URL 拼参与已合并档案过滤。
 * 由 core.js 拆出；所有后端读写都从这里出入。
 */
import { renderLogin } from "./boot.js";
import { askConfirm } from "./core_dialog.js";
import { flushActiveDraft, stopDraftTimer } from "./core_draft.js";
import { API_BASE, EDITION_CAPS, state } from "./core_state.js";

  export const API_TIMEOUT_MS = 15000;

  export function networkFailureText(timedOut) {
    return timedOut
      ? "请求超过 " + API_TIMEOUT_MS / 1000 + " 秒未响应，请检查网络后重试"
      : "网络连接失败，请检查网络后重试";
  }

  // 写请求在途计数：桌面壳退出确认要把「提交进行中」也算作未保存。
  export let pendingWrites = 0;
  export async function api(method, path, body) {
    if (method === "GET" || method === "HEAD") {
      return apiRequest(method, path, body);
    }
    pendingWrites += 1;
    try {
      return await apiRequest(method, path, body);
    } finally {
      pendingWrites -= 1;
    }
  }
  export async function apiRequest(method, path, body) {
    const options = { method, credentials: "include", headers: {} };
    if (EDITION_CAPS) {
      options.headers["X-API-Version"] = String(EDITION_CAPS.api_version);
    }
    if (state.csrf && method !== "GET") {
      options.headers["X-CSRF-Token"] = state.csrf;
    }
    if (body !== undefined) {
      options.headers["Content-Type"] = "application/json";
      options.body = JSON.stringify(body);
    }
    // 统一 15 秒超时；超时与网络错误的自动重试原样重发同一个 options，
    // 请求体里的 request_id 不变，幂等键复用，重试不会出第二张单。
    for (;;) {
      const controller = typeof AbortController === "function" ? new AbortController() : null;
      const timer = controller ? setTimeout(() => controller.abort(), API_TIMEOUT_MS) : 0;
      let response = null;
      let payload = null;
      let failure = null;
      try {
        response = await fetch(
          API_BASE + path,
          controller ? { ...options, signal: controller.signal } : options
        );
        try {
          payload = await response.json();
        } catch (error) {
          payload = null;
        }
      } catch (error) {
        failure = error;
      } finally {
        if (timer) clearTimeout(timer);
      }
      if (failure) {
        const timedOut = Boolean(failure && failure.name === "AbortError");
        if (method === "GET") {
          // 读请求由页面自身的刷新按钮重试，避免一次刷新并发多个重试弹层。
          const error = new Error(networkFailureText(timedOut) + "（可点「刷新」重试）");
          error.network = true;
          error.timedOut = timedOut;
          throw error;
        }
        const retry = await askConfirm(networkFailureText(timedOut) + "。是否重试本次提交？", {
          okText: "重试",
          cancelText: "取消",
        });
        if (retry) continue;
        const error = new Error(timedOut ? "请求超时，已取消提交" : "网络连接失败，已取消提交");
        error.network = true;
        error.timedOut = timedOut;
        throw error;
      }
      if (!response.ok) {
        if (
          response.status === 401 &&
          state.current &&
          path !== "/api/login" &&
          path !== "/api/session"
        ) {
          // 会话过期：先把当前表单落到本地，重新登录后可接着填。
          flushActiveDraft();
          stopDraftTimer();
          renderLogin("登录已过期，请重新登录");
        }
        const message = payload && payload.message ? payload.message : "HTTP " + response.status;
        const error = new Error(message);
        error.status = response.status;
        error.code = payload && payload.code;
        throw error;
      }
      if (method === "GET") {
        const plain = path.split("?")[0];
        if (plain === "/api/customers" || plain === "/api/suppliers") {
          return filterMergedTraders(plain, payload);
        }
      }
      return payload;
    }
  }

  // 已合并档案：partner 层记录 merged_into，客户下拉与列表按伙伴 id 隐藏；
  // 供应商旧 id 需经 supplier_ids 映射回档案 id。
  export let mergedTraderCache = null;
  export function invalidateMergedTraders() {
    mergedTraderCache = null;
  }
  export async function loadMergedTraders() {
    if (mergedTraderCache) return mergedTraderCache;
    const customers = new Set();
    const suppliers = new Set();
    try {
      const response = await fetch(API_BASE + "/api/partners", { credentials: "include" });
      if (response.ok) {
        const rows = await response.json();
        for (const row of rows || []) {
          if (!row.merged_into) continue;
          if (row.is_customer) customers.add(Number(row.id));
          for (const legacyId of row.supplier_ids || []) suppliers.add(Number(legacyId));
        }
      }
    } catch (error) {
      // 读取失败时按未合并处理，不阻塞页面。
    }
    mergedTraderCache = { customers, suppliers };
    return mergedTraderCache;
  }
  export async function filterMergedTraders(path, rows) {
    if (!Array.isArray(rows)) return rows;
    const merged = await loadMergedTraders();
    const hidden = path === "/api/suppliers" ? merged.suppliers : merged.customers;
    if (!hidden.size) return rows;
    return rows.filter((row) => !hidden.has(Number(row.id)));
  }

  export function loadScript(src) {
    return new Promise((resolve, reject) => {
      const script = document.createElement("script");
      script.src = src;
      script.onload = resolve;
      script.onerror = () => reject(new Error("加载脚本失败: " + src));
      document.body.appendChild(script);
    });
  }
  export function newRequestId() {
    if (window.crypto && typeof window.crypto.randomUUID === "function") {
      return window.crypto.randomUUID();
    }
    // 单机版走 http 非安全上下文时 randomUUID 不可用，但 getRandomValues 仍可用；
    // 幂等键不能退回 Math.random，否则可被预测撞键。
    if (window.crypto && typeof window.crypto.getRandomValues === "function") {
      const bytes = new Uint8Array(8);
      window.crypto.getRandomValues(bytes);
      let hex = "";
      for (const byte of bytes) hex += byte.toString(16).padStart(2, "0");
      return "rid-" + Date.now().toString(36) + "-" + hex;
    }
    return "rid-" + Date.now().toString(36) + "-" + Math.random().toString(36).slice(2, 10);
  }

  // 打印路径的 target_key=… 已经带 ?，报表查询串也从 ? 开始；直接相加会得到
  // target_key=purchase-detail?from=…，服务端把 "?from" 并进 target_key 后报
  // 「未知的打印目标」。这里统一按 URL 解析：先解析已有查询，再 set 参数，
  // 不再依赖手写 ?/& 的先后顺序。
  export function withQuery(url, query) {
    if (!query) return url;
    const parsed = new URL(url, window.location.origin);
    const extra = new URLSearchParams(String(query).replace(/^\?/, ""));
    extra.forEach((value, key) => parsed.searchParams.set(key, value));
    return parsed.pathname + parsed.search;
  }
