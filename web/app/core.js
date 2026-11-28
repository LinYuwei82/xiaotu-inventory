/* app/core.js：共享状态、DOM 引用、请求封装、弹窗与通用格式化；其余模块只从这里取公共能力。
 * 由原 web/app.js 按职责拆出，供本模块与入口共同加载。
 */
import { renderLogin } from "./boot.js";

  export const params = new URLSearchParams(location.search);
  // 开发态：WebView 指向 dev server 时，用 ?api=http://127.0.0.1:<port> 指定后端地址。
  export const API_BASE = params.get("api") || "";
  export const state = {
    current: null,
    pages: [],
    allPages: [],
    csrf: "",
    permissions: [],
    edition: "",
    mode: "",
    localGate: {},
  };

  // 版本能力表由 tools/codegen 从 editions.json 生成，与后端 edition_caps.h 同源；
  // 这里只负责隐藏入口，服务端仍会按同一张表二次拒绝。
  export const EDITION_CAPS = window.XIAOTU_EDITION_CAPS || null;
  export function editionCaps() {
    if (!EDITION_CAPS) return null;
    const code = state.edition || EDITION_CAPS.default;
    return EDITION_CAPS.editions[code] || EDITION_CAPS.editions[EDITION_CAPS.default] || null;
  }
  export function editionAllows(feature) {
    const caps = editionCaps();
    if (!caps || !caps.features) return false;
    const value = caps.features[feature];
    return value === "full" || value === "limited";
  }
  export const main = document.getElementById("main");
  export const nav = document.getElementById("nav");
  export const notice = document.getElementById("notice");

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

  // WKWebView 没有实现 WKUIDelegate 的 confirm 面板，window.confirm 会静默返回
  // false；离开确认改用页内确认层，不依赖浏览器原生对话框。
  export let confirmDialogOpen = false;

  // options: message / okText / cancelText（传 null 表示只有确认按钮）/ withInput / defaultValue
  //          blocking（强制升级用：屏蔽 Esc 与点击遮罩，必须点确认）
  export function askDialog(options) {
    const okText = options.okText || "确定";
    const cancelText = options.cancelText === undefined ? "取消" : options.cancelText;
    const withInput = Boolean(options.withInput);
    const blocking = options.blocking === true;
    if (confirmDialogOpen) return Promise.resolve({ ok: false, value: null });
    confirmDialogOpen = true;
    return new Promise((resolve) => {
      const mask = document.createElement("div");
      mask.className = "confirm-mask";
      const box = document.createElement("div");
      box.className = "confirm-box";
      box.setAttribute("role", "dialog");
      box.setAttribute("aria-modal", "true");
      const text = document.createElement("p");
      text.className = "confirm-text";
      text.textContent = options.message;
      let field = null;
      if (withInput) {
        field = document.createElement("input");
        field.type = "text";
        field.className = "confirm-input";
        field.value =
          options.defaultValue === undefined ? "" : String(options.defaultValue);
      }
      const actions = document.createElement("div");
      actions.className = "confirm-actions";
      const cancel = document.createElement("button");
      cancel.type = "button";
      cancel.className = "btn";
      cancel.textContent = cancelText;
      const ok = document.createElement("button");
      ok.type = "button";
      ok.className = "btn primary";
      ok.textContent = okText;
      if (cancelText !== null) actions.append(cancel);
      actions.append(ok);
      if (field) box.append(text, field, actions);
      else box.append(text, actions);
      mask.append(box);

      const previous = document.activeElement;
      function close(confirmed) {
        document.removeEventListener("keydown", onKey, true);
        mask.remove();
        confirmDialogOpen = false;
        if (previous && previous.focus) previous.focus();
        resolve({ ok: confirmed, value: field ? field.value : null });
      }
      function onKey(event) {
        if (event.key === "Escape") {
          event.preventDefault();
          if (!blocking) close(false);
        } else if (event.key === "Enter") {
          event.preventDefault();
          close(true);
        }
      }
      cancel.addEventListener("click", () => close(false));
      ok.addEventListener("click", () => close(true));
      mask.addEventListener("click", (event) => {
        if (event.target === mask && !blocking) close(false);
      });
      document.addEventListener("keydown", onKey, true);
      document.body.appendChild(mask);
      if (field) {
        field.focus();
        field.select();
      } else {
        ok.focus();
      }
    });
  }
  export async function askConfirm(message, options) {
    const result = await askDialog({
      message,
      okText: options && options.okText,
      cancelText: options && options.cancelText,
    });
    return result.ok;
  }
  export async function askAlert(message) {
    await askDialog({ message, okText: "知道了", cancelText: null });
  }
  export async function askPrompt(message, defaultValue) {
    const result = await askDialog({ message, defaultValue, withInput: true });
    return result.ok ? result.value : null;
  }
  export async function confirmLeavePage() {
    if (!hasUnsaved()) return true;
    return askConfirm("当前单据还有未保存的内容，离开后这些内容会丢失。", {
      okText: "离开",
      cancelText: "留在此页",
    });
  }
  export function escapeHtml(value) {
    return String(value === null || value === undefined ? "" : value).replace(
      /[&<>"']/g,
      (ch) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[ch]
    );
  }

  // 各报表页共用的空行片段。放在模块作用域：经营看板、销售报表、
  // 资金/库存报表都引用它，留在单个页面函数里会导致其余页面 ReferenceError。
  export function emptyRow(text, colspan) {
    return `<tbody><tr><td class="empty" colspan="${colspan}">${text}</td></tr></tbody>`;
  }

  // 商品下拉的选项文案，全站唯一口径：编码 + 空格 + 名称 + 空格 + 规格。
  // 规格为空时自动省略，不留多余空格；同编码不同规格的商品据此区分。
  // 单据明细行、报表筛选、销售报表维度下拉都引用这里，避免各页面自行拼接。
  export function itemOptionText(item) {
    if (!item) return "";
    return [item.code, item.name, item.spec]
      .map((part) => (part === null || part === undefined ? "" : String(part).trim()))
      .filter(Boolean)
      .join(" ");
  }
  export function money(cents) {
    const value = Number(cents || 0);
    return (value / 100).toFixed(2);
  }
  export const QTY_SCALE = 10000n;

  // 定点整数（×10000）→ 十进制文本，去掉末尾 0；全程 BigInt，不做浮点换算。
  // 服务端返回的 qty 已是十进制字符串（如 "20"、"1.25"）；本地计算出的
  // 定点整数（×10000）才走下面的换算。两种来源不能混着除。
  export function qtyText(value) {
    if (typeof value === "string") return value;
    if (value === null || value === undefined || value === "") return "";
    let scaled;
    try {
      scaled = BigInt(value);
    } catch (error) {
      return String(value);
    }
    const negative = scaled < 0n;
    if (negative) scaled = -scaled;
    const whole = scaled / QTY_SCALE;
    const fraction = (scaled % QTY_SCALE)
      .toString()
      .padStart(4, "0")
      .replace(/0+$/, "");
    return (negative ? "-" : "") + whole.toString() + (fraction ? "." + fraction : "");
  }

  // 十进制文本 → 定点整数字符串；最多 4 位小数、不允许负数，非法返回 null。
  // allowZero 为 false 时 0 也视为非法（单据数量必须大于 0，盘点实盘可以为 0）。
  export function parseQty(text, allowZero) {
    const value = String(text === null || text === undefined ? "" : text).trim();
    const match = /^(-?)(\d+)(?:\.(\d+))?$/.exec(value);
    if (!match) return null;
    const fraction = match[3] || "";
    if (fraction.length > 4) return null;
    const whole = match[2].replace(/^0+(?=\d)/, "");
    if (whole.length > 9) return null;
    let scaled = BigInt(whole) * QTY_SCALE + BigInt((fraction + "0000").slice(0, 4));
    if (match[1]) scaled = -scaled;
    if (scaled < 0n) return null;
    if (!allowZero && scaled === 0n) return null;
    return scaled.toString();
  }

  // 金额预览用 BigInt 计算，避免数量 × 单价超过 2^53。
  export function qtyAmountText(qtyScaled, priceCents) {
    if (qtyScaled === null) return "—";
    const numerator = BigInt(qtyScaled) * BigInt(priceCents || 0);
    const scaled =
      numerator >= 0n ? (numerator + QTY_SCALE / 2n) / QTY_SCALE
                      : -((-numerator + QTY_SCALE / 2n) / QTY_SCALE);
    const negative = scaled < 0n;
    const absolute = negative ? -scaled : scaled;
    const fraction = (absolute % 100n).toString().padStart(2, "0");
    return (negative ? "-" : "") + (absolute / 100n).toString() + "." + fraction;
  }

  // 数量（定点整数）× 单价（分）→ 金额（分），与后端 mul_qty_price 同一舍入口径。
  export function qtyAmountCents(qtyScaled, priceCents) {
    if (qtyScaled === null) return 0;
    const numerator = BigInt(qtyScaled) * BigInt(priceCents || 0);
    const scaled =
      numerator >= 0n ? (numerator + QTY_SCALE / 2n) / QTY_SCALE
                      : -((-numerator + QTY_SCALE / 2n) / QTY_SCALE);
    return Number(scaled);
  }

  // 行税额：整数分四舍五入，逐行计算，保证「净额 + 税额 = 价税合计」不差一分。
  export function taxForAmount(amountCents, rateBp, includesTax) {
    if (!rateBp) return { net: amountCents, tax: 0 };
    const divRound = (numerator, denominator) =>
      numerator >= 0
        ? Math.floor((numerator + Math.floor(denominator / 2)) / denominator)
        : -Math.floor((-numerator + Math.floor(denominator / 2)) / denominator);
    if (includesTax) {
      const tax = divRound(amountCents * rateBp, 10000 + rateBp);
      return { net: amountCents - tax, tax };
    }
    return { net: amountCents, tax: divRound(amountCents * rateBp, 10000) };
  }
  export function qtyIsNegative(value) {
    return String(value === null || value === undefined ? "" : value).startsWith("-");
  }
  export function bizCategoryText(value) {
    return (
      {
        GIFT: "赠品",
        USAGE: "领用",
        LOSS: "报损",
        OPENING: "期初",
        OTHER: "其他",
      }[value] || ""
    );
  }
  export function bizTypeText(value) {
    const text = String(value);
    if (text === "REVERSE") return "反审核回冲";
    if (text.endsWith("_REVERSE")) {
      return bizTypeText(text.slice(0, -"_REVERSE".length)) + "回冲";
    }
    return (
      {
        PUR_IN: "采购入库",
        PUR_RET: "采购退货",
        SAL_OUT: "销售出库",
        SAL_RET: "销售退货",
        OTH_IN: "其他入库",
        OTH_OUT: "其他出库",
        STK_TRF: "调拨出库",
        STK_TRF_IN: "调拨入库",
        STK_CNT_IN: "盘盈",
        STK_CNT_OUT: "盘亏",
        STK_CNT: "盘点调整",
        CST_ADJ: "成本调整",
      }[text] || text
    );
  }
  export function showNotice(message, kind) {
    notice.textContent = message;
    notice.className = "notice" + (kind ? " " + kind : "");
    notice.hidden = !message;
  }

  // ---------- 离线提示、统一超时与表单草稿 ----------
  export const API_TIMEOUT_MS = 15000;
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
  export function networkFailureText(timedOut) {
    return timedOut
      ? "请求超过 " + API_TIMEOUT_MS / 1000 + " 秒未响应，请检查网络后重试"
      : "网络连接失败，请检查网络后重试";
  }

  // 前端只负责隐藏入口，真正的拦截在各路由的 guard 里。
  export function can(permission) {
    return (
      state.permissions.indexOf("*") >= 0 || state.permissions.indexOf(permission) >= 0
    );
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
  export function loadScript(src) {
    return new Promise((resolve, reject) => {
      const script = document.createElement("script");
      script.src = src;
      script.onload = resolve;
      script.onerror = () => reject(new Error("加载脚本失败: " + src));
      document.body.appendChild(script);
    });
  }

  // Windows WebView2 壳检测：chrome.webview + hostObjects 存在
  export function isWinWebview() {
    return !!(window.chrome && window.chrome.webview && window.chrome.webview.hostObjects);
  }
  // 用系统默认浏览器打开外链：Windows 走壳的 native_io.openUrl，其余用 window.open
  export function openExternalUrl(url) {
    if (!url) return;
    try {
      if (isWinWebview()) {
        window.chrome.webview.hostObjects.native_io.openUrl(url);
        return;
      }
      window.open(url, "_blank", "noopener");
    } catch (error) {
      showNotice("打开链接失败：" + (error && error.message ? error.message : url), "error");
    }
  }
  // Blob → base64（UTF-8 safe，替代 btoa）
  export function blobToBase64(blob) {
    return new Promise((resolve, reject) => {
      const reader = new FileReader();
      reader.onload = () => resolve(reader.result.split(",")[1]);
      reader.onerror = reject;
      reader.readAsDataURL(blob);
    });
  }

  // 报表 / 对账单共用：浏览器走 Blob 下载，macOS 原生交 WKWebView，Windows WebView2 走原生对话框。
  export async function downloadCsv(path) {
    const response = await fetch(API_BASE + path, { credentials: "include" });
    if (!response.ok) {
      let message = "导出失败（HTTP " + response.status + "）";
      try {
        const payload = await response.json();
        if (payload && payload.message) message = payload.message;
      } catch (error) {
        /* 非 JSON 响应，保留默认提示 */
      }
      throw new Error(message);
    }
    const disposition = response.headers.get("Content-Disposition") || "";
    const match = /filename="([^"]+)"/.exec(disposition);
    const filename = match ? match[1] : "report.csv";
    // response.text() 会丢掉 BOM，这里统一补回，保证 Excel 按 UTF-8 打开。
    const text = await response.text();
    const content = "\uFEFF" + text;
    const handlers = window.webkit && window.webkit.messageHandlers;
    if (handlers && handlers.xiaotuExport) {
      handlers.xiaotuExport.postMessage({ filename, content });
      return;
    }
    if (isWinWebview()) {
      const nativeIo = await window.chrome.webview.hostObjects.native_io;
      const b64 = await blobToBase64(new Blob([content], { type: "text/csv;charset=utf-8" }));
      const ok = await nativeIo.saveFile(filename, b64);
      if (ok === false) return;  // 用户取消保存：静默返回，不弹错误
      return;
    }
    const blob = new Blob([content], { type: "text/csv;charset=utf-8" });
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = url;
    link.download = filename;
    document.body.appendChild(link);
    link.click();
    link.remove();
    URL.revokeObjectURL(url);
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

  // xlsx 是二进制，单机版桥接用 base64 传回原生保存，浏览器走 Blob 下载。
  export function toBase64(bytes) {
    let binary = "";
    const chunk = 0x8000;
    for (let i = 0; i < bytes.length; i += chunk) {
      binary += String.fromCharCode.apply(null, bytes.subarray(i, i + chunk));
    }
    return btoa(binary);
  }
  export async function downloadXlsx(path) {
    const response = await fetch(API_BASE + path, { credentials: "include" });
    if (!response.ok) {
      let message = "导出失败（HTTP " + response.status + "）";
      try {
        const payload = await response.json();
        if (payload && payload.message) message = payload.message;
      } catch (error) {
        /* 非 JSON 响应，保留默认提示 */
      }
      throw new Error(message);
    }
    const disposition = response.headers.get("Content-Disposition") || "";
    const match = /filename="([^"]+)"/.exec(disposition);
    const filename = match ? match[1] : "report.xlsx";
    const bytes = new Uint8Array(await response.arrayBuffer());
    const handlers = window.webkit && window.webkit.messageHandlers;
    if (handlers && handlers.xiaotuExport) {
      handlers.xiaotuExport.postMessage({ filename, contentBase64: toBase64(bytes) });
      return;
    }
    if (isWinWebview()) {
      const nativeIo = await window.chrome.webview.hostObjects.native_io;
      await nativeIo.saveFile(filename, toBase64(bytes));  // 取消保存时静默返回
      return;
    }
    const blob = new Blob([bytes], {
      type: "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
    });
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = url;
    link.download = filename;
    document.body.appendChild(link);
    link.click();
    link.remove();
    URL.revokeObjectURL(url);
  }

  // 文本类导出（错误清单等）：macOS 交原生，Windows WebView2 走原生对话框，浏览器走 Blob。
  export function saveText(filename, text) {
    const content = "\uFEFF" + text;
    const handlers = window.webkit && window.webkit.messageHandlers;
    if (handlers && handlers.xiaotuExport) {
      handlers.xiaotuExport.postMessage({ filename, content });
      return;
    }
    if (isWinWebview()) {
      // hostObjects 下的对象是异步代理，不保证有 .then，统一用 async 函数包一层
      (async () => {
        try {
          const nativeIo = await window.chrome.webview.hostObjects.native_io;
          const b64 = await blobToBase64(
            new Blob([content], { type: "text/plain;charset=utf-8" })
          );
          await nativeIo.saveFile(filename, b64);
        } catch (error) {
          showNotice(error.message, "error");
        }
      })();
      return;
    }
    const blob = new Blob([content], { type: "text/csv;charset=utf-8" });
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = url;
    link.download = filename;
    document.body.appendChild(link);
    link.click();
    link.remove();
    URL.revokeObjectURL(url);
  }
  export function csvField(value) {
    const text = String(value === null || value === undefined ? "" : value);
    return /[",\r\n]/.test(text) ? '"' + text.replace(/"/g, '""') + '"' : text;
  }

  // ---------- 通用 CRUD 页面（供 codegen 生成的页面调用） ----------
