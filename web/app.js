/* 小兔进销存 M0 骨架前端：无构建步骤，直接由内嵌资源或本地 dev server 提供。
 * 表结构驱动的列表页 / 表单页由 tools/codegen 生成到 web/gen/。
 */
(function () {
  "use strict";

  const params = new URLSearchParams(location.search);
  // 开发态：WebView 指向 dev server 时，用 ?api=http://127.0.0.1:<port> 指定后端地址。
  const API_BASE = params.get("api") || "";

  const state = {
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
  const EDITION_CAPS = window.XIAOTU_EDITION_CAPS || null;

  function editionCaps() {
    if (!EDITION_CAPS) return null;
    const code = state.edition || EDITION_CAPS.default;
    return EDITION_CAPS.editions[code] || EDITION_CAPS.editions[EDITION_CAPS.default] || null;
  }

  function editionAllows(feature) {
    const caps = editionCaps();
    if (!caps || !caps.features) return false;
    const value = caps.features[feature];
    return value === "full" || value === "limited";
  }

  const main = document.getElementById("main");
  const nav = document.getElementById("nav");
  const notice = document.getElementById("notice");

  // 录入页注册「是否有未保存内容」；只有注册过的页面才会拦导航与关闭窗口。
  let unsavedGuard = null;

  function setUnsavedGuard(guard) {
    // 切页时旧录入页已确认离开：先把当前表单补写成本地草稿，再停掉自动保存定时器。
    if (!guard) {
      flushActiveDraft();
      stopDraftTimer();
    }
    unsavedGuard = guard;
  }

  function hasUnsaved() {
    return Boolean(unsavedGuard && unsavedGuard());
  }

  // 桌面壳退出时要知道「当前有没有未保存内容 / 提交是否在途」，否则会直接退出。
  // 浏览器里没有这个通道，postMessage 不存在时静默跳过。
  // windowShouldClose 必须同步返回，而 evaluateJavaScript 是异步的，所以由页面
  // 主动推送状态，壳只缓存最近一次结果。
  function notifyShellDirty() {
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
  function keychainBridge() {
    return window.webkit && window.webkit.messageHandlers
      ? window.webkit.messageHandlers.xiaotuKeychain
      : null;
  }

  function keychainAvailable() {
    return Boolean(keychainBridge());
  }

  async function keychainCall(action, payload) {
    const bridge = keychainBridge();
    if (!bridge) return { ok: false, error: "当前环境不支持系统钥匙串" };
    try {
      const result = await bridge.postMessage(Object.assign({ action }, payload || {}));
      return result || { ok: false, error: "系统钥匙串没有返回结果" };
    } catch (error) {
      return { ok: false, error: error && error.message ? error.message : String(error) };
    }
  }

  async function saveRememberToken(token) {
    if (!token || !keychainAvailable()) return false;
    const result = await keychainCall("set", { token });
    return result.ok === true;
  }

  async function clearRememberToken() {
    if (!keychainAvailable()) return;
    await keychainCall("delete");
  }

  // WKWebView 没有实现 WKUIDelegate 的 confirm 面板，window.confirm 会静默返回
  // false；离开确认改用页内确认层，不依赖浏览器原生对话框。
  let confirmDialogOpen = false;

  // options: message / okText / cancelText（传 null 表示只有确认按钮）/ withInput / defaultValue
  //          blocking（强制升级用：屏蔽 Esc 与点击遮罩，必须点确认）
  function askDialog(options) {
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

  async function askConfirm(message, options) {
    const result = await askDialog({
      message,
      okText: options && options.okText,
      cancelText: options && options.cancelText,
    });
    return result.ok;
  }

  async function askAlert(message) {
    await askDialog({ message, okText: "知道了", cancelText: null });
  }

  async function askPrompt(message, defaultValue) {
    const result = await askDialog({ message, defaultValue, withInput: true });
    return result.ok ? result.value : null;
  }

  async function confirmLeavePage() {
    if (!hasUnsaved()) return true;
    return askConfirm("当前单据还有未保存的内容，离开后这些内容会丢失。", {
      okText: "离开",
      cancelText: "留在此页",
    });
  }

  function escapeHtml(value) {
    return String(value === null || value === undefined ? "" : value).replace(
      /[&<>"']/g,
      (ch) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[ch]
    );
  }

  // 各报表页共用的空行片段。放在模块作用域：经营看板、销售报表、
  // 资金/库存报表都引用它，留在单个页面函数里会导致其余页面 ReferenceError。
  function emptyRow(text, colspan) {
    return `<tbody><tr><td class="empty" colspan="${colspan}">${text}</td></tr></tbody>`;
  }

  // 商品下拉的选项文案，全站唯一口径：编码 + 空格 + 名称 + 空格 + 规格。
  // 规格为空时自动省略，不留多余空格；同编码不同规格的商品据此区分。
  // 单据明细行、报表筛选、销售报表维度下拉都引用这里，避免各页面自行拼接。
  function itemOptionText(item) {
    if (!item) return "";
    return [item.code, item.name, item.spec]
      .map((part) => (part === null || part === undefined ? "" : String(part).trim()))
      .filter(Boolean)
      .join(" ");
  }

  function money(cents) {
    const value = Number(cents || 0);
    return (value / 100).toFixed(2);
  }

  const QTY_SCALE = 10000n;

  // 定点整数（×10000）→ 十进制文本，去掉末尾 0；全程 BigInt，不做浮点换算。
  // 服务端返回的 qty 已是十进制字符串（如 "20"、"1.25"）；本地计算出的
  // 定点整数（×10000）才走下面的换算。两种来源不能混着除。
  function qtyText(value) {
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
  function parseQty(text, allowZero) {
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
  function qtyAmountText(qtyScaled, priceCents) {
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
  function qtyAmountCents(qtyScaled, priceCents) {
    if (qtyScaled === null) return 0;
    const numerator = BigInt(qtyScaled) * BigInt(priceCents || 0);
    const scaled =
      numerator >= 0n ? (numerator + QTY_SCALE / 2n) / QTY_SCALE
                      : -((-numerator + QTY_SCALE / 2n) / QTY_SCALE);
    return Number(scaled);
  }

  // 行税额：整数分四舍五入，逐行计算，保证「净额 + 税额 = 价税合计」不差一分。
  function taxForAmount(amountCents, rateBp, includesTax) {
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

  function qtyIsNegative(value) {
    return String(value === null || value === undefined ? "" : value).startsWith("-");
  }

  function bizCategoryText(value) {
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

  function bizTypeText(value) {
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

  function showNotice(message, kind) {
    notice.textContent = message;
    notice.className = "notice" + (kind ? " " + kind : "");
    notice.hidden = !message;
  }

  // ---------- 离线提示、统一超时与表单草稿 ----------

  const API_TIMEOUT_MS = 15000;
  const DRAFT_INTERVAL_MS = 30000;
  const DRAFT_PREFIX = "xiaotu.draft.v1.";

  // 断网提示：占正文流常驻在页面顶部，不遮挡操作；恢复联网后自动隐藏。
  // 沿用现有 .notice.warn 配色，不额外引入颜色。
  let networkBanner = null;

  function ensureNetworkBanner() {
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

  function updateNetworkBanner() {
    ensureNetworkBanner().hidden = navigator.onLine !== false;
  }

  window.addEventListener("online", updateNetworkBanner);
  window.addEventListener("offline", updateNetworkBanner);
  updateNetworkBanner();

  // 当前录入页注册的「立即写草稿」回调：切页、关窗、401 跳登录前都先补一次。
  let draftFlush = null;
  let draftTimer = 0;

  function stopDraftTimer() {
    if (draftTimer) {
      clearInterval(draftTimer);
      draftTimer = 0;
    }
    draftFlush = null;
  }

  function flushActiveDraft() {
    if (draftFlush) draftFlush();
  }

  function startDraftTimer(flush) {
    stopDraftTimer();
    draftFlush = flush;
    draftTimer = setInterval(flush, DRAFT_INTERVAL_MS);
  }

  // 草稿只存业务字段，按单据类型分开；口令、token、CSRF 一律不进本地存储。
  function readDraft(docType) {
    try {
      const raw = localStorage.getItem(DRAFT_PREFIX + docType);
      if (!raw) return null;
      const draft = JSON.parse(raw);
      return draft && typeof draft === "object" && draft.form ? draft : null;
    } catch (error) {
      return null;
    }
  }

  function writeDraft(docType, draft) {
    try {
      localStorage.setItem(DRAFT_PREFIX + docType, JSON.stringify(draft));
    } catch (error) {
      // 本地存储不可用（隐私模式 / 配额满）时不写草稿，不阻塞录入。
    }
  }

  function clearDraft(docType) {
    try {
      localStorage.removeItem(DRAFT_PREFIX + docType);
    } catch (error) {
      // 删不掉也不影响录入。
    }
  }

  function networkFailureText(timedOut) {
    return timedOut
      ? "请求超过 " + API_TIMEOUT_MS / 1000 + " 秒未响应，请检查网络后重试"
      : "网络连接失败，请检查网络后重试";
  }

  // 前端只负责隐藏入口，真正的拦截在各路由的 guard 里。
  function can(permission) {
    return (
      state.permissions.indexOf("*") >= 0 || state.permissions.indexOf(permission) >= 0
    );
  }

  // 已合并档案：partner 层记录 merged_into，客户下拉与列表按伙伴 id 隐藏；
  // 供应商旧 id 需经 supplier_ids 映射回档案 id。
  let mergedTraderCache = null;

  function invalidateMergedTraders() {
    mergedTraderCache = null;
  }

  async function loadMergedTraders() {
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

  async function filterMergedTraders(path, rows) {
    if (!Array.isArray(rows)) return rows;
    const merged = await loadMergedTraders();
    const hidden = path === "/api/suppliers" ? merged.suppliers : merged.customers;
    if (!hidden.size) return rows;
    return rows.filter((row) => !hidden.has(Number(row.id)));
  }

  // 写请求在途计数：桌面壳退出确认要把「提交进行中」也算作未保存。
  let pendingWrites = 0;

  async function api(method, path, body) {
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

  async function apiRequest(method, path, body) {
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

  function loadScript(src) {
    return new Promise((resolve, reject) => {
      const script = document.createElement("script");
      script.src = src;
      script.onload = resolve;
      script.onerror = () => reject(new Error("加载脚本失败: " + src));
      document.body.appendChild(script);
    });
  }

  // Windows WebView2 壳检测：chrome.webview + hostObjects 存在
  function isWinWebview() {
    return !!(window.chrome && window.chrome.webview && window.chrome.webview.hostObjects);
  }
  // Blob → base64（UTF-8 safe，替代 btoa）
  function blobToBase64(blob) {
    return new Promise((resolve, reject) => {
      const reader = new FileReader();
      reader.onload = () => resolve(reader.result.split(",")[1]);
      reader.onerror = reject;
      reader.readAsDataURL(blob);
    });
  }

  // 报表 / 对账单共用：浏览器走 Blob 下载，macOS 原生交 WKWebView，Windows WebView2 走原生对话框。
  async function downloadCsv(path) {
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

  function printPage(layout) {
    const handlers = window.webkit && window.webkit.messageHandlers;
    if (handlers && handlers.xiaotuPrint) {
      handlers.xiaotuPrint.postMessage({ paper: layout || null });
    } else {
      window.print();
    }
  }

  // ---------- 自定义打印模板 ----------

  function printAlign(align) {
    return align === "RIGHT" ? "right" : align === "CENTER" ? "center" : "left";
  }

  function printCellText(value, type) {
    if (value === null || value === undefined) return "";
    // 「—」等占位文本按原样输出，避免被金额格式化吞掉。
    if (type === "MONEY" && typeof value === "number") return money(value);
    return String(value);
  }

  function printTotalsText(totals) {
    if (!totals) return "";
    if (totals.text !== undefined) return String(totals.text);
    if (totals.total_amount_text !== undefined) {
      let text = "合计数量 " + totals.total_qty + "，合计金额 " + totals.total_amount_text + " 元";
      if (totals.total_tax_text !== undefined) {
        text += "\n税额 " + totals.total_tax_text + " 元";
        text += "\n价税合计 " + totals.total_with_tax_text + " 元";
      }
      return text;
    }
    if (totals.closing_text !== undefined) {
      let text = "期初余额 " + totals.opening_text + " 元，期末余额 " + totals.closing_text + " 元";
      if (totals.reverse_text !== undefined) {
        text += "；本期撤销收付款 " + totals.reverse_text;
      }
      if (totals.settle_net_text !== undefined) {
        text += "，期间核销净额 " + totals.settle_net_text;
      }
      return text;
    }
    return "";
  }

  // 模板只提供展示配置，所有数据都按纯文本转义后拼进打印视图。
  function printViewHtml(data) {
    const tmpl = data.template || {};
    const layout = data.layout || {};
    const columns = data.columns || [];
    const scale = Math.max(10, Math.min(100, Number(layout.scale_percent) || 100));
    const tableWidth = Number(layout.table_width_mm) || 0;
    const colgroup = columns
      .map((column) =>
        column.width_mm > 0 ? `<col style="width:${Number(column.width_mm)}mm" />` : "<col />"
      )
      .join("");
    const head = columns
      .map(
        (column) =>
          `<th style="text-align:${printAlign(column.align)}">${escapeHtml(column.label)}</th>`
      )
      .join("");
    const body = (data.rows || []).length
      ? data.rows
          .map(
            (row) =>
              `<tr>${columns
                .map((column) => {
                  const styles = ["text-align:" + printAlign(column.align)];
                  if (column.font_size_pt) styles.push("font-size:" + Number(column.font_size_pt) + "pt");
                  if (column.bold) styles.push("font-weight:600");
                  return `<td style="${styles.join(";")}">${escapeHtml(
                    printCellText(row[column.key], column.type)
                  )}</td>`;
                })
                .join("")}</tr>`
          )
          .join("")
      : `<tr><td colspan="${columns.length || 1}" style="text-align:center">暂无数据</td></tr>`;
    const fields = (data.header_fields || [])
      .map(
        (field) =>
          `<div><strong>${escapeHtml(field.label)}：</strong>${escapeHtml(field.value)}</div>`
      )
      .join("");
    const totals = printTotalsText(data.totals);
    const headerText = data.header_text
      ? `<div class="print-header">${escapeHtml(data.header_text)}</div>`
      : "";
    const footerText = data.footer_text
      ? `<div class="print-footer">${escapeHtml(data.footer_text)}</div>`
      : "";
    return `
      <div class="print-sheet" style="width:${
        tableWidth > 0 ? tableWidth + "mm" : "auto"
      };zoom:${scale / 100};font-size:${Number(tmpl.font_size_pt) || 10}pt">
        ${headerText}
        <h1 class="print-title">${escapeHtml(data.title || "")}</h1>
        <div class="print-subtitle">${escapeHtml(data.subtitle || "")}</div>
        ${fields ? `<div class="print-fields">${fields}</div>` : ""}
        <div class="print-table-wrap">
          <table class="print-table">
            <colgroup>${colgroup}</colgroup>
            <thead><tr>${head}</tr></thead>
            <tbody>${body}</tbody>
          </table>
        </div>
        ${totals ? `<div class="print-totals">${escapeHtml(totals)}</div>` : ""}
        <div class="print-meta">打印时间：${escapeHtml(data.printed_at || "")}</div>
        ${footerText}
      </div>`;
  }

  function printPageCss(tmpl, layout) {
    const size =
      layout && layout.width_mm && layout.height_mm
        ? `${Number(layout.width_mm)}mm ${Number(layout.height_mm)}mm`
        : tmpl.page_size === "CUSTOM"
          ? `${Number(tmpl.paper_width_mm)}mm ${Number(tmpl.paper_height_mm)}mm`
          : tmpl.orientation === "LANDSCAPE"
            ? "297mm 210mm"
            : "210mm 297mm";
    // WKWebView 打印时以 @page margin 为准，NSPrintInfo 的 margin 会被它盖掉；
    // 原生模式若把这里归零，纸边距就只剩 0。三端统一用模板边距。
    const margin = Number(tmpl.margin_mm) || 0;
    return `@page { size: ${size}; margin: ${margin}mm; }`;
  }

  function mountPrintView(data) {
    for (const node of document.querySelectorAll("[data-print-view]")) node.remove();
    const view = document.createElement("section");
    view.className = "print-view";
    view.dataset.printView = "1";
    view.innerHTML = printViewHtml(data);
    document.body.appendChild(view);
    const style = document.createElement("style");
    style.dataset.printView = "1";
    style.textContent = printPageCss(data.template || {}, data.layout || {});
    document.head.appendChild(style);
    document.body.classList.add("print-mode");
    return () => {
      document.body.classList.remove("print-mode");
      view.remove();
      style.remove();
    };
  }

  async function confirmPrintLayout(layout) {
    if (!layout || !layout.warn || layout.warn === "paper_unknown") return true;
    let message = layout.warn_text || "打印内容超出纸张，可能被裁切。";
    if (layout.suggest_landscape) message += "\n\n可把纸张方向改为横向后再试。";
    return askConfirm(message + "\n\n仍要继续吗？", {
      okText: "继续",
      cancelText: "返回调整",
    });
  }

  async function printWithTemplate(data) {
    if (!(await confirmPrintLayout(data.layout))) return;
    const cleanup = mountPrintView(data);
    // 浏览器打印结束（含取消）后清理；单机版桥接不触发时，下次打印会先清掉旧视图。
    window.addEventListener("afterprint", cleanup, { once: true });
    printPage(data.layout);
  }

  function pdfBridge() {
    const handlers = window.webkit && window.webkit.messageHandlers;
    if (handlers && handlers.xiaotuPrintPdf) return handlers.xiaotuPrintPdf;
    // Windows WebView2：mock 一个有 postMessage 的桥，内部用 chrome.webview.postMessage 发到 C++ 壳
    if (isWinWebview()) {
      return {
        postMessage: (payload) => {
          // 必须发字符串：壳端优先按字符串读取，对象消息在部分 WebView2 版本读不到
          window.chrome.webview.postMessage(
            JSON.stringify({ kind: "printPdf", filename: payload && payload.filename })
          );
        },
      };
    }
    return null;
  }

  function pdfFileName(name) {
    const base = String(name || "导出").replace(/[\\/:*?"<>|]/g, "_").trim() || "导出";
    return base + ".pdf";
  }

  // 直接导出 PDF：复用打印视图，交给原生保存面板写文件，不弹系统打印对话框。
  async function exportPdfTarget(path, filename) {
    const bridge = pdfBridge();
    if (!bridge) {
      showNotice("直接导出 PDF 仅在桌面版可用；浏览器请用系统打印另存为 PDF", "error");
      return;
    }
    try {
      const data = await api("GET", path);
      if (!(await confirmPrintLayout(data.layout))) return;
      const cleanup = mountPrintView(data);
      window.xiaotuPdfDone = (result) => {
        cleanup();
        delete window.xiaotuPdfDone;
        if (result === "ok") showNotice("PDF 已导出", "ok");
        else if (result === "error") showNotice("PDF 导出失败", "error");
      };
      bridge.postMessage({ filename: pdfFileName(filename || data.title), paper: data.layout });
    } catch (error) {
      showNotice(error.message, "error");
    }
  }

  async function printTarget(path) {
    try {
      await printWithTemplate(await api("GET", path));
    } catch (error) {
      showNotice(error.message, "error");
    }
  }

  // 新建单据的幂等键：进入新建页生成一个，提交成功后作废，重试复用同一个。
  // 服务端按 request_id 唯一索引返回首次创建的单据，避免双击/断线重试重复建单。
  function newRequestId() {
    if (window.crypto && typeof window.crypto.randomUUID === "function") {
      return window.crypto.randomUUID();
    }
    return "rid-" + Date.now().toString(36) + "-" + Math.random().toString(36).slice(2, 10);
  }

  // 打印路径的 target_key=… 已经带 ?，报表查询串也从 ? 开始；直接相加会得到
  // target_key=purchase-detail?from=…，服务端把 "?from" 并进 target_key 后报
  // 「未知的打印目标」。这里统一按 URL 解析：先解析已有查询，再 set 参数，
  // 不再依赖手写 ?/& 的先后顺序。
  function withQuery(url, query) {
    if (!query) return url;
    const parsed = new URL(url, window.location.origin);
    const extra = new URLSearchParams(String(query).replace(/^\?/, ""));
    extra.forEach((value, key) => parsed.searchParams.set(key, value));
    return parsed.pathname + parsed.search;
  }

  // xlsx 是二进制，单机版桥接用 base64 传回原生保存，浏览器走 Blob 下载。
  function toBase64(bytes) {
    let binary = "";
    const chunk = 0x8000;
    for (let i = 0; i < bytes.length; i += chunk) {
      binary += String.fromCharCode.apply(null, bytes.subarray(i, i + chunk));
    }
    return btoa(binary);
  }

  async function downloadXlsx(path) {
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
  function saveText(filename, text) {
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

  function csvField(value) {
    const text = String(value === null || value === undefined ? "" : value);
    return /[",\r\n]/.test(text) ? '"' + text.replace(/"/g, '""') + '"' : text;
  }

  // ---------- 通用 CRUD 页面（供 codegen 生成的页面调用） ----------

  async function crudPage(mount, def) {
    let rows = [];
    let editingId = 0;
    const optionCache = {};

    const io = def.importable ? def.permissions || {} : null;
    const isTrader = def.key === "customer" || def.key === "supplier";
    const ioButtons = io
      ? `${can(io.read) ? `<button class="btn" data-role="template">下载模板</button>
          <button class="btn" data-role="export-xlsx">导出 Excel</button>
          <button class="btn" data-role="export-csv">导出 CSV</button>` : ""}
        ${can(io.write) ? `<button class="btn" data-role="import">导入</button>` : ""}`
      : "";

    mount.innerHTML = `
      <div class="page-head">
        <div>
          <h2>${escapeHtml(def.title)}</h2>
          <p class="page-sub">${escapeHtml(def.api)}</p>
        </div>
        <div class="page-actions">
          ${ioButtons}
          ${isTrader && can("customer.write") ? '<button class="btn" data-role="merge-open">合并单位</button>' : ""}
          <button class="btn primary" data-role="new">新增</button>
        </div>
      </div>
      <div class="card" data-role="form-card" hidden>
        <form class="grid-form" data-role="form"></form>
        <div class="form-actions">
          <button class="btn primary" type="button" data-role="save">保存</button>
          <button class="btn" type="button" data-role="cancel">取消</button>
        </div>
      </div>
      ${isTrader ? `<div class="card" data-role="merge-card" hidden>
        <div class="card-head">
          <h3>合并往来单位</h3>
          <button class="btn" type="button" data-role="merge-close">关闭</button>
        </div>
        <p class="page-sub" data-role="merge-hint"></p>
        <div data-role="merge-body"></div>
        <div class="card-head"><h3>最近合并与撤销</h3></div>
        <div data-role="merge-log"></div>
      </div>` : ""}
      ${def.filters ? '<div class="card filters" data-role="filters"></div>' : ""}
      <div class="card">
        <table class="table" data-role="table"></table>
      </div>`;

    const formCard = mount.querySelector('[data-role="form-card"]');
    const form = mount.querySelector('[data-role="form"]');
    const table = mount.querySelector('[data-role="table"]');
    const filtersBar = mount.querySelector('[data-role="filters"]');

    function optionLabel(option) {
      if (option.full_name) return option.full_name;
      return [option.code, option.name].filter(Boolean).join(" ") || String(option.id);
    }

    async function renderFilters() {
      if (!def.filters) return;
      const parts = [];
      for (const filter of def.filters) {
        if (filter.optionsApi) {
          const options = await api("GET", filter.optionsApi);
          parts.push(`<label class="field"><span>${escapeHtml(filter.label)}</span>
            <select data-filter="${escapeHtml(filter.key)}">
              <option value="">${escapeHtml(filter.allLabel || "全部")}</option>
              ${filter.zeroLabel ? `<option value="0">${escapeHtml(filter.zeroLabel)}</option>` : ""}
              ${options
                .filter((option) => option.status !== false)
                .map(
                  (option) =>
                    `<option value="${option.id}">${escapeHtml(
                      "　".repeat(Math.max(0, (option.depth || 1) - 1)) + optionLabel(option)
                    )}</option>`
                )
                .join("")}
            </select>
          </label>`);
        } else if (filter.options) {
          parts.push(`<label class="field"><span>${escapeHtml(filter.label)}</span>
            <select data-filter="${escapeHtml(filter.key)}">
              <option value="">${escapeHtml(filter.allLabel || "全部")}</option>
              ${filter.options
                .map(
                  (option) =>
                    `<option value="${escapeHtml(option.value)}">${escapeHtml(option.label)}</option>`
                )
                .join("")}
            </select>
          </label>`);
        } else {
          parts.push(`<label class="field"><span>${escapeHtml(filter.label)}</span>
            <input type="text" data-filter="${escapeHtml(filter.key)}" placeholder="${escapeHtml(
            filter.placeholder || ""
          )}" />
          </label>`);
        }
      }
      parts.push('<button class="btn" type="button" data-role="filter-reset">重置</button>');
      filtersBar.innerHTML = parts.join("");
      filtersBar.addEventListener("change", () => reload());
      filtersBar.querySelector('[data-role="filter-reset"]').addEventListener("click", () => {
        filtersBar.querySelectorAll("[data-filter]").forEach((input) => {
          input.value = "";
        });
        reload();
      });
    }

    function filterQuery() {
      if (!def.filters) return "";
      const params = [];
      for (const filter of def.filters) {
        const input = filtersBar.querySelector('[data-filter="' + filter.key + '"]');
        const value = input ? input.value.trim() : "";
        if (value) params.push(encodeURIComponent(filter.key) + "=" + encodeURIComponent(value));
      }
      return params.length ? "?" + params.join("&") : "";
    }

    function fieldInput(field, value) {
      if (field.type === "bool") {
        return `<label class="checkbox">
            <input type="checkbox" data-field="${escapeHtml(field.key)}" ${
              value ? "checked" : ""
            } /> ${escapeHtml(field.label)}
          </label>`;
      }
      if (field.optionsApi) {
        const options = optionCache[field.optionsApi] || [];
        return `<label class="field">
            <span>${escapeHtml(field.label)}${field.required ? " *" : ""}</span>
            <select data-field="${escapeHtml(field.key)}" ${field.required ? "required" : ""}>
              <option value="0">未指定</option>
              ${options
                .filter((option) => option.status !== false)
                .map(
                  (option) =>
                    `<option value="${option.id}" ${
                      String(option.id) === String(value) ? "selected" : ""
                    }>${escapeHtml(
                      "　".repeat(Math.max(0, (option.depth || 1) - 1)) + optionLabel(option)
                    )}</option>`
                )
                .join("")}
            </select>
          </label>`;
      }
      if (field.type === "int") {
        const scale = field.scale || 1;
        const min = field.min === undefined ? "" : ` min="${Number(field.min) / scale}"`;
        const max = field.max === undefined ? "" : ` max="${Number(field.max) / scale}"`;
        const shown =
          value === undefined || value === null
            ? ""
            : Number(value) / scale;
        return `<label class="field">
          <span>${escapeHtml(field.label)}${field.required ? " *" : ""}</span>
          <input type="number" step="${scale === 1 ? "1" : "0.01"}"${min}${max} data-field="${escapeHtml(
          field.key
        )}" value="${escapeHtml(shown)}" ${field.required ? "required" : ""} />
        </label>`;
      }
      return `<label class="field">
          <span>${escapeHtml(field.label)}${field.required ? " *" : ""}</span>
          <input type="text" data-field="${escapeHtml(field.key)}" value="${escapeHtml(
        value === undefined || value === null ? "" : value
      )}" ${field.required ? "required" : ""} />
        </label>`;
    }

    async function loadOptions() {
      for (const field of def.fields) {
        if (field.optionsApi && !optionCache[field.optionsApi]) {
          optionCache[field.optionsApi] = await api("GET", field.optionsApi);
        }
      }
    }

    function openForm(row) {
      editingId = row ? row.id : 0;
      form.innerHTML =
        def.fields
          .map((field) =>
            fieldInput(field, row ? row[field.key] : field.default)
          )
          .join("") + (def.formExtra ? def.formExtra(row) : "");
      formCard.hidden = false;
      if (def.bindFormExtra) {
        def.bindFormExtra(form, row);
      }
    }

    function readForm() {
      const payload = {};
      for (const field of def.fields) {
        const input = form.querySelector('[data-field="' + field.key + '"]');
        if (field.type === "bool") {
          payload[field.key] = input.checked;
        } else if (field.optionsApi) {
          payload[field.key] = Number(input.value || 0);
        } else if (field.type === "int") {
          payload[field.key] = Math.round(Number(input.value || 0) * (field.scale || 1));
        } else {
          payload[field.key] = input.value.trim();
        }
      }
      return payload;
    }

    function renderTable() {
      if (!rows.length) {
        table.innerHTML = '<tbody><tr><td class="empty">暂无数据，点击「新增」开始</td></tr></tbody>';
        return;
      }
      const head = def.columns
        .map((column) => `<th>${escapeHtml(column.label)}</th>`)
        .join("");
      const body = rows
        .map((row) => {
          const cells = def.columns
            .map((column) => {
              const value = row[column.key];
              if (column.type === "bool") {
                return `<td>${value ? "是" : "否"}</td>`;
              }
              if (column.type === "percent_bp") {
                return `<td>${(Number(value || 0) / 100).toFixed(2)}%</td>`;
              }
              return `<td>${escapeHtml(value)}</td>`;
            })
            .join("");
          return `<tr data-id="${row.id}">${cells}
              <td class="actions">
                <button class="link" data-role="edit">编辑</button>
                <button class="link danger" data-role="delete">删除</button>
              </td>
            </tr>`;
        })
        .join("");
      table.innerHTML = `<thead><tr>${head}<th class="actions">操作</th></tr></thead><tbody>${body}</tbody>`;
    }

    async function reload() {
      rows = await api("GET", def.api + filterQuery());
      renderTable();
    }

    const mergeCard = mount.querySelector('[data-role="merge-card"]');
    const mergeBody = mount.querySelector('[data-role="merge-body"]');
    const mergeHint = mount.querySelector('[data-role="merge-hint"]');
    const mergeLog = mount.querySelector('[data-role="merge-log"]');
    let mergePreview = null;
    let mergePartners = [];
    let mergeLogs = [];

    function mergeOriginLabel(origin) {
      if (origin === "AUTO_ARCHIVE") return "自动补建";
      if (origin === "MANUAL") return "手工/合并";
      return origin || "—";
    }

    function renderMergeLogs() {
      if (!mergeLog) return;
      mergeLog.innerHTML = !mergeLogs.length
        ? '<p class="page-sub">还没有合并记录</p>'
        : `<table class="table"><thead><tr><th>时间</th><th>保留档案</th><th>被合并</th>
             <th>执行人</th><th>状态</th><th>操作</th></tr></thead><tbody>${mergeLogs
             .map(
               (log) => `<tr>
                 <td>${escapeHtml(log.created_at)}</td>
                 <td>${escapeHtml(log.target_code + " " + log.target_name)}</td>
                 <td>${escapeHtml(log.source_ids)}</td>
                 <td>${escapeHtml(log.actor)}</td>
                 <td>${log.undone_at ? "已撤销 " + escapeHtml(log.undone_at) : "已合并"}</td>
                 <td class="actions">${
                   log.undone_at
                     ? '<span class="page-sub">—</span>'
                     : log.can_undo
                       ? `<button class="link" data-role="undo" data-log="${log.id}" data-name="${escapeHtml(log.target_name)}">撤销</button>`
                       : `<span class="page-sub" title="${escapeHtml(log.undo_reason)}">不可撤销</span>`
                 }</td>
               </tr>`
             )
             .join("")}</tbody></table>`;
    }

    async function openMergePanel() {
      if (!mergeCard) return;
      const role = def.key === "customer" ? "customer" : "supplier";
      mergeCard.hidden = false;
      mergeHint.textContent = "正在读取候选与合并记录…";
      try {
        const [partners, preview, logs] = await Promise.all([
          api("GET", "/api/partners?role=" + role),
          api("GET", "/api/partners/merge-preview"),
          api("GET", "/api/partners/merge-log?limit=10"),
        ]);
        mergePreview = preview;
        mergeLogs = logs;
        mergePartners = partners.filter((row) => !row.merged_into && row.status);
        mergeHint.textContent =
          `候选组 ${preview.groups} 组（高置信 ${preview.high}、中高 ${preview.medium}、` +
          `仅名称 ${preview.name_only}）。仅名称相同属低置信，不会自动合并，请逐组核对。`;
        mergeBody.innerHTML = !mergePartners.length
          ? '<p class="page-sub">当前没有可合并的档案</p>'
          : `<table class="table"><thead><tr><th>合并</th><th>保留</th><th>编码</th>
               <th>名称</th><th>来源</th></tr></thead><tbody>${mergePartners
               .map(
                 (row) => `<tr>
                   <td><input type="checkbox" data-merge-source value="${row.id}" /></td>
                   <td><input type="radio" name="merge-target-${def.key}" value="${row.id}" /></td>
                   <td>${escapeHtml(row.code)}</td>
                   <td>${escapeHtml(row.name)}</td>
                   <td>${escapeHtml(mergeOriginLabel(row.origin))}</td>
                 </tr>`
               )
               .join("")}</tbody></table>
             <div class="form-actions">
               <button class="btn primary" type="button" data-role="merge-run">合并所选</button>
               <span class="page-sub">先勾选要并入的档案，再选一条作为保留档案</span>
             </div>`;
        renderMergeLogs();
      } catch (error) {
        mergeHint.textContent = "读取失败：" + error.message;
      }
    }

    if (mergeCard) {
      mergeCard.addEventListener("click", async (event) => {
        const button = event.target.closest("button");
        if (!button) return;
        if (button.dataset.role === "merge-close") {
          mergeCard.hidden = true;
          return;
        }
        if (button.dataset.role === "merge-run") {
          const sources = Array.from(
            mergeBody.querySelectorAll("input[data-merge-source]:checked")
          )
            .map((input) => Number(input.value))
            .filter(Boolean);
          const targetInput = mergeBody.querySelector(
            `input[name="merge-target-${def.key}"]:checked`
          );
          const target = targetInput ? Number(targetInput.value) : 0;
          if (!target) {
            showNotice("请选择保留档案", "error");
            return;
          }
          const selected = sources.filter((id) => id !== target);
          if (!selected.length) {
            showNotice("请至少勾选一条要并入的档案，且不能只选保留档案", "error");
            return;
          }
          const byId = new Map(mergePartners.map((row) => [row.id, row]));
          const names = selected.map((id) => (byId.get(id) || {}).name || "#" + id);
          const targetName = (byId.get(target) || {}).name || "#" + target;
          const candidates = (mergePreview && mergePreview.candidates) || [];
          const matched = candidates.filter((group) =>
            (group.rows || []).some((row) => names.includes(row.name))
          );
          const matchText = matched.length
            ? matched.map((group) => group.match + "：" + group.match_value).join("；")
            : "未命中自动候选，按人工选择合并";
          const ok = await askConfirm(
            `将把以下 ${selected.length} 条档案合并到「${targetName}」：\n` +
              names.join("、") +
              `\n\n预览结果：${matchText}\n\n` +
              "合并后这些档案不再出现在列表与下拉，历史台账、账龄与对账单归到保留档案。确认继续？",
            { okText: "确认合并" }
          );
          if (!ok) return;
          try {
            const result = await api("POST", "/api/partners/merge", {
              target_id: target,
              source_ids: selected,
            });
            invalidateMergedTraders();
            mergeCard.hidden = true;
            showNotice(`已合并 ${selected.length} 条到「${result.target_name || targetName}」`);
            await reload();
          } catch (error) {
            showNotice(error.message, "error");
            await openMergePanel();
          }
          return;
        }
        if (button.dataset.role === "undo") {
          const logId = Number(button.dataset.log);
          const ok = await askConfirm(
            `撤销合并到「${button.dataset.name || ""}」？\n` +
              "仅在合并后没有新业务写入被合并档案时才允许撤销。",
            { okText: "确认撤销" }
          );
          if (!ok) return;
          try {
            await api("POST", "/api/partners/undo", { log_id: logId });
            invalidateMergedTraders();
            mergeCard.hidden = true;
            showNotice("已撤销合并");
            await reload();
          } catch (error) {
            showNotice(error.message, "error");
            await openMergePanel();
          }
        }
      });
      const openButton = mount.querySelector('[data-role="merge-open"]');
      if (openButton) openButton.addEventListener("click", openMergePanel);
    }

    await renderFilters();

    if (io) {
      async function showImportErrors(payload) {
        const errors = payload.errors || [];
        const lines = errors
          .slice(0, 100)
          .map((item) => `第 ${item.row} 行 · ${item.column}：${item.reason}`);
        const more = errors.length > 100 ? `\n…另有 ${errors.length - 100} 条未显示` : "";
        const download = await askConfirm(
          `${payload.message}\n\n${lines.join("\n")}${more}\n\n是否下载完整错误清单？`,
          { okText: "下载清单", cancelText: "关闭" }
        );
        if (!download) return;
        const csv = ["行号,列名,原因"]
          .concat(errors.map((item) => [item.row, item.column, item.reason].map(csvField).join(",")))
          .join("\r\n");
        saveText(def.key + "-import-errors.csv", csv);
      }

      const templateButton = mount.querySelector('[data-role="template"]');
      if (templateButton) {
        templateButton.addEventListener("click", async () => {
          try {
            await downloadXlsx(def.api + "/template");
          } catch (error) {
            showNotice(error.message, "error");
          }
        });
      }
      const exportXlsx = mount.querySelector('[data-role="export-xlsx"]');
      if (exportXlsx) {
        exportXlsx.addEventListener("click", async () => {
          try {
            await downloadXlsx(def.api + "/export?format=xlsx");
          } catch (error) {
            showNotice(error.message, "error");
          }
        });
      }
      const exportCsv = mount.querySelector('[data-role="export-csv"]');
      if (exportCsv) {
        exportCsv.addEventListener("click", async () => {
          try {
            await downloadCsv(def.api + "/export?format=csv");
          } catch (error) {
            showNotice(error.message, "error");
          }
        });
      }

      const importButton = mount.querySelector('[data-role="import"]');
      if (importButton) {
        const fileInput = document.createElement("input");
        fileInput.type = "file";
        fileInput.accept = ".xlsx";
        fileInput.hidden = true;
        mount.append(fileInput);
        // 处理导入主逻辑：接收一个 File 对象
        const runImport = async (file) => {
          const keyLabel = def.key === "item" ? "商品编码" : "编码";
          const confirmed = await askConfirm(
            `将从「${file.name}」导入${def.title}。\n\n` +
              "· 导入前会自动备份数据库，备份失败则中止导入；\n" +
              "· 空单元格一律保持原值，已有记录的「启用」留空不会被改成启用；\n" +
              `· 按${keyLabel}匹配，已存在则更新，不存在则新增；\n` +
              "· 单次最多 5000 行、5MB，任何一行有错误都会整批拒绝。\n\n" +
              "确认开始导入？",
            { okText: "开始导入", cancelText: "取消" }
          );
          if (!confirmed) return;
          importButton.disabled = true;
          importButton.textContent = "导入中…";
          try {
            const headers = {
              "Content-Type":
                "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
              "X-File-Name": encodeURIComponent(file.name),
            };
            if (state.csrf) headers["X-CSRF-Token"] = state.csrf;
            const response = await fetch(API_BASE + def.api + "/import", {
              method: "POST",
              credentials: "include",
              headers,
              body: file,
            });
            let payload = null;
            try {
              payload = await response.json();
            } catch (error) {
              payload = null;
            }
            if (response.ok) {
              showNotice(
                `导入完成：新增 ${payload.inserted} 条，更新 ${payload.updated} 条；` +
                  `导入前已备份为 ${payload.backup}。`
              );
              await reload();
              return;
            }
            if (payload && payload.errors && payload.errors.length) {
              await showImportErrors(payload);
              return;
            }
            throw new Error(
              payload && payload.message ? payload.message : "导入失败（HTTP " + response.status + "）"
            );
          } catch (error) {
            showNotice(error.message, "error");
          } finally {
            importButton.disabled = false;
            importButton.textContent = "导入";
          }
        };
        importButton.addEventListener("click", async () => {
          // Windows WebView2：走原生打开对话框
          if (isWinWebview()) {
            try {
              const nativeIo = await window.chrome.webview.hostObjects.native_io;
              const resultJson = await nativeIo.openFile("Excel 文件", "*.xlsx");
              const result = JSON.parse(resultJson);
              if (!result.ok) return;  // 用户取消
              // base64 → Blob → File（保留原文件名）
              const binary = atob(result.base64);
              const bytes = new Uint8Array(binary.length);
              for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
              const file = new File([bytes], result.name, {
                type: "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
              });
              await runImport(file);
            } catch (error) {
              showNotice(error.message, "error");
            }
            return;
          }
          fileInput.value = "";
          fileInput.click();
        });
        fileInput.addEventListener("change", async () => {
          const file = fileInput.files && fileInput.files[0];
          if (!file) return;
          await runImport(file);
        });
      }
    }

    mount.querySelector('[data-role="new"]').addEventListener("click", async () => {
      await loadOptions();
      openForm(null);
    });
    mount.querySelector('[data-role="cancel"]').addEventListener("click", () => {
      formCard.hidden = true;
    });
    mount.querySelector('[data-role="save"]').addEventListener("click", async () => {
      const payload = readForm();
      for (const field of def.fields) {
        if (field.required && !payload[field.key]) {
          showNotice(field.label + "不能为空", "error");
          return;
        }
        if (field.type === "int") {
          const value = payload[field.key];
          const tooSmall = field.min !== undefined && value < field.min;
          const tooLarge = field.max !== undefined && value > field.max;
          if (!Number.isInteger(value) || tooSmall || tooLarge) {
            const range =
              field.min !== undefined && field.max !== undefined
                ? "应在 " + field.min + "–" + field.max + " 之间"
                : "应为整数";
            showNotice(field.label + range, "error");
            return;
          }
        }
      }
      if (def.beforeSave) {
        const problem = def.beforeSave(payload);
        if (problem) {
          showNotice(problem, "error");
          return;
        }
      }
      try {
        let saved = null;
        if (editingId) {
          saved = await api("PUT", def.api + "/" + editingId, payload);
        } else {
          saved = await api("POST", def.api, payload);
        }
        if (def.afterSave) {
          await def.afterSave(saved, payload, editingId);
        }
        formCard.hidden = true;
        showNotice("");
        await reload();
      } catch (error) {
        showNotice(error.message, "error");
      }
    });

    table.addEventListener("click", async (event) => {
      const button = event.target.closest("button");
      if (!button) return;
      const row = event.target.closest("tr");
      const id = Number(row.dataset.id);
      if (button.dataset.role === "edit") {
        await loadOptions();
        openForm(rows.find((item) => item.id === id));
      } else if (button.dataset.role === "delete") {
        if (!(await askConfirm("确认删除这条记录？"))) return;
        try {
          await api("DELETE", def.api + "/" + id);
          await reload();
        } catch (error) {
          showNotice(error.message, "error");
        }
      }
    });

    await reload();
  }

  // 商品页在通用 CRUD 之上补分类/条码列与筛选；税率按百分数录入，提交时换回整数万分比。
  // 条码支持一商品多码：表单里逐行维护，主码排第一并同步到 item.barcode。
  function itemPage(mount, def) {
    const fields = def.fields
      .filter((field) => field.key !== "barcode")
      .map((field) =>
        field.key === "tax_rate_bp" ? { ...field, label: "税率（%）", scale: 100 } : field
      );
    let pendingBarcodes = [];

    function barcodeRowHtml(code, primary) {
      return `<div class="barcode-row" data-role="barcode-row">
          <input type="text" data-code value="${escapeHtml(code || "")}" placeholder="条码" />
          <label class="inline-check"><input type="radio" name="item-primary-barcode" data-primary ${primary ? "checked" : ""} /> 主码</label>
          <button class="link danger" type="button" data-role="remove-barcode">删除</button>
        </div>`;
    }

    function readBarcodes(form) {
      const rows = Array.from(form.querySelectorAll('[data-role="barcode-row"]'));
      const codes = [];
      let primary = -1;
      for (const row of rows) {
        const value = row.querySelector("[data-code]").value.trim();
        if (!value) continue;
        if (row.querySelector("[data-primary]").checked && primary < 0) {
          primary = codes.length;
        }
        codes.push(value);
      }
      if (primary > 0) {
        codes.unshift(codes.splice(primary, 1)[0]);
      }
      return codes;
    }

    return crudPage(mount, {
      ...def,
      columns: [
        { key: "code", label: "编码", type: "string" },
        { key: "name", label: "名称", type: "string" },
        { key: "spec", label: "规格", type: "string" },
        { key: "category_name", label: "分类", type: "string" },
        { key: "barcode", label: "主条码", type: "string" },
        { key: "unit", label: "单位", type: "string" },
        { key: "tax_rate_bp", label: "税率", type: "percent_bp" },
        { key: "status", label: "启用", type: "bool" },
      ],
      fields,
      formExtra() {
        return `<div class="field" data-role="barcode-editor">
            <span>条码（主码 + 备用码）</span>
            <div data-role="barcode-rows"></div>
            <button class="btn" type="button" data-role="add-barcode">添加条码</button>
            <span class="page-sub">选中「主码」的那一行会同步到商品条码字段；扫码可命中任意一个码。</span>
          </div>`;
      },
      bindFormExtra(form, row) {
        const rows = form.querySelector('[data-role="barcode-rows"]');
        const codes =
          row && Array.isArray(row.barcodes) && row.barcodes.length
            ? row.barcodes
            : [row && row.barcode ? row.barcode : ""];
        rows.innerHTML = codes
          .map((code, index) => barcodeRowHtml(code, index === 0))
          .join("");
        form.querySelector('[data-role="add-barcode"]').addEventListener("click", () => {
          rows.insertAdjacentHTML("beforeend", barcodeRowHtml("", false));
        });
        rows.addEventListener("click", (event) => {
          const button = event.target.closest('[data-role="remove-barcode"]');
          if (!button) return;
          const rowElement = button.closest('[data-role="barcode-row"]');
          if (rows.querySelectorAll('[data-role="barcode-row"]').length <= 1) {
            rowElement.querySelector("[data-code]").value = "";
            return;
          }
          rowElement.remove();
        });
      },
      beforeSave(payload) {
        const codes = readBarcodes(mount.querySelector('[data-role="form"]'));
        const seen = new Set();
        for (const code of codes) {
          if (seen.has(code)) {
            return "条码重复：" + code;
          }
          seen.add(code);
        }
        pendingBarcodes = codes;
        payload.barcode = codes[0] || "";
        return "";
      },
      async afterSave(saved) {
        if (!saved || !saved.id) return;
        await api("PUT", "/api/items/" + saved.id + "/barcodes", {
          barcodes: pendingBarcodes,
        });
      },
      filters: [
        {
          key: "category_id",
          label: "分类",
          optionsApi: "/api/item-categories",
          allLabel: "全部分类",
          zeroLabel: "未分类",
        },
        { key: "q", label: "搜索", placeholder: "编码 / 名称 / 条码" },
      ],
    });
  }

  // 商品分类树：数据库不限层，界面按产品约定只允许建到第三层。
  async function itemCategoryPage(mount) {
    let rows = [];
    let editingId = 0;
    const writable = can("item.write");

    mount.innerHTML = `
      <div class="page-head">
        <div>
          <h2>商品分类</h2>
          <p class="page-sub">最多三层；停用分类不再出现在新建商品的下拉中，已归属的商品照常显示</p>
        </div>
        <div class="page-actions">
          <button class="btn" data-role="refresh">刷新</button>
          <button class="btn primary" data-role="new" ${writable ? "" : "hidden"}>新增一级分类</button>
        </div>
      </div>
      <div class="card" data-role="form-card" hidden>
        <form class="grid-form" data-role="form">
          <label class="field"><span>上级分类</span><select data-role="parent"></select></label>
          <label class="field"><span>分类名称 *</span><input type="text" data-role="name" required /></label>
          <label class="field"><span>排序</span><input type="number" step="1" data-role="sort" value="0" /></label>
          <label class="checkbox"><input type="checkbox" data-role="status" checked /> 启用</label>
        </form>
        <div class="form-actions">
          <button class="btn primary" type="button" data-role="save">保存</button>
          <button class="btn" type="button" data-role="cancel">取消</button>
        </div>
      </div>
      <div class="card"><table class="table" data-role="table"></table></div>`;

    const formCard = mount.querySelector('[data-role="form-card"]');
    const form = mount.querySelector('[data-role="form"]');
    const parentSelect = mount.querySelector('[data-role="parent"]');
    const nameInput = mount.querySelector('[data-role="name"]');
    const sortInput = mount.querySelector('[data-role="sort"]');
    const statusInput = mount.querySelector('[data-role="status"]');
    const table = mount.querySelector('[data-role="table"]');

    function renderTable() {
      if (!rows.length) {
        table.innerHTML = '<tbody><tr><td class="empty">还没有分类，先建一个一级分类</td></tr></tbody>';
        return;
      }
      table.innerHTML = `<thead><tr><th>分类名称</th><th>商品数</th><th>排序</th><th>状态</th><th class="actions">操作</th></tr></thead>
        <tbody>${rows
          .map((row) => {
            const indent = "　".repeat(Math.max(0, row.depth - 1));
            return `<tr data-id="${row.id}">
              <td>${indent}${escapeHtml(row.name)}</td>
              <td>${row.item_count}</td>
              <td>${row.sort_order}</td>
              <td>${row.status ? "启用" : "停用"}</td>
              <td class="actions">
                ${
                  writable && row.depth < 3
                    ? '<button class="link" data-role="add-child">新增子分类</button>'
                    : ""
                }
                ${writable ? '<button class="link" data-role="edit">编辑</button>' : ""}
                ${writable ? '<button class="link danger" data-role="delete">删除</button>' : ""}
              </td>
            </tr>`;
          })
          .join("")}</tbody>`;
    }

    function openForm(row, parentId) {
      editingId = row ? row.id : 0;
      // 第三层之下不再提供新建入口；编辑时保留当前上级，避免被下拉截断。
      const parents = rows.filter(
        (item) =>
          item.depth < 3 &&
          (!row || (item.id !== row.id && !item.path.startsWith(row.path)))
      );
      parentSelect.innerHTML =
        `<option value="0">一级分类</option>` +
        parents
          .map(
            (item) =>
              `<option value="${item.id}">${"　".repeat(item.depth - 1)}${escapeHtml(item.name)}</option>`
          )
          .join("");
      if (row) {
        parentSelect.value = String(row.parent_id);
        // 当前上级若因层级限制不在列表里，补一个选项，避免保存时被改到根。
        if (!parents.some((item) => item.id === row.parent_id) && row.parent_id !== 0) {
          parentSelect.insertAdjacentHTML(
            "beforeend",
            `<option value="${row.parent_id}">当前上级</option>`
          );
          parentSelect.value = String(row.parent_id);
        }
      } else {
        parentSelect.value = String(parentId || 0);
      }
      nameInput.value = row ? row.name : "";
      sortInput.value = row ? row.sort_order : 0;
      statusInput.checked = row ? row.status : true;
      formCard.hidden = false;
      nameInput.focus();
    }

    async function reload() {
      rows = await api("GET", "/api/item-categories");
      renderTable();
    }

    mount.querySelector('[data-role="new"]').addEventListener("click", () => openForm(null));
    mount.querySelector('[data-role="refresh"]').addEventListener("click", () => reload());
    mount.querySelector('[data-role="cancel"]').addEventListener("click", () => {
      formCard.hidden = true;
    });
    mount.querySelector('[data-role="save"]').addEventListener("click", async () => {
      const payload = {
        parent_id: Number(parentSelect.value || 0),
        name: nameInput.value.trim(),
        sort_order: Number(sortInput.value || 0),
        status: statusInput.checked,
      };
      if (!payload.name) {
        showNotice("分类名称不能为空", "error");
        return;
      }
      try {
        if (editingId) {
          await api("PUT", "/api/item-categories/" + editingId, payload);
        } else {
          await api("POST", "/api/item-categories", payload);
        }
        formCard.hidden = true;
        showNotice("");
        await reload();
      } catch (error) {
        showNotice(error.message, "error");
      }
    });

    table.addEventListener("click", async (event) => {
      const button = event.target.closest("button");
      if (!button) return;
      const tr = event.target.closest("tr");
      const id = Number(tr.dataset.id);
      const row = rows.find((item) => item.id === id);
      if (!row) return;
      if (button.dataset.role === "edit") {
        openForm(row);
      } else if (button.dataset.role === "add-child") {
        openForm(null, row.id);
      } else if (button.dataset.role === "delete") {
        if (!(await askConfirm("确认删除分类「" + row.name + "」？"))) return;
        try {
          await api("DELETE", "/api/item-categories/" + id);
          await reload();
        } catch (error) {
          showNotice(error.message, "error");
        }
      }
    });

    await reload();
  }

  // 期初导入：库存 / 应收 / 应付 / 资金账户四类，有业务流水后服务端会拒绝。
  async function openingPage(mount) {
    let status = [];
    mount.innerHTML = `
      <div class="page-head">
        <div>
          <h2>期初导入</h2>
          <p class="page-sub">只做库存、应收、应付、资金账户四类期初；有业务流水后不再允许导入，需要重来须清空数据目录重建</p>
        </div>
        <button class="btn" data-role="refresh">刷新</button>
      </div>
      <div data-role="list"></div>`;
    const list = mount.querySelector('[data-role="list"]');

    function render() {
      list.innerHTML = status
        .map(
          (item) => `<div class="card">
            <div class="card-head">
              <h3>${escapeHtml(item.label)}</h3>
              <span class="page-sub">${item.allowed ? "可导入" : escapeHtml(item.reason)}</span>
            </div>
            <div class="form-actions">
              <button class="btn" data-role="template" data-kind="${escapeHtml(item.kind)}">下载模板</button>
              <button class="btn primary" data-role="import" data-kind="${escapeHtml(item.kind)}"
                ${item.allowed ? "" : "disabled"}>导入</button>
            </div>
          </div>`
        )
        .join("");
    }

    async function reload() {
      status = await api("GET", "/api/openings/status");
      render();
    }

    list.addEventListener("click", async (event) => {
      const button = event.target.closest("button");
      if (!button) return;
      const kind = button.dataset.kind;
      if (button.dataset.role === "template") {
        try {
          await downloadXlsx("/api/openings/" + kind + "/template");
        } catch (error) {
          showNotice(error.message, "error");
        }
        return;
      }
      if (button.dataset.role === "import") {
        // 共用导入处理函数：接收一个 File 对象
        const runOpeningImport = async (file) => {
          const confirmed = await askConfirm(
            `将从「${file.name}」导入期初。\n\n` +
              "· 导入前会自动备份数据库，备份失败则中止；\n" +
              "· 整批校验，任何一行有错误都不会写入；\n" +
              "· 库存期初会生成「其他入库 + 期初」并直接审核。\n\n确认开始导入？",
            { okText: "开始导入", cancelText: "取消" }
          );
          if (!confirmed) return;
          try {
            const headers = {
              "Content-Type":
                "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
              "X-File-Name": encodeURIComponent(file.name),
            };
            if (state.csrf) headers["X-CSRF-Token"] = state.csrf;
            const response = await fetch(API_BASE + "/api/openings/" + kind + "/import", {
              method: "POST",
              credentials: "include",
              headers,
              body: file,
            });
            let payload = null;
            try {
              payload = await response.json();
            } catch (error) {
              payload = null;
            }
            if (response.ok) {
              const docs = (payload.doc_nos || []).join("、");
              showNotice(
                `导入完成：写入 ${payload.inserted} 条；导入前已备份为 ${payload.backup}。` +
                  (docs ? "期初单据：" + docs : "")
              );
              await reload();
              return;
            }
            if (payload && payload.errors && payload.errors.length) {
              const lines = payload.errors
                .slice(0, 100)
                .map((item) => `第 ${item.row} 行 · ${item.column}：${item.reason}`);
              await askAlert(payload.message + "\n\n" + lines.join("\n"));
              return;
            }
            throw new Error(
              payload && payload.message ? payload.message : "导入失败（HTTP " + response.status + "）"
            );
          } catch (error) {
            showNotice(error.message, "error");
          }
        };
        // Windows WebView2：走原生打开对话框
        if (isWinWebview()) {
          try {
            const nativeIo = await window.chrome.webview.hostObjects.native_io;
            const resultJson = await nativeIo.openFile("Excel 文件", "*.xlsx");
            const result = JSON.parse(resultJson);
            if (!result.ok) return;
            const binary = atob(result.base64);
            const bytes = new Uint8Array(binary.length);
            for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
            const file = new File([bytes], result.name, {
              type: "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
            });
            await runOpeningImport(file);
          } catch (error) {
            showNotice(error.message, "error");
          }
          return;
        }
        const fileInput = document.createElement("input");
        fileInput.type = "file";
        fileInput.accept = ".xlsx";
        fileInput.hidden = true;
        mount.append(fileInput);
        fileInput.addEventListener("change", async () => {
          const file = fileInput.files && fileInput.files[0];
          fileInput.remove();
          if (!file) return;
          await runOpeningImport(file);
        });
        fileInput.click();
      }
    });

    mount.querySelector('[data-role="refresh"]').addEventListener("click", () => reload());
    await reload();
  }

  // ---------- 六类单据共用的列表：筛选条 + 服务端分页 + 服务端合计 ----------

  const DOC_FILTER_PREFIX = "xt.docFilter.";
  const DOC_FILTER_STATUS = ["", "draft", "approved", "reversed"];

  function isoDateOrEmpty(value) {
    return typeof value === "string" && /^\d{4}-\d{2}-\d{2}$/.test(value) ? value : "";
  }

  // 读取上次条件：字段类型不对就整条丢弃，按默认条件（本月）处理。
  function readSavedDocFilter(docType) {
    let data = null;
    try {
      const raw = localStorage.getItem(DOC_FILTER_PREFIX + docType);
      if (!raw) return null;
      data = JSON.parse(raw);
    } catch (error) {
      return null;
    }
    if (!data || typeof data !== "object" || Array.isArray(data)) return null;
    return {
      keyword: typeof data.keyword === "string" ? data.keyword : "",
      date_from: isoDateOrEmpty(data.date_from),
      date_to: isoDateOrEmpty(data.date_to),
      status: DOC_FILTER_STATUS.includes(data.status) ? data.status : "",
      amount_min: typeof data.amount_min === "string" ? data.amount_min : "",
      amount_max: typeof data.amount_max === "string" ? data.amount_max : "",
      amount_open: data.amount_open === true,
    };
  }

  function writeSavedDocFilter(docType, filter) {
    try {
      localStorage.setItem(DOC_FILTER_PREFIX + docType, JSON.stringify(filter));
    } catch (error) {
      // 本地存储不可用时不记条件，不影响查询。
    }
  }

  function defaultDocFilter() {
    const now = new Date();
    const pad = (value) => String(value).padStart(2, "0");
    const month = now.getFullYear() + "-" + pad(now.getMonth() + 1);
    return {
      keyword: "",
      date_from: month + "-01",
      date_to:
        now.getFullYear() + "-" + pad(now.getMonth() + 1) + "-" + pad(now.getDate()),
      status: "",
      amount_min: "",
      amount_max: "",
      amount_open: false,
    };
  }

  function documentAmountValid(value) {
    return value === "" || /^\d+(\.\d{1,2})?$/.test(value);
  }

  // columns: [{title, cell(doc), actions?}]；返回表格 DOM 与可按 id 取行的接口。
  function createDocumentList(options) {
    const docType = options.docType;
    const mount = options.mount;
    const columns = options.columns;
    const emptyText = options.emptyText || "没有符合条件的单据";
    const pageSize = options.pageSize || 20;
    let filter = readSavedDocFilter(docType) || defaultDocFilter();
    let rows = [];
    let totalCount = 0;
    let totalAmountCents = 0;
    let page = 1;
    let loading = false;
    let requestSeq = 0;

    mount.innerHTML = `
      <form class="filters" data-role="doc-filters">
        <label class="field"><span>关键词</span>
          <input type="text" data-field="keyword" placeholder="单号 / 往来单位 / 业务员" />
        </label>
        <label class="field"><span>开始日期</span><input type="date" data-field="date-from" /></label>
        <label class="field"><span>结束日期</span><input type="date" data-field="date-to" /></label>
        <label class="field"><span>状态</span>
          <select data-field="status">
            <option value="">全部</option>
            <option value="draft">草稿</option>
            <option value="approved">已审核</option>
            <option value="reversed">已反审核</option>
          </select>
        </label>
        <div class="form-actions">
          <button class="btn" type="button" data-role="amount-toggle">金额</button>
          <button class="btn primary" type="submit">查询</button>
          <button class="btn" type="button" data-role="filter-reset">重置</button>
        </div>
      </form>
      <form class="filters" data-role="amount-fields" hidden>
        <label class="field"><span>金额下限（元）</span>
          <input type="text" inputmode="decimal" data-field="amount-min" placeholder="0.00" />
        </label>
        <label class="field"><span>金额上限（元）</span>
          <input type="text" inputmode="decimal" data-field="amount-max" placeholder="不限" />
        </label>
      </form>
      <p class="page-sub" data-role="doc-summary"></p>
      <table class="table" data-role="documents"></table>
      <div class="form-actions">
        <button class="btn" type="button" data-role="prev">上一页</button>
        <span class="page-sub" data-role="doc-page"></span>
        <button class="btn" type="button" data-role="next">下一页</button>
      </div>`;

    const filters = mount.querySelector('[data-role="doc-filters"]');
    const amountFields = mount.querySelector('[data-role="amount-fields"]');
    const amountToggle = mount.querySelector('[data-role="amount-toggle"]');
    const keywordInput = filters.querySelector('[data-field="keyword"]');
    const dateFromInput = filters.querySelector('[data-field="date-from"]');
    const dateToInput = filters.querySelector('[data-field="date-to"]');
    const statusField = filters.querySelector('[data-field="status"]');
    const amountMinInput = amountFields.querySelector('[data-field="amount-min"]');
    const amountMaxInput = amountFields.querySelector('[data-field="amount-max"]');
    const summary = mount.querySelector('[data-role="doc-summary"]');
    const table = mount.querySelector('[data-role="documents"]');
    const pageInfo = mount.querySelector('[data-role="doc-page"]');
    const prevButton = mount.querySelector('[data-role="prev"]');
    const nextButton = mount.querySelector('[data-role="next"]');

    function syncForm() {
      keywordInput.value = filter.keyword;
      dateFromInput.value = filter.date_from;
      dateToInput.value = filter.date_to;
      statusField.value = filter.status;
      amountMinInput.value = filter.amount_min;
      amountMaxInput.value = filter.amount_max;
      amountFields.hidden = !filter.amount_open;
      amountToggle.textContent = filter.amount_open ? "收起金额" : "金额";
    }

    // 从表单收集条件；金额只在展开时生效，收起时一律按清空处理。
    function collectFilter() {
      const next = {
        keyword: keywordInput.value.trim(),
        date_from: dateFromInput.value,
        date_to: dateToInput.value,
        status: DOC_FILTER_STATUS.includes(statusField.value) ? statusField.value : "",
        amount_min: "",
        amount_max: "",
        amount_open: !amountFields.hidden,
      };
      if (next.amount_open) {
        next.amount_min = amountMinInput.value.trim();
        next.amount_max = amountMaxInput.value.trim();
        if (!documentAmountValid(next.amount_min) || !documentAmountValid(next.amount_max)) {
          showNotice("金额最多两位小数，请检查金额区间", "error");
          return null;
        }
      }
      return next;
    }

    async function fetchPage() {
      const seq = ++requestSeq;
      loading = true;
      const params = new URLSearchParams();
      params.set("doc_type", docType);
      params.set("page", String(page));
      params.set("page_size", String(pageSize));
      params.set("keyword", filter.keyword);
      params.set("date_from", filter.date_from);
      params.set("date_to", filter.date_to);
      params.set("status", filter.status);
      params.set("amount_min", filter.amount_open ? filter.amount_min : "");
      params.set("amount_max", filter.amount_open ? filter.amount_max : "");
      try {
        const result = await api("GET", "/api/documents?" + params.toString());
        if (seq !== requestSeq) return;
        rows = result && Array.isArray(result.rows) ? result.rows : [];
        totalCount = Number(result && result.total_count) || 0;
        totalAmountCents = Number(result && result.total_amount_cents) || 0;
        page = Number(result && result.page) || page;
        render();
      } catch (error) {
        if (seq === requestSeq) showNotice(error.message, "error");
      } finally {
        if (seq === requestSeq) loading = false;
      }
    }

    async function reload() {
      const next = collectFilter();
      if (!next) return;
      filter = next;
      writeSavedDocFilter(docType, filter);
      await fetchPage();
    }

    function render() {
      const pages = Math.max(1, Math.ceil(totalCount / pageSize));
      page = Math.min(Math.max(1, page), pages);
      summary.textContent =
        "共 " + totalCount + " 张，合计金额 " + money(totalAmountCents) + " 元";
      pageInfo.textContent = "第 " + page + " / " + pages + " 页";
      prevButton.disabled = page <= 1;
      nextButton.disabled = page >= pages;
      if (!rows.length) {
        table.innerHTML = `<tbody><tr><td class="empty">${escapeHtml(emptyText)}</td></tr></tbody>`;
        return;
      }
      table.innerHTML = `
        <thead><tr>${columns
          .map((column) => `<th${column.actions ? ' class="actions"' : ""}>${escapeHtml(column.title)}</th>`)
          .join("")}</tr></thead>
        <tbody>${rows
          .map(
            (doc) => `<tr data-id="${doc.id}">${columns
              .map(
                (column) =>
                  `<td${column.actions ? ' class="actions"' : ""}>${column.cell(doc)}</td>`
              )
              .join("")}</tr>`
          )
          .join("")}</tbody>`;
    }

    syncForm();

    filters.addEventListener("submit", (event) => {
      event.preventDefault();
      page = 1;
      reload();
    });
    const onFilterChange = () => {
      page = 1;
      reload();
    };
    keywordInput.addEventListener("change", onFilterChange);
    dateFromInput.addEventListener("change", onFilterChange);
    dateToInput.addEventListener("change", onFilterChange);
    statusField.addEventListener("change", onFilterChange);
    amountMinInput.addEventListener("change", () => {
      if (!amountFields.hidden) onFilterChange();
    });
    amountMaxInput.addEventListener("change", () => {
      if (!amountFields.hidden) onFilterChange();
    });
    filters.querySelector('[data-role="filter-reset"]').addEventListener("click", () => {
      filter = defaultDocFilter();
      page = 1;
      syncForm();
      writeSavedDocFilter(docType, filter);
      fetchPage();
    });
    amountToggle.addEventListener("click", () => {
      if (amountFields.hidden) {
        amountFields.hidden = false;
        amountToggle.textContent = "收起金额";
        filter = { ...filter, amount_open: true };
        writeSavedDocFilter(docType, filter);
        return;
      }
      // 收起即清空金额条件，并立即按新条件回到第一页。
      amountFields.hidden = true;
      amountToggle.textContent = "金额";
      amountMinInput.value = "";
      amountMaxInput.value = "";
      page = 1;
      reload();
    });
    prevButton.addEventListener("click", () => {
      if (loading || page <= 1) return;
      page -= 1;
      fetchPage();
    });
    nextButton.addEventListener("click", () => {
      if (loading) return;
      page += 1;
      fetchPage();
    });

    return {
      table,
      reload,
      rowById(id) {
        return rows.find((doc) => doc.id === id);
      },
      get rows() {
        return rows;
      },
    };
  }

  // ---------- 单据页面（销售出库 / 采购入库共用） ----------

  async function documentPage(mount, config) {
    const docType = config.docType;
    const costAdjust = !!config.costAdjust;
    // 只有其他出入库有业务类型；入库可用期初，出库可用领用与报损，赠品两方向通用。
    const otherDoc = docType === "OTH_IN" || docType === "OTH_OUT";
    const salesDoc = docType === "SAL_OUT" || docType === "SAL_RET";
    const categoryOptions =
      docType === "OTH_IN"
        ? [
            ["GIFT", "赠品"],
            ["OPENING", "期初"],
            ["OTHER", "其他"],
          ]
        : [
            ["GIFT", "赠品"],
            ["USAGE", "领用"],
            ["LOSS", "报损"],
            ["OTHER", "其他"],
          ];
    let warehouses = [];
    let items = [];
    let editingId = 0;
    // 乐观锁版本与幂等键：版本随详情/列表带回，request_id 每次新建页生成一次。
    let editingVersion = -1;
    let requestId = newRequestId();
    // 录入页的脏标记：任一字段变动即置脏，保存成功、重置或切换单据后清除。
    let dirty = false;

    mount.innerHTML = `
      <div class="page-head">
        <div>
          <h2>${escapeHtml(config.title)}</h2>
          <p class="page-sub">${escapeHtml(config.subtitle)}</p>
        </div>
        <button class="btn" data-role="refresh">刷新</button>
      </div>
      <div class="card">
        <form class="doc-form" data-role="form">
          <div class="doc-head">
            <label class="field" ${costAdjust ? "hidden" : ""}><span>${config.twoWarehouse ? "调出仓库" : "仓库"}</span>
              <select data-role="warehouse" required></select>
            </label>
            <label class="field" ${config.twoWarehouse ? "" : "hidden"}><span>调入仓库</span>
              <select data-role="target-warehouse" required></select>
            </label>
            <label class="field"><span>单据日期</span>
              <input type="date" data-role="date" required />
            </label>
            <label class="field" ${costAdjust ? "hidden" : ""}><span>${escapeHtml(config.partnerLabel)}</span>
              <input type="text" data-role="partner" placeholder="${escapeHtml(
                config.partnerPlaceholder
              )}" ${config.partnerApi ? "hidden" : ""} />
              <select data-role="partner-select" ${config.partnerApi ? "" : "hidden"}></select>
            </label>
            <label class="field" ${otherDoc ? "" : "hidden"}><span>业务类型</span>
              <select data-role="biz-category">
                ${categoryOptions
                  .map(([value, label]) => `<option value="${value}">${label}</option>`)
                  .join("")}
              </select>
            </label>
            <label class="field" ${salesDoc ? "" : "hidden"}><span>业务员</span>
              <select data-role="salesperson">
                <option value="0">未指定</option>
              </select>
            </label>
          </div>
          <div class="doc-remark">
            <label class="field"><span>备注</span>
              <textarea data-role="remark" rows="2" maxlength="200" placeholder="选填，最多 200 个字"></textarea>
            </label>
          </div>
          <table class="table lines" data-role="lines"></table>
          <p class="page-sub" data-role="doc-totals"></p>
          <div class="form-actions">
            <button class="btn" type="button" data-role="add-line">添加明细行</button>
            <button class="btn" type="button" data-role="save-draft">存为草稿</button>
            <button class="btn primary" type="button" data-role="submit">保存并审核</button>
            <button class="btn" type="button" data-role="cancel-edit" hidden>取消编辑</button>
          </div>
        </form>
      </div>
      <div class="card" data-role="doc-list"></div>`;

    const form = mount.querySelector('[data-role="form"]');
    const warehouseSelect = mount.querySelector('[data-role="warehouse"]');
    const targetWarehouseSelect = mount.querySelector('[data-role="target-warehouse"]');
    const dateInput = mount.querySelector('[data-role="date"]');
    const linesTable = mount.querySelector('[data-role="lines"]');
    const partnerSelect = mount.querySelector('[data-role="partner-select"]');
    const bizCategorySelect = mount.querySelector('[data-role="biz-category"]');
    const salespersonSelect = mount.querySelector('[data-role="salesperson"]');
    const remarkInput = mount.querySelector('[data-role="remark"]');

    const documentList = createDocumentList({
      docType,
      mount: mount.querySelector('[data-role="doc-list"]'),
      emptyText: `还没有${config.docLabel}单`,
      columns: [
        { title: "单据号", cell: (doc) => escapeHtml(doc.doc_no) },
        { title: "日期", cell: (doc) => escapeHtml(doc.doc_date) },
        { title: "仓库", cell: (doc) => escapeHtml(doc.warehouse_name) },
        { title: config.partnerLabel, cell: (doc) => escapeHtml(doc.customer || "—") },
        ...(otherDoc
          ? [
              {
                title: "业务类型",
                cell: (doc) => escapeHtml(bizCategoryText(doc.biz_category) || "—"),
              },
            ]
          : []),
        ...(salesDoc
          ? [
              {
                title: "业务员",
                cell: (doc) => escapeHtml(doc.salesperson_name || "未指定"),
              },
            ]
          : []),
        { title: "数量", cell: (doc) => (costAdjust ? "—" : qtyText(doc.total_qty)) },
        { title: "金额（元）", cell: (doc) => money(doc.total_amount_cents) },
        { title: "状态", cell: (doc) => statusLabel(doc.status) },
        { title: "操作", actions: true, cell: (doc) => actionsFor(doc) },
      ],
    });
    const documentsTable = documentList.table;

    dateInput.value = new Date().toISOString().slice(0, 10);
    if (costAdjust && !can("cost.adjust")) {
      mount.querySelector('[data-role="submit"]').hidden = true;
    }

    function itemOptions(selected) {
      return items
        .map(
          (item) =>
            `<option value="${item.id}" ${String(item.id) === String(selected) ? "selected" : ""}>${escapeHtml(
              itemOptionText(item)
            )}</option>`
        )
        .join("");
    }

    function warehouseOptions(selected) {
      return warehouses
        .filter((warehouse) => warehouse.status)
        .map(
          (warehouse) =>
            `<option value="${warehouse.id}" ${
              String(warehouse.id) === String(selected) ? "selected" : ""
            }>${escapeHtml(warehouse.code + " " + warehouse.name)}</option>`
        )
        .join("");
    }

    function addLine(line) {
      const row = document.createElement("tr");
      if (costAdjust) {
        row.innerHTML = `
          <td><select data-role="item">${itemOptions(line ? line.item_id : undefined)}</select></td>
          <td><select data-role="line-warehouse">${warehouseOptions(
            line ? line.warehouse_id : undefined
          )}</select></td>
          <td><input type="text" inputmode="decimal" value="0.00" data-role="adjust" /></td>
          <td><input type="text" placeholder="调整原因" data-role="reason" /></td>
          <td class="actions"><button class="link danger" type="button" data-role="remove">移除</button></td>`;
        linesTable.querySelector("tbody").appendChild(row);
        row.querySelector('[data-role="remove"]').addEventListener("click", () => row.remove());
        if (line) {
          row.querySelector('[data-role="adjust"]').value = (line.adjust_cents / 100).toFixed(2);
          row.querySelector('[data-role="reason"]').value = line.adjust_reason || "";
        }
        return;
      }
      row.innerHTML = `
        <td><select data-role="item">${itemOptions(line ? line.item_id : undefined)}</select></td>
        <td><span class="qty-cell"><input type="text" inputmode="decimal" value="1" data-role="qty" /><span class="unit-hint" data-role="unit"></span></span></td>
        <td><input type="number" min="0" step="0.01" value="0.00" data-role="price" /></td>
        <td><input type="text" class="readonly" value="—" data-role="amount" disabled /></td>
        <td><input type="number" min="0" max="100" step="0.01" value="0" data-role="tax-rate" /></td>
        <td><label class="checkbox"><input type="checkbox" data-role="tax-included" /></label></td>
        <td><input type="text" class="readonly" value="—" data-role="tax" disabled /></td>
        <td><input type="text" class="readonly" value="—" data-role="with-tax" disabled /></td>
        <td class="actions"><button class="link danger" type="button" data-role="remove">移除</button></td>`;
      linesTable.querySelector("tbody").appendChild(row);
      row.querySelector('[data-role="remove"]').addEventListener("click", () => {
        row.remove();
        recalcTotals();
      });
      const recalc = () => {
        const qty = parseQty(row.querySelector('[data-role="qty"]').value, true);
        const price = Math.round(Number(row.querySelector('[data-role="price"]').value || 0) * 100);
        const amount = qtyAmountCents(qty, price);
        const rateBp = Math.round(Number(row.querySelector('[data-role="tax-rate"]').value || 0) * 100);
        const includesTax = row.querySelector('[data-role="tax-included"]').checked;
        row.querySelector('[data-role="amount"]').value = qtyAmountText(qty, price);
        const lineTax = taxForAmount(amount, rateBp, includesTax).tax;
        row.querySelector('[data-role="tax"]').value = money(lineTax);
        row.querySelector('[data-role="with-tax"]').value = money(
          includesTax ? amount : amount + lineTax
        );
        recalcTotals();
      };
      row.querySelector('[data-role="qty"]').addEventListener("input", recalc);
      row.querySelector('[data-role="price"]').addEventListener("input", recalc);
      row.querySelector('[data-role="tax-rate"]').addEventListener("input", recalc);
      row.querySelector('[data-role="tax-included"]').addEventListener("change", recalc);
      const itemSelect = row.querySelector('[data-role="item"]');
      const unitHint = row.querySelector('[data-role="unit"]');
      const updateUnit = () => {
        const item = items.find((candidate) => String(candidate.id) === String(itemSelect.value));
        unitHint.textContent = item && item.unit ? item.unit : "";
        // 新行跟随商品默认税率；编辑历史单据时保留落库税率。
        if (!line) {
          const bp = item && item.tax_rate_bp ? item.tax_rate_bp : 0;
          row.querySelector('[data-role="tax-rate"]').value = (bp / 100)
            .toFixed(2)
            .replace(/\.?0+$/, "");
        }
        recalc();
      };
      itemSelect.addEventListener("change", updateUnit);
      updateUnit();
      if (line) {
        row.querySelector('[data-role="qty"]').value = qtyText(line.qty);
        row.querySelector('[data-role="price"]').value = (line.price_cents / 100).toFixed(2);
        row.querySelector('[data-role="tax-rate"]').value = ((line.tax_rate_bp || 0) / 100)
          .toFixed(2)
          .replace(/\.?0+$/, "");
        row.querySelector('[data-role="tax-included"]').checked = !!line.price_includes_tax;
        recalc();
      }
    }

    function recalcTotals() {
      if (costAdjust) return;
      let qty = 0;
      let amount = 0;
      let tax = 0;
      let withTax = 0;
      for (const row of linesTable.querySelectorAll("tbody tr")) {
        const qtyScaled = parseQty(row.querySelector('[data-role="qty"]').value, true) || 0;
        const price = Math.round(Number(row.querySelector('[data-role="price"]').value || 0) * 100);
        const lineAmount = qtyAmountCents(qtyScaled, price);
        const rateBp = Math.round(Number(row.querySelector('[data-role="tax-rate"]').value || 0) * 100);
        const includesTax = row.querySelector('[data-role="tax-included"]').checked;
        const lineTax = taxForAmount(lineAmount, rateBp, includesTax).tax;
        qty += Number(qtyScaled);
        amount += lineAmount;
        tax += lineTax;
        withTax += includesTax ? lineAmount : lineAmount + lineTax;
      }
      const totalsEl = mount.querySelector('[data-role="doc-totals"]');
      if (totalsEl) {
        totalsEl.textContent =
          "合计数量 " + qtyText(qty) + "，金额 " + money(amount) +
          "，税额 " + money(tax) + "，价税合计 " + money(withTax);
      }
    }

    function resetLines() {
      linesTable.innerHTML = costAdjust
        ? `<thead><tr><th>商品</th><th>仓库</th><th>调整金额（元）</th><th>调整原因</th><th class="actions">操作</th></tr></thead><tbody></tbody>`
        : `<thead><tr><th>商品</th><th>数量</th><th>单价（元）</th><th>金额（录入价）</th><th>税率（%）</th><th>含税</th><th>税额（元）</th><th>价税合计（元）</th><th class="actions">操作</th></tr></thead><tbody></tbody>`;
      addLine();
      recalcTotals();
    }

    function statusLabel(status) {
      return { APPROVED: "已审核", DRAFT: "草稿", VOIDED: "已作废" }[status] || status;
    }

    function actionsFor(doc) {
      const print =
        '<button class="link" data-role="print">打印</button>' +
        '<button class="link" data-role="pdf">导出 PDF</button>' +
        '<button class="link" data-role="export-csv">CSV</button>' +
        '<button class="link" data-role="export-xlsx">Excel</button>';
      if (costAdjust && !can("cost.adjust")) {
        return doc.status === "DRAFT"
          ? print + '<button class="link" data-role="edit">编辑</button>'
          : print;
      }
      if (doc.status === "DRAFT") {
        return (
          print +
          '<button class="link" data-role="approve">审核</button>' +
          '<button class="link" data-role="edit">编辑</button>' +
          '<button class="link danger" data-role="delete">删除</button>'
        );
      }
      if (doc.status === "APPROVED") {
        return print + '<button class="link danger" data-role="reverse">反审核</button>';
      }
      return print;
    }

    async function reload() {
      const [warehouseList, itemList, partnerList, employeeList] = await Promise.all([
        api("GET", "/api/warehouses"),
        api("GET", "/api/items"),
        config.partnerApi ? api("GET", config.partnerApi) : Promise.resolve([]),
        salesDoc ? api("GET", "/api/employees") : Promise.resolve([]),
      ]);
      warehouses = warehouseList;
      items = itemList;
      if (config.partnerApi) {
        partnerSelect.innerHTML =
          '<option value="">不指定</option>' +
          partnerList
            .filter((partner) => partner.status)
            .map(
              (partner) =>
                `<option value="${partner.id}" data-name="${escapeHtml(partner.name)}">${escapeHtml(
                  partner.code + " " + partner.name
                )}</option>`
            )
            .join("");
      }
      if (salesDoc) {
        // 停用/离职职员仍列在历史单据里，这里保留可选项并标注，不隐藏。
        salespersonSelect.innerHTML =
          '<option value="0">未指定</option>' +
          employeeList
            .map(
              (person) =>
                `<option value="${person.id}">${escapeHtml(
                  person.code + " " + person.name
                )}${person.status ? "" : "（停用）"}</option>`
            )
            .join("");
      }
      const warehouseOptions = warehouses
        .filter((warehouse) => warehouse.status)
        .map(
          (warehouse) =>
            `<option value="${warehouse.id}">${escapeHtml(warehouse.code + " " + warehouse.name)}</option>`
        )
        .join("");
      warehouseSelect.innerHTML = warehouseOptions;
      if (config.twoWarehouse) {
        targetWarehouseSelect.innerHTML = warehouseOptions;
        if (warehouses.filter((warehouse) => warehouse.status).length > 1) {
          targetWarehouseSelect.selectedIndex = 1;
        }
      }
      await documentList.reload();
      if (!linesTable.querySelector("tbody") || !linesTable.querySelector("tbody").children.length) {
        resetLines();
      }
    }

    function resetForm() {
      editingId = 0;
      editingVersion = -1;
      // 提交成功/取消后作废幂等键，下一次新建重新生成。
      requestId = newRequestId();
      dirty = false;
      // 提交成功、取消编辑都算显式结束，本地草稿一并清掉。
      clearDraft(docType);
      mount.querySelector('[data-role="cancel-edit"]').hidden = true;
      form.querySelector('[data-role="partner"]').value = "";
      if (partnerSelect) partnerSelect.value = "";
      if (otherDoc) bizCategorySelect.value = "OTHER";
      if (salesDoc) salespersonSelect.value = "0";
      remarkInput.value = "";
      dateInput.value = new Date().toISOString().slice(0, 10);
      resetLines();
    }

    // 长表单本地草稿：只存业务字段，按单据类型分键，每 30 秒写一次。
    const draftEnteredAt = Date.now();

    function draftFormData() {
      const data = {
        warehouse_id: Number(warehouseSelect.value || 0),
        target_warehouse_id: config.twoWarehouse
          ? Number(targetWarehouseSelect.value || 0)
          : 0,
        doc_date: dateInput.value,
        partner: form.querySelector('[data-role="partner"]').value.trim(),
        partner_id: partnerSelect && !partnerSelect.hidden ? Number(partnerSelect.value || 0) : 0,
        biz_category: otherDoc ? bizCategorySelect.value : "",
        salesperson_id: salesDoc ? Number(salespersonSelect.value || 0) : 0,
        remark: remarkInput.value.trim(),
        lines: [],
      };
      for (const row of linesTable.querySelectorAll("tbody tr")) {
        const itemId = Number(row.querySelector('[data-role="item"]').value);
        if (!itemId) continue;
        if (costAdjust) {
          data.lines.push({
            item_id: itemId,
            warehouse_id: Number(row.querySelector('[data-role="line-warehouse"]').value || 0),
            adjust: row.querySelector('[data-role="adjust"]').value.trim(),
            reason: row.querySelector('[data-role="reason"]').value.trim(),
          });
          continue;
        }
        data.lines.push({
          item_id: itemId,
          qty: row.querySelector('[data-role="qty"]').value.trim(),
          price: row.querySelector('[data-role="price"]').value,
          tax_rate: row.querySelector('[data-role="tax-rate"]').value,
          tax_included: row.querySelector('[data-role="tax-included"]').checked,
        });
      }
      return data;
    }

    function saveDraftNow() {
      // 编辑服务端已有草稿时不写本地草稿，避免和版本校验互相干扰。
      if (editingId || !dirty) return false;
      writeDraft(docType, {
        savedAt: Date.now(),
        enteredAt: draftEnteredAt,
        form: draftFormData(),
      });
      return true;
    }

    function applyDraft(draft) {
      const data = draft.form;
      editingId = 0;
      editingVersion = -1;
      // 恢复出来的是新的提交意图，重新生成幂等键。
      requestId = newRequestId();
      mount.querySelector('[data-role="cancel-edit"]').hidden = true;
      if (data.warehouse_id) warehouseSelect.value = String(data.warehouse_id);
      if (config.twoWarehouse && data.target_warehouse_id) {
        targetWarehouseSelect.value = String(data.target_warehouse_id);
      }
      if (data.doc_date) dateInput.value = data.doc_date;
      form.querySelector('[data-role="partner"]').value = data.partner || "";
      if (partnerSelect && !partnerSelect.hidden) {
        partnerSelect.value = data.partner_id ? String(data.partner_id) : "";
      }
      if (otherDoc) bizCategorySelect.value = data.biz_category || "OTHER";
      if (salesDoc) salespersonSelect.value = String(data.salesperson_id || 0);
      remarkInput.value = data.remark || "";
      linesTable.innerHTML = costAdjust
        ? `<thead><tr><th>商品</th><th>仓库</th><th>调整金额（元）</th><th>调整原因</th><th class="actions">操作</th></tr></thead><tbody></tbody>`
        : `<thead><tr><th>商品</th><th>数量</th><th>单价（元）</th><th>金额（录入价）</th><th>税率（%）</th><th>含税</th><th>税额（元）</th><th>价税合计（元）</th><th class="actions">操作</th></tr></thead><tbody></tbody>`;
      for (const line of data.lines || []) {
        addLine(
          costAdjust
            ? {
                item_id: line.item_id,
                qty: "0",
                price_cents: 0,
                warehouse_id: line.warehouse_id,
                adjust_cents: Math.round(Number(line.adjust || 0) * 100),
                adjust_reason: line.reason || "",
              }
            : {
                item_id: line.item_id,
                qty: parseQty(line.qty, true) || "0",
                price_cents: Math.round(Number(line.price || 0) * 100),
                tax_rate_bp: Math.round(Number(line.tax_rate || 0) * 100),
                price_includes_tax: Boolean(line.tax_included),
              }
        );
      }
      if (!linesTable.querySelector("tbody tr")) addLine();
      if (!costAdjust) recalcTotals();
      dirty = true;
    }

    async function restoreDraftIfAny() {
      const draft = readDraft(docType);
      if (!draft) return;
      // 只恢复「进入表单后确实改过」的草稿：写入时间晚于该草稿记录的进入时间。
      if (!(Number(draft.savedAt) > Number(draft.enteredAt || 0))) {
        clearDraft(docType);
        return;
      }
      const when = new Date(Number(draft.savedAt));
      const pad = (value) => String(value).padStart(2, "0");
      const stamp =
        pad(when.getMonth() + 1) +
        "-" +
        pad(when.getDate()) +
        " " +
        pad(when.getHours()) +
        ":" +
        pad(when.getMinutes());
      const restore = await askConfirm(
        "发现「" + config.title + "」未提交的本地草稿（保存于 " + stamp + "），是否恢复？",
        { okText: "恢复", cancelText: "放弃" }
      );
      if (!restore) {
        // 放弃即显式丢弃，清掉本地草稿。
        clearDraft(docType);
        return;
      }
      applyDraft(draft);
      showNotice("已恢复本地草稿，可继续填写", "ok");
    }

    function collectLines() {
      const lines = [];
      for (const row of linesTable.querySelectorAll("tbody tr")) {
        const itemId = Number(row.querySelector('[data-role="item"]').value);
        if (costAdjust) {
          const warehouseId = Number(row.querySelector('[data-role="line-warehouse"]').value);
          const adjustText = row.querySelector('[data-role="adjust"]').value.trim();
          const adjust = Math.round(Number(adjustText) * 100);
          const reason = row.querySelector('[data-role="reason"]').value.trim();
          if (!itemId || !warehouseId || !adjustText || !Number.isFinite(adjust) || adjust === 0) {
            showNotice("请检查明细行的商品、仓库与调整金额", "error");
            return null;
          }
          if (!reason) {
            showNotice("请填写调整原因", "error");
            return null;
          }
          lines.push({
            item_id: itemId,
            qty: "0",
            price_cents: 0,
            warehouse_id: warehouseId,
            adjust_cents: adjust,
            adjust_reason: reason,
          });
          continue;
        }
        const qty = parseQty(
          row.querySelector('[data-role="qty"]').value,
          docType === "STK_CNT"
        );
        const price = Math.round(Number(row.querySelector('[data-role="price"]').value || 0) * 100);
        const rateBp = Math.round(
          Number(row.querySelector('[data-role="tax-rate"]').value || 0) * 100
        );
        if (!itemId || qty === null) {
          showNotice("请检查明细行的商品与数量", "error");
          return null;
        }
        lines.push({
          item_id: itemId,
          qty,
          price_cents: price,
          tax_rate_bp: rateBp,
          price_includes_tax: row.querySelector('[data-role="tax-included"]').checked,
        });
      }
      if (!lines.length) {
        showNotice("请至少添加一行明细", "error");
        return null;
      }
      return lines;
    }

    async function save(draft) {
      const lines = collectLines();
      if (!lines) return;
      const partnerInput = form.querySelector('[data-role="partner"]');
      const usePartnerSelect = partnerSelect && !partnerSelect.hidden;
      const partnerOption = usePartnerSelect ? partnerSelect.selectedOptions[0] : null;
      const body = {
        doc_type: docType,
        warehouse_id: costAdjust ? 0 : Number(warehouseSelect.value),
        target_warehouse_id: config.twoWarehouse ? Number(targetWarehouseSelect.value) : 0,
        doc_date: dateInput.value,
        partner_id: usePartnerSelect ? Number(partnerSelect.value || 0) : 0,
        partner: usePartnerSelect
          ? partnerOption && partnerOption.value
            ? partnerOption.dataset.name || ""
            : ""
          : partnerInput.value.trim(),
        biz_category: otherDoc ? bizCategorySelect.value : "",
        salesperson_id: salesDoc ? Number(salespersonSelect.value || 0) : 0,
        remark: remarkInput.value.trim(),
        lines,
      };
      const submit = async (confirmCredit) => {
        if (editingId) {
          // 带上读取时的版本；被他人改过会返回 VERSION_CONFLICT。
          const updated = await api("PUT", "/api/documents/" + editingId, {
            ...body,
            confirm_credit: confirmCredit,
            version: editingVersion,
          });
          editingVersion = updated ? updated.version ?? editingVersion : editingVersion;
          if (!draft) {
            await api(
              "POST",
              "/api/documents/" + editingId + "/approve" +
                (confirmCredit ? "?confirm_credit=1" : ""),
              { version: editingVersion }
            );
          }
          return null;
        }
        return api("POST", "/api/documents", {
          ...body,
          save_as_draft: draft,
          confirm_credit: confirmCredit,
          request_id: requestId,
        });
      };
      const finish = async (result) => {
        showNotice(
          result
            ? (draft ? "已存为草稿：" : "已保存并审核：") + result.doc_no
            : draft
              ? "草稿已保存"
              : "已保存并审核",
          "ok"
        );
        resetForm();
        await reload();
      };
      try {
        await finish(await submit(false));
      } catch (error) {
        if (error.code !== "CREDIT_LIMIT_WARNING") {
          showNotice(error.message, "error");
          return;
        }
        const ok = await askConfirm(error.message + "。仍要继续审核吗？", {
          okText: "继续出库",
          cancelText: "取消",
        });
        if (!ok) return;
        try {
          await finish(await submit(true));
        } catch (retryError) {
          showNotice(retryError.message, "error");
        }
      }
    }

    async function startEdit(doc) {
      try {
        const detail = await api("GET", "/api/documents/" + doc.id);
        editingId = doc.id;
        editingVersion = detail.version ?? -1;
        warehouseSelect.value = String(detail.warehouse_id);
        dateInput.value = detail.doc_date;
        if (config.twoWarehouse && detail.target_warehouse_id) {
          targetWarehouseSelect.value = String(detail.target_warehouse_id);
        }
        form.querySelector('[data-role="partner"]').value = detail.partner || "";
        if (partnerSelect && !partnerSelect.hidden) {
          partnerSelect.value = detail.partner_id ? String(detail.partner_id) : "";
        }
        if (otherDoc) bizCategorySelect.value = detail.biz_category || "OTHER";
        if (salesDoc) salespersonSelect.value = String(detail.salesperson_id || 0);
        remarkInput.value = detail.remark || "";
        linesTable.innerHTML = costAdjust
          ? `<thead><tr><th>商品</th><th>仓库</th><th>调整金额（元）</th><th>调整原因</th><th class="actions">操作</th></tr></thead><tbody></tbody>`
          : `<thead><tr><th>商品</th><th>数量</th><th>单价（元）</th><th>金额（元）</th><th class="actions">操作</th></tr></thead><tbody></tbody>`;
        for (const line of detail.lines) addLine(line);
        dirty = false;
        mount.querySelector('[data-role="cancel-edit"]').hidden = false;
        form.scrollIntoView({ behavior: "smooth", block: "start" });
        showNotice("正在编辑草稿 " + detail.doc_no, "ok");
      } catch (error) {
        showNotice(error.message, "error");
      }
    }

    // 只有真正录过内容才拦：任一输入/选择变动即置脏，离开前确认一次。
    form.addEventListener("input", () => {
      dirty = true;
    });
    form.addEventListener("change", () => {
      dirty = true;
    });
    setUnsavedGuard(() => dirty);

    mount.querySelector('[data-role="add-line"]').addEventListener("click", () => addLine());
    mount.querySelector('[data-role="refresh"]').addEventListener("click", () => reload());
    mount.querySelector('[data-role="cancel-edit"]').addEventListener("click", () => resetForm());
    mount.querySelector('[data-role="save-draft"]').addEventListener("click", () => save(true));
    mount.querySelector('[data-role="submit"]').addEventListener("click", () => save(false));

    documentsTable.addEventListener("click", async (event) => {
      const button = event.target.closest("button");
      if (!button) return;
      const id = Number(event.target.closest("tr").dataset.id);
      try {
        if (button.dataset.role === "print") {
          await printTarget(
            "/api/print/render?target_type=DOC&target_key=" +
              docType +
              "&doc_id=" +
              id
          );
          return;
        }
        if (button.dataset.role === "pdf") {
          const doc = documentList.rowById(id);
          await exportPdfTarget(
            "/api/print/render?target_type=DOC&target_key=" + docType + "&doc_id=" + id,
            doc ? doc.doc_no : undefined
          );
          return;
        }
        if (button.dataset.role === "export-csv") {
          await downloadCsv("/api/documents/" + id + "/export?format=csv");
          showNotice("已导出 CSV", "ok");
          return;
        }
        if (button.dataset.role === "export-xlsx") {
          await downloadXlsx("/api/documents/" + id + "/export?format=xlsx");
          showNotice("已导出 Excel", "ok");
          return;
        }
        if (button.dataset.role === "reverse") {
          if (!(await askConfirm("反审核会生成反向库存流水，确认继续？"))) return;
          const doc = documentList.rowById(id);
          await api(
            "POST",
            "/api/documents/" + id + "/reverse",
            doc ? { version: doc.version } : {}
          );
          showNotice("已反审核", "ok");
        } else if (button.dataset.role === "approve") {
          if (!(await askConfirm("审核后才会写库存流水，确认继续？"))) return;
          const doc = documentList.rowById(id);
          await api(
            "POST",
            "/api/documents/" + id + "/approve",
            doc ? { version: doc.version } : {}
          );
          showNotice("已审核", "ok");
        } else if (button.dataset.role === "delete") {
          if (!(await askConfirm("确认删除这张草稿？"))) return;
          const doc = documentList.rowById(id);
          await api(
            "DELETE",
            "/api/documents/" + id + (doc ? "?version=" + doc.version : "")
          );
          if (editingId === id) resetForm();
          showNotice("草稿已删除", "ok");
        } else if (button.dataset.role === "edit") {
          await startEdit(documentList.rowById(id));
          return;
        }
        await reload();
      } catch (error) {
        showNotice(error.message, "error");
      }
    });

    await reload();
    startDraftTimer(saveDraftNow);
    await restoreDraftIfAny();
  }

  // ---------- 订单页面（销售订单 / 采购订单共用） ----------

  async function orderPage(mount, options = {}) {
    const config = {
      docType: "SAL_ORDER",
      title: "销售订单",
      subtitle: "只做需求确认与转出库来源，不占库存、不写流水、不产生往来",
      partnerLabel: "客户",
      partnerApi: "/api/customers",
      warehouseLabel: "预计出库仓",
      convertTitle: "转出库",
      convertWarehouseLabel: "出库仓库",
      convertDateLabel: "出库日期",
      convertAction: "转出库",
      convertSubmit: "确认转出",
      convertNotice: "已生成销售出库单：",
      convertedLabel: "已转出库",
      partialLabel: "部分转出",
      emptyLabel: "还没有销售订单",
      ...options,
    };
    const docType = config.docType;
    let warehouses = [];
    let items = [];
    let customers = [];
    let editingId = 0;
    let editingVersion = -1;
    let requestId = newRequestId();
    let convertingId = 0;
    // 订单录入的本地草稿：只有改过表单才写，按订单类型分键。
    let formDirty = false;
    const draftEnteredAt = Date.now();

    mount.innerHTML = `
      <div class="page-head">
        <div>
          <h2>${escapeHtml(config.title)}</h2>
          <p class="page-sub">${escapeHtml(config.subtitle)}</p>
        </div>
        <button class="btn" data-role="refresh">刷新</button>
      </div>
      <div class="card">
        <form class="doc-form" data-role="form">
          <div class="doc-head">
            <label class="field"><span>${escapeHtml(config.warehouseLabel)}</span>
              <select data-role="warehouse" required></select>
            </label>
            <label class="field"><span>订单日期</span>
              <input type="date" data-role="date" required />
            </label>
            <label class="field"><span>${escapeHtml(config.partnerLabel)}</span>
              <select data-role="customer"></select>
            </label>
            ${
              docType === "SAL_ORDER"
                ? `<label class="field"><span>业务员</span>
                    <select data-role="salesperson">
                      <option value="0">未指定</option>
                    </select>
                  </label>`
                : ""
            }
          </div>
          <div class="doc-remark">
            <label class="field"><span>备注</span>
              <textarea data-role="remark" rows="2" maxlength="200" placeholder="选填，最多 200 个字"></textarea>
            </label>
          </div>
          <table class="table lines" data-role="lines"></table>
          <div class="form-actions">
            <button class="btn" type="button" data-role="add-line">添加明细行</button>
            <button class="btn" type="button" data-role="save-draft">存为草稿</button>
            <button class="btn primary" type="button" data-role="submit">保存并确认</button>
            <button class="btn" type="button" data-role="cancel-edit" hidden>取消编辑</button>
          </div>
        </form>
      </div>
      <div class="card" data-role="convert-card" hidden>
        <div class="card-head">
          <h3>${escapeHtml(config.convertTitle)}</h3>
          <span class="page-sub" data-role="convert-title"></span>
        </div>
        <div class="doc-head">
          <label class="field"><span>${escapeHtml(config.convertWarehouseLabel)}</span>
            <select data-role="convert-warehouse"></select>
          </label>
          <label class="field"><span>${escapeHtml(config.convertDateLabel)}</span>
            <input type="date" data-role="convert-date" />
          </label>
        </div>
        <table class="table" data-role="convert-lines"></table>
        <div class="form-actions">
          <button class="btn primary" type="button" data-role="convert-submit">${escapeHtml(
            config.convertSubmit
          )}</button>
          <button class="btn" type="button" data-role="convert-cancel">取消</button>
        </div>
      </div>
      <div class="card" data-role="doc-list"></div>`;

    const form = mount.querySelector('[data-role="form"]');
    const warehouseSelect = mount.querySelector('[data-role="warehouse"]');
    const dateInput = mount.querySelector('[data-role="date"]');
    const customerSelect = mount.querySelector('[data-role="customer"]');
    const salespersonSelect = mount.querySelector('[data-role="salesperson"]');
    const remarkInput = mount.querySelector('[data-role="remark"]');
    const linesTable = mount.querySelector('[data-role="lines"]');
    const convertCard = mount.querySelector('[data-role="convert-card"]');
    const convertTitle = mount.querySelector('[data-role="convert-title"]');
    const convertWarehouse = mount.querySelector('[data-role="convert-warehouse"]');
    const convertDate = mount.querySelector('[data-role="convert-date"]');
    const convertLines = mount.querySelector('[data-role="convert-lines"]');

    const documentList = createDocumentList({
      docType,
      mount: mount.querySelector('[data-role="doc-list"]'),
      emptyText: config.emptyLabel,
      columns: [
        { title: "订单号", cell: (doc) => escapeHtml(doc.doc_no) },
        { title: "日期", cell: (doc) => escapeHtml(doc.doc_date) },
        { title: "仓库", cell: (doc) => escapeHtml(doc.warehouse_name) },
        { title: config.partnerLabel, cell: (doc) => escapeHtml(doc.customer || "—") },
        ...(docType === "SAL_ORDER"
          ? [
              {
                title: "业务员",
                cell: (doc) => escapeHtml(doc.salesperson_name || "未指定"),
              },
            ]
          : []),
        { title: "数量", cell: (doc) => qtyText(doc.total_qty) },
        { title: "金额（元）", cell: (doc) => money(doc.total_amount_cents) },
        { title: "状态", cell: (doc) => orderStatusLabel(doc) },
        { title: "操作", actions: true, cell: (doc) => actionsFor(doc) },
      ],
    });
    const documentsTable = documentList.table;

    dateInput.value = new Date().toISOString().slice(0, 10);
    convertDate.value = dateInput.value;

    function itemLabelById(itemId) {
      const item = items.find((row) => row.id === itemId);
      return item ? itemOptionText(item) : String(itemId);
    }

    function itemOptions(selected) {
      return items
        .map(
          (item) =>
            `<option value="${item.id}" ${String(item.id) === String(selected) ? "selected" : ""}>${escapeHtml(
              itemOptionText(item)
            )}</option>`
        )
        .join("");
    }

    function addLine(line) {
      const row = document.createElement("tr");
      row.innerHTML = `
        <td><select data-role="item">${itemOptions(line ? line.item_id : undefined)}</select></td>
        <td><span class="qty-cell"><input type="text" inputmode="decimal" value="1" data-role="qty" /><span class="unit-hint" data-role="unit"></span></span></td>
        <td><input type="number" min="0" step="0.01" value="0.00" data-role="price" /></td>
        <td><input type="text" class="readonly" value="—" data-role="amount" disabled /></td>
        <td class="actions"><button class="link danger" type="button" data-role="remove">移除</button></td>`;
      linesTable.querySelector("tbody").appendChild(row);
      row.querySelector('[data-role="remove"]').addEventListener("click", () => row.remove());
      const recalc = () => {
        const qty = parseQty(row.querySelector('[data-role="qty"]').value, true);
        const price = Math.round(Number(row.querySelector('[data-role="price"]').value || 0) * 100);
        row.querySelector('[data-role="amount"]').value = qtyAmountText(qty, price);
      };
      row.querySelector('[data-role="qty"]').addEventListener("input", recalc);
      row.querySelector('[data-role="price"]').addEventListener("input", recalc);
      const itemSelect = row.querySelector('[data-role="item"]');
      const unitHint = row.querySelector('[data-role="unit"]');
      const updateUnit = () => {
        const item = items.find((candidate) => String(candidate.id) === String(itemSelect.value));
        unitHint.textContent = item && item.unit ? item.unit : "";
      };
      itemSelect.addEventListener("change", updateUnit);
      updateUnit();
      if (line) {
        row.querySelector('[data-role="qty"]').value = qtyText(line.qty);
        row.querySelector('[data-role="price"]').value = (line.price_cents / 100).toFixed(2);
        recalc();
      }
    }

    function resetLines() {
      linesTable.innerHTML = `<thead><tr><th>商品</th><th>数量</th><th>单价（元）</th><th>金额（元）</th><th class="actions">操作</th></tr></thead><tbody></tbody>`;
      addLine();
    }

    function orderState(doc) {
      if (doc.status === "DRAFT") return "DRAFT";
      if (doc.order_status === "CLOSED") return "CLOSED";
      if (doc.order_status === "CONVERTED") return "CONVERTED";
      if (BigInt(doc.converted_qty || 0) > 0n) return "PARTIAL";
      return "CONFIRMED";
    }

    function orderStatusLabel(doc) {
      const state = orderState(doc);
      if (state === "DRAFT") return "草稿";
      if (state === "CLOSED") return "已关闭";
      if (state === "CONVERTED") return config.convertedLabel;
      if (state === "PARTIAL") return config.partialLabel;
      return "已确认";
    }

    function actionsFor(doc) {
      const print =
        '<button class="link" data-role="print">打印</button>' +
        '<button class="link" data-role="pdf">导出 PDF</button>';
      if (doc.status === "DRAFT") {
        return (
          print +
          '<button class="link" data-role="approve">确认</button>' +
          '<button class="link" data-role="edit">编辑</button>' +
          '<button class="link danger" data-role="delete">删除</button>'
        );
      }
      if (doc.order_status === "CONFIRMED") {
        return (
          print +
          `<button class="link" data-role="convert">${escapeHtml(config.convertAction)}</button>` +
          '<button class="link danger" data-role="close">关闭</button>'
        );
      }
      return print;
    }

    async function reload() {
      const [warehouseList, itemList, customerList, employeeList] = await Promise.all([
        api("GET", "/api/warehouses"),
        api("GET", "/api/items"),
        api("GET", config.partnerApi),
        docType === "SAL_ORDER" ? api("GET", "/api/employees") : Promise.resolve([]),
      ]);
      warehouses = warehouseList;
      items = itemList;
      customers = customerList;
      const warehouseOptions = warehouses
        .filter((warehouse) => warehouse.status)
        .map(
          (warehouse) =>
            `<option value="${warehouse.id}">${escapeHtml(warehouse.code + " " + warehouse.name)}</option>`
        )
        .join("");
      warehouseSelect.innerHTML = warehouseOptions;
      convertWarehouse.innerHTML = warehouseOptions;
      customerSelect.innerHTML =
        '<option value="">不指定</option>' +
        customers
          .filter((customer) => customer.status)
          .map(
            (customer) =>
              `<option value="${customer.id}" data-name="${escapeHtml(customer.name)}">${escapeHtml(
                customer.code + " " + customer.name
              )}</option>`
          )
          .join("");
      if (salespersonSelect) {
        salespersonSelect.innerHTML =
          '<option value="0">未指定</option>' +
          employeeList
            .map(
              (person) =>
                `<option value="${person.id}">${escapeHtml(
                  person.code + " " + person.name
                )}${person.status ? "" : "（停用）"}</option>`
            )
            .join("");
      }
      await documentList.reload();
      if (!linesTable.querySelector("tbody") || !linesTable.querySelector("tbody").children.length) {
        resetLines();
      }
    }

    function resetForm() {
      editingId = 0;
      editingVersion = -1;
      requestId = newRequestId();
      formDirty = false;
      // 确认保存成功、取消编辑都清掉本地草稿。
      clearDraft(docType);
      mount.querySelector('[data-role="cancel-edit"]').hidden = true;
      customerSelect.value = "";
      if (salespersonSelect) salespersonSelect.value = "0";
      remarkInput.value = "";
      dateInput.value = new Date().toISOString().slice(0, 10);
      resetLines();
    }

    // 订单本地草稿：只存业务字段，按订单类型分键。
    function draftFormData() {
      const data = {
        warehouse_id: Number(warehouseSelect.value || 0),
        doc_date: dateInput.value,
        partner_id: Number(customerSelect.value || 0),
        salesperson_id: salespersonSelect ? Number(salespersonSelect.value || 0) : 0,
        remark: remarkInput.value.trim(),
        lines: [],
      };
      for (const row of linesTable.querySelectorAll("tbody tr")) {
        const itemId = Number(row.querySelector('[data-role="item"]').value);
        if (!itemId) continue;
        data.lines.push({
          item_id: itemId,
          qty: row.querySelector('[data-role="qty"]').value.trim(),
          price: row.querySelector('[data-role="price"]').value,
        });
      }
      return data;
    }

    function saveDraftNow() {
      if (editingId || !formDirty) return false;
      writeDraft(docType, {
        savedAt: Date.now(),
        enteredAt: draftEnteredAt,
        form: draftFormData(),
      });
      return true;
    }

    function applyDraft(draft) {
      const data = draft.form;
      editingId = 0;
      editingVersion = -1;
      requestId = newRequestId();
      mount.querySelector('[data-role="cancel-edit"]').hidden = true;
      if (data.warehouse_id) warehouseSelect.value = String(data.warehouse_id);
      if (data.doc_date) dateInput.value = data.doc_date;
      customerSelect.value = data.partner_id ? String(data.partner_id) : "";
      if (salespersonSelect) salespersonSelect.value = String(data.salesperson_id || 0);
      remarkInput.value = data.remark || "";
      linesTable.innerHTML = `<thead><tr><th>商品</th><th>数量</th><th>单价（元）</th><th>金额（元）</th><th class="actions">操作</th></tr></thead><tbody></tbody>`;
      for (const line of data.lines || []) {
        addLine({
          item_id: line.item_id,
          qty: parseQty(line.qty, true) || "0",
          price_cents: Math.round(Number(line.price || 0) * 100),
        });
      }
      if (!linesTable.querySelector("tbody tr")) addLine();
      formDirty = true;
    }

    async function restoreDraftIfAny() {
      const draft = readDraft(docType);
      if (!draft) return;
      if (!(Number(draft.savedAt) > Number(draft.enteredAt || 0))) {
        clearDraft(docType);
        return;
      }
      const when = new Date(Number(draft.savedAt));
      const pad = (value) => String(value).padStart(2, "0");
      const stamp =
        pad(when.getMonth() + 1) +
        "-" +
        pad(when.getDate()) +
        " " +
        pad(when.getHours()) +
        ":" +
        pad(when.getMinutes());
      const restore = await askConfirm(
        "发现「" + config.title + "」未提交的本地草稿（保存于 " + stamp + "），是否恢复？",
        { okText: "恢复", cancelText: "放弃" }
      );
      if (!restore) {
        clearDraft(docType);
        return;
      }
      applyDraft(draft);
      showNotice("已恢复本地草稿，可继续填写", "ok");
    }

    function collectLines() {
      const lines = [];
      for (const row of linesTable.querySelectorAll("tbody tr")) {
        const itemId = Number(row.querySelector('[data-role="item"]').value);
        const qty = parseQty(
          row.querySelector('[data-role="qty"]').value,
          docType === "STK_CNT"
        );
        const price = Math.round(Number(row.querySelector('[data-role="price"]').value || 0) * 100);
        if (!itemId || qty === null) {
          showNotice("请检查明细行的商品与数量", "error");
          return null;
        }
        lines.push({ item_id: itemId, qty, price_cents: price });
      }
      if (!lines.length) {
        showNotice("请至少添加一行明细", "error");
        return null;
      }
      return lines;
    }

    async function save(draft) {
      const lines = collectLines();
      if (!lines) return;
      const option = customerSelect.selectedOptions[0];
      const body = {
        doc_type: docType,
        warehouse_id: Number(warehouseSelect.value),
        doc_date: dateInput.value,
        partner_id: Number(customerSelect.value || 0),
        partner: option && option.value ? option.dataset.name || "" : "",
        salesperson_id: salespersonSelect ? Number(salespersonSelect.value || 0) : 0,
        remark: remarkInput.value.trim(),
        lines,
      };
      try {
        if (editingId) {
          const updated = await api("PUT", "/api/documents/" + editingId, {
            ...body,
            version: editingVersion,
          });
          editingVersion = updated ? updated.version ?? editingVersion : editingVersion;
          if (!draft) {
            await api("POST", "/api/documents/" + editingId + "/approve", {
              version: editingVersion,
            });
          }
          showNotice(draft ? "订单草稿已保存" : "订单已确认", "ok");
        } else {
          const result = await api("POST", "/api/documents", {
            ...body,
            save_as_draft: draft,
            request_id: requestId,
          });
          showNotice((draft ? "已存为草稿：" : "订单已确认：") + result.doc_no, "ok");
        }
        resetForm();
        await reload();
      } catch (error) {
        showNotice(error.message, "error");
      }
    }

    async function startEdit(doc) {
      try {
        const detail = await api("GET", "/api/documents/" + doc.id);
        editingId = doc.id;
        editingVersion = detail.version ?? -1;
        warehouseSelect.value = String(detail.warehouse_id);
        dateInput.value = detail.doc_date;
        customerSelect.value = detail.partner_id ? String(detail.partner_id) : "";
        if (salespersonSelect) salespersonSelect.value = String(detail.salesperson_id || 0);
        remarkInput.value = detail.remark || "";
        linesTable.innerHTML = `<thead><tr><th>商品</th><th>数量</th><th>单价（元）</th><th>金额（元）</th><th class="actions">操作</th></tr></thead><tbody></tbody>`;
        for (const line of detail.lines) addLine(line);
        formDirty = false;
        mount.querySelector('[data-role="cancel-edit"]').hidden = false;
        form.scrollIntoView({ behavior: "smooth", block: "start" });
      } catch (error) {
        showNotice(error.message, "error");
      }
    }

    async function openConvert(doc) {
      try {
        const detail = await api("GET", "/api/documents/" + doc.id);
        convertingId = doc.id;
        convertTitle.textContent =
          detail.doc_no + " · " + (detail.partner || "未指定" + config.partnerLabel);
        convertWarehouse.value = String(detail.warehouse_id);
        convertDate.value = new Date().toISOString().slice(0, 10);
        convertLines.innerHTML = `
          <thead><tr><th>商品</th><th>订单数量</th><th>已下推</th><th>剩余</th><th>本次数量</th><th>单价（元）</th></tr></thead>
          <tbody>${detail.lines
            .map((line) => {
              const remaining = Number(line.remaining_qty);
              return `<tr data-line-id="${line.id}" data-price-cents="${line.price_cents}">
                <td>${escapeHtml(itemLabelById(line.item_id))}</td>
                <td>${qtyText(line.qty)}</td>
                <td>${qtyText(line.converted_qty)}</td>
                <td>${qtyText(line.remaining_qty)}</td>
                <td><input type="text" inputmode="decimal"
                  value="${qtyText(line.remaining_qty)}" data-role="convert-qty" ${remaining <= 0 ? "disabled" : ""} /></td>
                <td><input type="number" min="0" step="0.01"
                  value="${(line.price_cents / 100).toFixed(2)}" data-role="convert-price" ${
                    remaining <= 0 ? "disabled" : ""
                  } /></td>
              </tr>`;
            })
            .join("")}</tbody>`;
        convertCard.hidden = false;
        convertCard.scrollIntoView({ behavior: "smooth", block: "start" });
      } catch (error) {
        showNotice(error.message, "error");
      }
    }

    async function submitConvert() {
      const lines = [];
      for (const row of convertLines.querySelectorAll("tbody tr")) {
        const input = row.querySelector('[data-role="convert-qty"]');
        if (input.disabled) continue;
        const qty = parseQty(input.value, true);
        if (qty === null) {
          showNotice("数量格式不正确，最多 4 位小数", "error");
          return;
        }
        if (qty === "0") continue;
        const priceInput = row.querySelector('[data-role="convert-price"]');
        const priceCents = Math.round(Number(priceInput.value || 0) * 100);
        if (!Number.isFinite(priceCents) || priceCents < 0) {
          showNotice("单价必须是不小于 0 的数字", "error");
          return;
        }
        const entry = { order_line_id: Number(row.dataset.lineId), qty };
        // 价格未改动时不传，交给后端沿用订单价。
        if (priceCents !== Number(row.dataset.priceCents)) entry.price_cents = priceCents;
        lines.push(entry);
      }
      if (!lines.length) {
        showNotice("请填写要下推的数量", "error");
        return;
      }
      try {
        const result = await api("POST", "/api/documents/" + convertingId + "/convert", {
          warehouse_id: Number(convertWarehouse.value || 0),
          doc_date: convertDate.value,
          lines,
        });
        showNotice(config.convertNotice + result.doc_no, "ok");
        convertCard.hidden = true;
        convertingId = 0;
        await reload();
      } catch (error) {
        showNotice(error.message, "error");
      }
    }

    mount.querySelector('[data-role="add-line"]').addEventListener("click", () => addLine());
    mount.querySelector('[data-role="refresh"]').addEventListener("click", () => reload());
    // 订单表单只有改过才写本地草稿。
    form.addEventListener("input", () => {
      formDirty = true;
    });
    form.addEventListener("change", () => {
      formDirty = true;
    });
    mount.querySelector('[data-role="cancel-edit"]').addEventListener("click", resetForm);
    mount.querySelector('[data-role="save-draft"]').addEventListener("click", () => save(true));
    mount.querySelector('[data-role="submit"]').addEventListener("click", () => save(false));
    mount.querySelector('[data-role="convert-submit"]').addEventListener("click", submitConvert);
    mount.querySelector('[data-role="convert-cancel"]').addEventListener("click", () => {
      convertCard.hidden = true;
      convertingId = 0;
    });

    documentsTable.addEventListener("click", async (event) => {
      const button = event.target.closest("button");
      if (!button) return;
      const id = Number(event.target.closest("tr").dataset.id);
      try {
        if (button.dataset.role === "print") {
          await printTarget(
            "/api/print/render?target_type=DOC&target_key=" +
              docType +
              "&doc_id=" +
              id
          );
          return;
        }
        if (button.dataset.role === "pdf") {
          const doc = documentList.rowById(id);
          await exportPdfTarget(
            "/api/print/render?target_type=DOC&target_key=" + docType + "&doc_id=" + id,
            doc ? doc.doc_no : undefined
          );
          return;
        }
        if (button.dataset.role === "approve") {
          if (!(await askConfirm(`确认订单后不可再改单，只能${config.convertTitle}或关闭。继续？`))) return;
          const doc = documentList.rowById(id);
          await api(
            "POST",
            "/api/documents/" + id + "/approve",
            doc ? { version: doc.version } : {}
          );
          showNotice("订单已确认", "ok");
        } else if (button.dataset.role === "edit") {
          await startEdit(documentList.rowById(id));
          return;
        } else if (button.dataset.role === "delete") {
          if (!(await askConfirm("确认删除这张订单草稿？"))) return;
          const doc = documentList.rowById(id);
          await api(
            "DELETE",
            "/api/documents/" + id + (doc ? "?version=" + doc.version : "")
          );
          if (editingId === id) resetForm();
          showNotice("订单草稿已删除", "ok");
        } else if (button.dataset.role === "convert") {
          await openConvert(documentList.rowById(id));
          return;
        } else if (button.dataset.role === "close") {
          if (!(await askConfirm(`关闭后订单不能再${config.convertTitle}，确认？`))) return;
          await api("POST", "/api/documents/" + id + "/close");
          showNotice("订单已关闭", "ok");
        }
        await reload();
      } catch (error) {
        showNotice(error.message, "error");
      }
    });

    await reload();
    startDraftTimer(saveDraftNow);
    await restoreDraftIfAny();
  }

  // ---------- 库存页面 ----------

  async function financePage(mount) {
    const today = new Date().toISOString().slice(0, 10);
    mount.innerHTML = `
      <div class="page-head">
        <div>
          <h2>收付款</h2>
          <p class="page-sub">可全额核销、部分核销或作为预收/预付；撤销通过负数记录回冲，不删除历史</p>
          <p class="page-sub">统计范围：仅统计 v7 迁移之后审核的单据，升级前已审核单据不计入往来与账龄</p>
        </div>
        <div class="toolbar">
          <select data-role="direction">
            <option value="1">收款</option>
            <option value="-1">付款</option>
          </select>
          <button class="btn" data-role="refresh">刷新</button>
        </div>
      </div>
      <div class="card">
        <h3>新建收付款单</h3>
        <div class="grid-form">
          <label class="field"><span>往来单位</span><select data-role="partner"></select></label>
          <label class="field"><span>资金账户</span><select data-role="account"></select></label>
          <label class="field"><span>结算方式</span>
            <select data-role="method">
              <option value="CASH">现金</option>
              <option value="BANK">银行</option>
              <option value="WECHAT">微信</option>
              <option value="ALIPAY">支付宝</option>
              <option value="OTHER">其他</option>
            </select>
          </label>
          <label class="field"><span>金额（元）</span><input type="text" inputmode="decimal" data-role="amount" value="0.00" /></label>
          <label class="field"><span>日期</span><input type="date" data-role="date" value="${today}" /></label>
          <label class="field"><span>经办人</span><input type="text" data-role="operator" placeholder="默认当前用户" /></label>
          <label class="field"><span>备注</span><input type="text" data-role="note" /></label>
        </div>
        <p class="page-sub">分摊明细可留空（预收/预付）或只勾选部分单据；分摊合计不能超过单据金额</p>
        <table class="table" data-role="allocations"></table>
        <div class="toolbar">
          <button class="btn" data-role="submit">保存收付款单</button>
        </div>
      </div>
      <div class="card">
        <h3>未核销单据</h3>
        <table class="table" data-role="rows"></table>
      </div>
      <div class="card">
        <h3>收付款记录</h3>
        <table class="table" data-role="settlements"></table>
      </div>`;

    const directionSelect = mount.querySelector('[data-role="direction"]');
    const partnerSelect = mount.querySelector('[data-role="partner"]');
    const accountSelect = mount.querySelector('[data-role="account"]');
    const methodSelect = mount.querySelector('[data-role="method"]');
    const amountInput = mount.querySelector('[data-role="amount"]');
    const dateInput = mount.querySelector('[data-role="date"]');
    const operatorInput = mount.querySelector('[data-role="operator"]');
    const noteInput = mount.querySelector('[data-role="note"]');
    const rowsTable = mount.querySelector('[data-role="rows"]');
    const settlementsTable = mount.querySelector('[data-role="settlements"]');
    const allocationsTable = mount.querySelector('[data-role="allocations"]');
    let entries = [];
    let accounts = [];
    let partners = [];
    let allocations = [];

    const methodText = (value) =>
      ({ CASH: "现金", BANK: "银行", WECHAT: "微信", ALIPAY: "支付宝", OTHER: "其他" })[value] ||
      (value ? value : "未记录");
    const accountName = (id) => {
      const found = accounts.find((row) => String(row.id) === String(id));
      return found ? found.name : "未指定账户";
    };

    function renderAllocations() {
      if (!allocations.length) {
        allocationsTable.innerHTML =
          '<tbody><tr><td class="empty">该往来单位暂无未核销单据，可直接保存为预收/预付</td></tr></tbody>';
        return;
      }
      allocationsTable.innerHTML = `<thead><tr><th>选择</th><th>单号</th><th>日期</th>
          <th>未核销（元）</th><th>本次分摊（元）</th></tr></thead>
        <tbody>${allocations
          .map(
            (row, index) => `<tr>
              <td><input type="checkbox" data-alloc="${index}" ${row.selected ? "checked" : ""} /></td>
              <td>${escapeHtml(row.doc_no)}</td>
              <td>${escapeHtml(row.biz_date)}</td>
              <td>${money(row.outstanding_cents)}</td>
              <td><input type="text" inputmode="decimal" data-alloc-amount="${index}"
                value="${row.amount}" ${row.selected ? "" : "disabled"} /></td>
            </tr>`
          )
          .join("")}</tbody>`;
      allocationsTable.querySelectorAll("[data-alloc]").forEach((box) => {
        box.addEventListener("change", () => {
          const index = Number(box.dataset.alloc);
          allocations[index].selected = box.checked;
          renderAllocations();
        });
      });
      allocationsTable.querySelectorAll("[data-alloc-amount]").forEach((input) => {
        input.addEventListener("input", () => {
          allocations[Number(input.dataset.allocAmount)].amount = input.value;
        });
      });
    }

    async function loadPartnerOptions() {
      const isReceipt = directionSelect.value === "1";
      const list = await api("GET", isReceipt ? "/api/customers" : "/api/suppliers");
      partners = list;
      const previous = partnerSelect.value;
      const prefix = isReceipt ? "C:" : "S:";
      partnerSelect.innerHTML =
        '<option value="">请选择</option>' +
        list
          .filter((row) => row.status)
          .map(
            (row) =>
              `<option value="${prefix}${row.id}">${escapeHtml(row.code)} ${escapeHtml(
                row.name
              )}</option>`
          )
          .join("");
      if (previous && list.some((row) => prefix + row.id === previous)) {
        partnerSelect.value = previous;
      }
    }

    async function loadAccounts() {
      accounts = await api("GET", "/api/fund-accounts");
      const active = accounts.filter((row) => row.status);
      const previous = accountSelect.value;
      accountSelect.innerHTML = active.length
        ? active
            .map(
              (row) =>
                `<option value="${row.id}">${escapeHtml(row.code)} ${escapeHtml(row.name)}</option>`
            )
            .join("")
        : '<option value="">请先在「资金账户」建档</option>';
      if (previous && active.some((row) => String(row.id) === previous)) {
        accountSelect.value = previous;
      }
    }

    async function loadEntries() {
      const partnerKey = partnerSelect.value;
      if (!partnerKey) {
        entries = [];
        allocations = [];
      } else {
        entries = await api(
          "GET",
          "/api/ar-ap?direction=" +
            directionSelect.value +
            "&partner_key=" +
            encodeURIComponent(partnerKey)
        );
        allocations = entries
          .filter((row) => row.outstanding_cents !== 0 && row.status !== "VOIDED")
          .map((row) => ({
            ...row,
            selected: false,
            amount: (Math.abs(row.outstanding_cents) / 100).toFixed(2),
          }));
      }
      const usable = entries.filter((row) => row.status !== "VOIDED");
      rowsTable.innerHTML = !usable.length
        ? '<tbody><tr><td class="empty">暂无未核销单据</td></tr></tbody>'
        : `<thead><tr><th>单号</th><th>类型</th><th>往来单位</th><th>日期</th>
             <th>金额（元）</th><th>已核销</th><th>未核销</th><th>状态</th><th></th></tr></thead>
           <tbody>${usable
             .map(
               (row) => `<tr>
                 <td>${escapeHtml(row.doc_no)}</td>
                 <td>${escapeHtml(row.doc_type)}</td>
                 <td>${escapeHtml(row.partner_name || "未指定")}</td>
                 <td>${escapeHtml(row.biz_date)}</td>
                 <td>${money(row.amount_cents)}</td>
                 <td>${money(row.settled_cents)}</td>
                 <td>${money(row.outstanding_cents)}</td>
                 <td>${escapeHtml(row.status)}</td>
                 <td>${
                   row.outstanding_cents !== 0
                     ? `<button class="btn small" data-settle="${row.id}">核销</button>`
                     : ""
                 }</td>
               </tr>`
             )
             .join("")}</tbody>`;
      renderAllocations();
    }

    async function reload() {
      await Promise.all([loadPartnerOptions(), loadAccounts()]);
      const settlements = await api(
        "GET",
        "/api/settlements?direction=" + directionSelect.value
      );
      await loadEntries();
      settlementsTable.innerHTML = !settlements.length
        ? '<tbody><tr><td class="empty">暂无收付款记录</td></tr></tbody>'
        : `<thead><tr><th>单号</th><th>类型</th><th>往来单位</th><th>账户</th>
             <th>方式</th><th>经办人</th><th>日期</th><th>金额（元）</th><th>备注</th><th></th></tr></thead>
           <tbody>${settlements
             .map(
               (row, index) => `<tr>
                 <td>${escapeHtml(row.settle_no)}</td>
                 <td>${row.direction === 1 ? "收款" : "付款"}</td>
                 <td>${escapeHtml(row.partner_name || "未指定")}</td>
                 <td>${escapeHtml(accountName(row.account_id))}</td>
                 <td>${escapeHtml(methodText(row.method))}</td>
                 <td>${escapeHtml(row.operator || "")}</td>
                 <td>${escapeHtml(row.settled_on)}</td>
                 <td>${money(row.amount_cents)}</td>
                 <td>${escapeHtml(row.note)}</td>
                 <td>${
                   row.reverses_id === 0 && row.amount_cents > 0
                     ? `<button class="btn small" data-reverse="${index}">撤销</button>`
                     : ""
                 }</td>
               </tr>`
             )
             .join("")}</tbody>`;
      settlements.forEach((row, index) => {
        const button = settlementsTable.querySelector(`[data-reverse="${index}"]`);
        if (!button) return;
        button.addEventListener("click", async () => {
          if (!(await askConfirm(`撤销收付款 ${row.settle_no}？`))) return;
          try {
            await api("POST", `/api/settlements/${row.id}/reverse`, {});
            await reload();
          } catch (error) {
            await askAlert(error.message);
          }
        });
      });
    }

    rowsTable.addEventListener("click", async (event) => {
      const button = event.target.closest("[data-settle]");
      if (!button) return;
      const entry = entries.find((row) => String(row.id) === button.dataset.settle);
      if (!entry) return;
      const defaultAmount = (Math.abs(entry.outstanding_cents) / 100).toFixed(2);
      const input = await askPrompt("本次核销金额（元）", defaultAmount);
      if (input === null) return;
      const value = Number(input);
      if (!Number.isFinite(value) || value <= 0) {
        await askAlert("请输入大于 0 的金额");
        return;
      }
      const cents = Math.round(value * 100) * Math.sign(entry.outstanding_cents);
      try {
        await api("POST", "/api/settlements", {
          direction: Number(directionSelect.value),
          partner_key: entry.partner_key,
          amount_cents: Math.abs(cents),
          account_id: Number(accountSelect.value || 0),
          method: methodSelect.value,
          allocations: [{ entry_id: entry.id, amount_cents: cents }],
          note: "",
        });
        await reload();
      } catch (error) {
        await askAlert(error.message);
      }
    });

    partnerSelect.addEventListener("change", loadEntries);
    directionSelect.addEventListener("change", reload);
    mount.querySelector('[data-role="refresh"]').addEventListener("click", reload);
    mount.querySelector('[data-role="submit"]').addEventListener("click", async () => {
      const partnerKey = partnerSelect.value;
      if (!partnerKey) {
        await askAlert("请选择往来单位");
        return;
      }
      if (!accountSelect.value) {
        await askAlert("请先在「资金账户」建档并启用");
        return;
      }
      const amount = Number(amountInput.value);
      if (!Number.isFinite(amount) || amount <= 0) {
        await askAlert("请输入大于 0 的金额");
        return;
      }
      const amountCents = Math.round(amount * 100);
      const payload = [];
      let allocated = 0;
      for (const row of allocations) {
        if (!row.selected) continue;
        const lineAmount = Number(row.amount);
        if (!Number.isFinite(lineAmount) || lineAmount <= 0) {
          await askAlert("分摊金额必须大于 0");
          return;
        }
        const cents = Math.round(lineAmount * 100) * Math.sign(row.outstanding_cents);
        allocated += Math.abs(cents);
        payload.push({ entry_id: row.id, amount_cents: cents });
      }
      if (allocated > amountCents) {
        await askAlert("分摊合计不能超过收付款金额");
        return;
      }
      const partner = partners.find((row) => String(row.id) === partnerKey.slice(2));
      try {
        await api("POST", "/api/settlements", {
          direction: Number(directionSelect.value),
          partner_key: partnerKey,
          partner_name: partner ? partner.name : "",
          amount_cents: amountCents,
          account_id: Number(accountSelect.value),
          method: methodSelect.value,
          operator: operatorInput.value.trim(),
          settled_on: dateInput.value,
          note: noteInput.value.trim(),
          allocations: payload,
        });
        amountInput.value = "0.00";
        noteInput.value = "";
        await reload();
        showNotice("收付款单已保存", "ok");
      } catch (error) {
        await askAlert(error.message);
      }
    });
    await reload();
  }

  // ---------- 报表页面 ----------

  async function reportPage(mount) {
    const today = new Date().toISOString().slice(0, 10);
    const profitAllowed = editionAllows("report.profit");
    mount.innerHTML = `
      <div class="page-head">
        <div>
          <h2>报表</h2>
          <p class="page-sub">只读汇总；期间为含首含尾的自然日；导出金额单位为元</p>
        </div>
        <div class="toolbar">
          <input type="date" data-role="from" value="${today.slice(0, 8)}01" />
          <input type="date" data-role="to" value="${today}" />
          <select data-role="direction">
            <option value="1">应收账龄</option>
            <option value="-1">应付账龄</option>
          </select>
          <button class="btn" data-role="refresh">刷新</button>
        </div>
      </div>
      <div class="card">
        <div class="card-head">
          <h3>资金流水</h3>
          <select data-role="fund-account"></select>
          <button class="btn small" data-role="export-fund">导出 CSV</button>
          <button class="btn small" data-role="excel-fund">导出 Excel</button>
          <button class="btn small" data-role="print-fund">打印</button>
          <button class="btn small" data-role="pdf-fund">导出 PDF</button>
        </div>
        <p class="page-sub">期初 + 收款 − 付款 = 余额；「未指定账户」是升级前没有账户归属的历史记录</p>
        <table class="table" data-role="fund-ledger"></table>
      </div>
      <div class="card">
        <div class="card-head">
          <h3>账户余额一览</h3>
          <button class="btn small" data-role="export-fund-balances">导出 CSV</button>
          <button class="btn small" data-role="excel-fund-balances">导出 Excel</button>
        </div>
        <p class="page-sub">期初 + 累计收入 − 累计支出，截至查询结束日期</p>
        <table class="table" data-role="fund-balances"></table>
      </div>
      <div class="card">
        <div class="card-head">
          <h3>库存余额表</h3>
          <select data-role="balance-warehouse"></select>
          <select data-role="balance-item"></select>
          <label class="checkbox"><input type="checkbox" data-role="balance-hide-zero" />隐藏零库存</label>
          <button class="btn small" data-role="export-balance">导出 CSV</button>
          <button class="btn small" data-role="excel-balance">导出 Excel</button>
          <button class="btn small" data-role="print-balance">打印</button>
          <button class="btn small" data-role="pdf-balance">导出 PDF</button>
        </div>
        <table class="table" data-role="balances"></table>
      </div>
      <div class="card">
        <div class="card-head">
          <h3>库存流水明细</h3>
          <select data-role="ledger-flow">
            <option value="">全部方向</option>
            <option value="in">仅入库</option>
            <option value="out">仅出库</option>
          </select>
          <select data-role="ledger-category">
            <option value="">全部业务类型</option>
            <option value="GIFT">赠品</option>
            <option value="USAGE">领用</option>
            <option value="LOSS">报损</option>
            <option value="OPENING">期初</option>
            <option value="OTHER">其他</option>
          </select>
          <button class="btn small" data-role="export-ledger">导出 CSV</button>
          <button class="btn small" data-role="excel-ledger">导出 Excel</button>
          <button class="btn small" data-role="print-ledger">打印</button>
          <button class="btn small" data-role="pdf-ledger">导出 PDF</button>
        </div>
        <p class="page-sub" data-role="ledger-hint"></p>
        <div class="pager" data-role="ledger-pager"></div>
        <table class="table" data-role="ledger"></table>
      </div>
      <div class="card">
        <div class="card-head">
          <h3>收发明细</h3>
          <button class="btn small" data-role="export-flows">导出 CSV</button>
          <button class="btn small" data-role="excel-flows">导出 Excel</button>
          <button class="btn small" data-role="print-flows">打印</button>
          <button class="btn small" data-role="pdf-flows">导出 PDF</button>
        </div>
        <table class="table" data-role="flows"></table>
      </div>
      <div class="card">
        <div class="card-head">
          <h3>进销存汇总</h3>
          <button class="btn small" data-role="export-summary">导出 CSV</button>
          <button class="btn small" data-role="excel-summary">导出 Excel</button>
          <button class="btn small" data-role="print-summary">打印</button>
          <button class="btn small" data-role="pdf-summary">导出 PDF</button>
        </div>
        <p class="page-sub">按商品跨仓库汇总：期初 + 入库 − 出库 = 期末，数量与金额双口径</p>
        <table class="table" data-role="summary"></table>
      </div>
      ${
        profitAllowed
          ? `<div class="card">
        <div class="card-head">
          <h3>销售毛利</h3>
          <button class="btn small" data-role="export-profit">导出 CSV</button>
          <button class="btn small" data-role="excel-profit">导出 Excel</button>
          <button class="btn small" data-role="print-profit">打印</button>
          <button class="btn small" data-role="pdf-profit">导出 PDF</button>
        </div>
        <p class="page-sub">收入取单据金额，成本取审核写入的库存流水；已反审核单据不计入</p>
        <table class="table" data-role="profit"></table>
      </div>`
          : ""
      }
      <div class="card">
        <div class="card-head">
          <h3>采购报表</h3>
          <select data-role="purchase-by">
            <option value="supplier">汇总维度：按供应商</option>
            <option value="item">汇总维度：按商品</option>
          </select>
        </div>
        <p class="page-sub">期间与页首一致；包含采购入库与采购退货，退货数量与金额按负数冲减，仅统计已审核单据</p>
        <div class="toolbar">
          <select data-role="purchase-supplier"></select>
          <select data-role="purchase-item"></select>
          <select data-role="purchase-warehouse"></select>
        </div>
        <div class="card-head">
          <h3>采购明细</h3>
          <span class="toolbar">
            <button class="btn small" data-role="export-purchase-detail">导出 CSV</button>
            <button class="btn small" data-role="excel-purchase-detail">导出 Excel</button>
            <button class="btn small" data-role="print-purchase-detail">打印</button>
            <button class="btn small" data-role="pdf-purchase-detail">导出 PDF</button>
          </span>
        </div>
        <p class="page-sub" data-role="purchase-detail-hint"></p>
        <div class="pager" data-role="purchase-pager"></div>
        <table class="table" data-role="purchase-detail"></table>
        <div class="card-head">
          <h3>采购汇总</h3>
          <span class="toolbar">
            <button class="btn small" data-role="export-purchase-summary">导出 CSV</button>
            <button class="btn small" data-role="excel-purchase-summary">导出 Excel</button>
            <button class="btn small" data-role="print-purchase-summary">打印</button>
            <button class="btn small" data-role="pdf-purchase-summary">导出 PDF</button>
          </span>
        </div>
        <table class="table" data-role="purchase-summary"></table>
      </div>
      <div class="card">
        <div class="card-head">
          <h3>其他出入库汇总</h3>
          <span class="toolbar">
            <button class="btn small" data-role="export-other-flow">导出 CSV</button>
            <button class="btn small" data-role="excel-other-flow">导出 Excel</button>
            <button class="btn small" data-role="print-other-flow">打印</button>
            <button class="btn small" data-role="pdf-other-flow">导出 PDF</button>
          </span>
        </div>
        <p class="page-sub">期间与页首一致；只统计已审核单据，出库数量与金额取绝对值</p>
        <div class="toolbar">
          <select data-role="other-flow-category">
            <option value="">全部业务类型</option>
            <option value="GIFT">赠品</option>
            <option value="USAGE">领用</option>
            <option value="LOSS">报损</option>
            <option value="OPENING">期初</option>
            <option value="OTHER">其他</option>
          </select>
          <select data-role="other-flow-warehouse"></select>
        </div>
        <table class="table" data-role="other-flow"></table>
      </div>
      <div class="card">
        <div class="card-head">
          <h3>应收应付余额与账龄</h3>
          <button class="btn small" data-role="export-aging">导出 CSV</button>
          <button class="btn small" data-role="excel-aging">导出 Excel</button>
          <button class="btn small" data-role="print-aging">打印</button>
          <button class="btn small" data-role="pdf-aging">导出 PDF</button>
        </div>
        <p class="page-sub">统计范围：仅 v7 迁移后审核的单据，升级前已审核单据不计入往来与账龄；上方库存类报表不受影响</p>
        <table class="table" data-role="aging"></table>
      </div>`;

    const fromInput = mount.querySelector('[data-role="from"]');
    const toInput = mount.querySelector('[data-role="to"]');
    const directionSelect = mount.querySelector('[data-role="direction"]');
    const balancesTable = mount.querySelector('[data-role="balances"]');
    const balanceWarehouseSelect = mount.querySelector('[data-role="balance-warehouse"]');
    const balanceItemSelect = mount.querySelector('[data-role="balance-item"]');
    const balanceHideZero = mount.querySelector('[data-role="balance-hide-zero"]');
    const ledgerTable = mount.querySelector('[data-role="ledger"]');
    const ledgerFlowSelect = mount.querySelector('[data-role="ledger-flow"]');
    const ledgerCategorySelect = mount.querySelector('[data-role="ledger-category"]');
    const ledgerHint = mount.querySelector('[data-role="ledger-hint"]');
    const ledgerPager = mount.querySelector('[data-role="ledger-pager"]');
    const purchasePager = mount.querySelector('[data-role="purchase-pager"]');
    const flowsTable = mount.querySelector('[data-role="flows"]');
    const summaryTable = mount.querySelector('[data-role="summary"]');
    const profitTable = mount.querySelector('[data-role="profit"]');
    const agingTable = mount.querySelector('[data-role="aging"]');
    const fundAccountSelect = mount.querySelector('[data-role="fund-account"]');
    const fundLedgerTable = mount.querySelector('[data-role="fund-ledger"]');
    const fundBalancesTable = mount.querySelector('[data-role="fund-balances"]');
    const purchaseBySelect = mount.querySelector('[data-role="purchase-by"]');
    const purchaseSupplierSelect = mount.querySelector('[data-role="purchase-supplier"]');
    const purchaseItemSelect = mount.querySelector('[data-role="purchase-item"]');
    const purchaseWarehouseSelect = mount.querySelector('[data-role="purchase-warehouse"]');
    const purchaseDetailTable = mount.querySelector('[data-role="purchase-detail"]');
    const purchaseDetailHint = mount.querySelector('[data-role="purchase-detail-hint"]');
    const purchaseSummaryTable = mount.querySelector('[data-role="purchase-summary"]');
    const otherFlowCategorySelect = mount.querySelector('[data-role="other-flow-category"]');
    const otherFlowWarehouseSelect = mount.querySelector('[data-role="other-flow-warehouse"]');
    const otherFlowTable = mount.querySelector('[data-role="other-flow"]');

    function periodQuery() {
      return (
        "?from=" + encodeURIComponent(fromInput.value) + "&to=" + encodeURIComponent(toInput.value)
      );
    }

    // 库存流水明细按出入库方向与业务类型筛选，导出与打印共用。
    // 报表真分页：页面每页 200 行，导出仍取全量。
    const PAGE_SIZE = 200;
    let ledgerPage = 1;
    let purchasePage = 1;
    let ledgerCached = { total: 0, offset: 0, rows: [] };

    function renderPager(element, data, page, onPage) {
      const total = Number(data.total || 0);
      const offset = Number(data.offset || 0);
      const count = (data.rows || []).length;
      if (total <= PAGE_SIZE) {
        element.innerHTML = "";
        return;
      }
      const pages = Math.max(1, Math.ceil(total / PAGE_SIZE));
      element.innerHTML = `<button class="btn" type="button" data-page="${page - 1}" ${
        page <= 1 ? "disabled" : ""
      }>上一页</button>
        <span>第 ${page} / ${pages} 页（共 ${total} 行）</span>
        <button class="btn" type="button" data-page="${page + 1}" ${
          page >= pages ? "disabled" : ""
        }>下一页</button>`;
      element.onclick = (event) => {
        const button = event.target.closest("button[data-page]");
        if (!button || button.disabled) return;
        const next = Number(button.dataset.page);
        if (next === page || next < 1) return;
        onPage(next);
      };
      if (count === 0 && page > 1) {
        // 筛选后页码越界，回到最后一页。
        onPage(pages);
      }
    }

    function ledgerQuery() {
      let query = ledgerFlowSelect.value
        ? periodQuery() + "&flow=" + ledgerFlowSelect.value
        : periodQuery();
      if (ledgerCategorySelect.value) {
        query += "&biz_category=" + encodeURIComponent(ledgerCategorySelect.value);
      }
      return query;
    }

    // 库存余额按仓库 / 商品筛选，可选隐藏零库存；导出与打印共用。
    function balanceQuery() {
      return (
        "?warehouse_id=" + encodeURIComponent(balanceWarehouseSelect.value || "0") +
        "&item_id=" + encodeURIComponent(balanceItemSelect.value || "0") +
        "&hide_zero=" + (balanceHideZero.checked ? "1" : "0")
      );
    }

    // 采购明细与采购汇总共用期间与供应商 / 商品 / 仓库筛选。
    function purchaseQuery() {
      return (
        periodQuery() +
        "&supplier_id=" + encodeURIComponent(purchaseSupplierSelect.value || "0") +
        "&item_id=" + encodeURIComponent(purchaseItemSelect.value || "0") +
        "&warehouse_id=" + encodeURIComponent(purchaseWarehouseSelect.value || "0")
      );
    }

    // 其他出入库汇总按业务类型与仓库筛选，导出与打印共用。
    function otherFlowQuery() {
      let query = periodQuery();
      if (otherFlowWarehouseSelect.value) {
        query += "&warehouse_id=" + encodeURIComponent(otherFlowWarehouseSelect.value);
      }
      if (otherFlowCategorySelect.value) {
        query += "&biz_category=" + encodeURIComponent(otherFlowCategorySelect.value);
      }
      return query;
    }

    // 保留上次选择，避免刷新后筛选条件被重置。
    function fillSelect(select, rows, allLabel, label) {
      const previous = select.value;
      select.innerHTML =
        `<option value="0">${allLabel}</option>` +
        rows
          .filter((row) => row.status)
          .map((row) => `<option value="${row.id}">${escapeHtml(label(row))}</option>`)
          .join("");
      if (previous && [...select.options].some((option) => option.value === previous)) {
        select.value = previous;
      }
    }

    async function reload() {
      const period = periodQuery();
      const [accounts, suppliers, items, warehouses] = await Promise.all([
        api("GET", "/api/fund-accounts"),
        api("GET", "/api/suppliers"),
        api("GET", "/api/items"),
        api("GET", "/api/warehouses"),
      ]);
      const previous = fundAccountSelect.value;
      fundAccountSelect.innerHTML =
        '<option value="all">全部账户</option>' +
        accounts
          .map(
            (row) =>
              `<option value="${row.id}">${escapeHtml(row.code)} ${escapeHtml(row.name)}${
                row.status ? "" : "（停用）"
              }</option>`
          )
          .join("") + '<option value="0">未指定账户</option>';
      // 页面默认「全部账户」，其余情况保留上一次选择。
      fundAccountSelect.value =
        previous && (previous === "all" || previous === "0" ||
                     accounts.some((row) => String(row.id) === previous))
          ? previous
          : "all";
      fillSelect(purchaseSupplierSelect, suppliers, "全部供应商", (row) => row.code + " " + row.name);
      fillSelect(purchaseItemSelect, items, "全部商品", itemOptionText);
      fillSelect(purchaseWarehouseSelect, warehouses, "全部仓库", (row) => row.code + " " + row.name);
      fillSelect(balanceWarehouseSelect, warehouses, "全部仓库", (row) => row.code + " " + row.name);
      fillSelect(balanceItemSelect, items, "全部商品", itemOptionText);
      fillSelect(otherFlowWarehouseSelect, warehouses, "全部仓库", (row) => row.code + " " + row.name);
      const fundQuery =
        period + "&account_id=" + encodeURIComponent(fundAccountSelect.value || "0");
      const purchaseQ = purchaseQuery();
      const [
        balances,
        ledger,
        flows,
        summary,
        profit,
        aging,
        fundLedger,
        fundBalances,
        purchaseDetail,
        purchaseSummary,
        otherFlow,
      ] = await Promise.all([
        api("GET", "/api/reports/stock-balance" + balanceQuery()),
        api(
          "GET",
          "/api/reports/stock-ledger" +
            ledgerQuery() +
            "&limit=" +
            PAGE_SIZE +
            "&offset=" +
            (ledgerPage - 1) * PAGE_SIZE
        ),
        api("GET", "/api/reports/goods-flow" + period),
        api("GET", "/api/reports/inventory-summary" + period),
        profitAllowed ? api("GET", "/api/reports/sales-profit" + period) : Promise.resolve([]),
        api("GET", "/api/reports/ar-ap-aging?direction=" + directionSelect.value),
        api("GET", "/api/reports/fund-ledger" + fundQuery),
        api("GET", "/api/reports/fund-balances?as_of=" + encodeURIComponent(toInput.value)),
        api(
          "GET",
          "/api/reports/purchase-detail" +
            purchaseQ +
            "&limit=" +
            PAGE_SIZE +
            "&offset=" +
            (purchasePage - 1) * PAGE_SIZE
        ),
        api(
          "GET",
          "/api/reports/purchase-summary" + purchaseQ + "&by=" + purchaseBySelect.value
        ),
        api("GET", "/api/reports/other-flow-summary" + otherFlowQuery()),
      ]);

      fundLedgerTable.innerHTML = !fundLedger.length
        ? emptyRow("该账户暂无资金流水", 10)
        : `<thead><tr><th>日期</th><th>类型</th><th>单号</th><th>往来单位</th>
             <th>结算方式</th><th>经办人</th><th>摘要</th><th>收入（元）</th>
             <th>支出（元）</th><th>余额（元）</th></tr></thead>
           <tbody>${fundLedger
             .map(
               (row) => `<tr>
                 <td>${escapeHtml(row.settled_on)}</td>
                 <td>${escapeHtml(row.direction_text)}</td>
                 <td>${escapeHtml(row.settle_no)}</td>
                 <td>${escapeHtml(row.partner_name)}</td>
                 <td>${escapeHtml(row.method_text)}</td>
                 <td>${escapeHtml(row.operator)}</td>
                 <td>${escapeHtml(row.note)}</td>
                 <td>${row.opening ? "" : money(row.income_cents)}</td>
                 <td>${row.opening ? "" : money(row.expense_cents)}</td>
                 <td>${money(row.balance_cents)}</td>
               </tr>`
             )
             .join("")}</tbody>`;

      fundBalancesTable.innerHTML = !fundBalances.length
        ? emptyRow("暂无资金账户", 7)
        : `<thead><tr><th>编码</th><th>名称</th><th>类型</th><th>期初（元）</th>
             <th>累计收入（元）</th><th>累计支出（元）</th><th>余额（元）</th></tr></thead>
           <tbody>${fundBalances
             .map(
               (row) => `<tr>
                 <td>${escapeHtml(row.code)}</td>
                 <td>${escapeHtml(row.name)}</td>
                 <td>${escapeHtml(row.kind_text)}</td>
                 <td>${money(row.opening_cents)}</td>
                 <td>${money(row.inflow_cents)}</td>
                 <td>${money(row.outflow_cents)}</td>
                 <td class="${row.balance_cents < 0 ? "negative" : ""}">${money(
                   row.balance_cents
                 )}</td>
               </tr>`
             )
             .join("")}</tbody>`;

      // 数量跨单位不可加总，页面合计只汇总金额，与 CSV / Excel 一致。
      const balanceAmount = balances.reduce((sum, row) => sum + row.amount_cents, 0);
      balancesTable.innerHTML = !balances.length
        ? emptyRow("暂无库存", 5)
        : `<thead><tr><th>商品编码</th><th>商品名称</th><th>仓库</th><th>数量</th><th>金额（元）</th></tr></thead>
           <tbody>${balances
             .map(
               (row) => `<tr>
                 <td>${escapeHtml(row.item_code)}</td>
                 <td>${escapeHtml(row.item_name)}</td>
                 <td>${escapeHtml(row.warehouse_name)}</td>
                 <td class="${qtyIsNegative(row.qty) ? "negative" : ""}">${qtyText(row.qty)}</td>
                 <td>${money(row.amount_cents)}</td>
               </tr>`
             )
             .join("")}</tbody>
           <tfoot><tr><td>合计 ${balances.length} 行</td><td colspan="3"></td>
             <td>${money(balanceAmount)}</td></tr></tfoot>`;

      const ledgerRows = ledger.rows || [];
      ledgerCached = ledger;
      ledgerHint.textContent =
        ledger.total > ledgerRows.length
          ? `共 ${ledger.total} 行，当前显示第 ${ledger.offset + 1}–${
              ledger.offset + ledgerRows.length
            } 行；可翻页或导出 CSV / Excel 查看全部`
          : `共 ${ledger.total || 0} 行`;
      renderPager(ledgerPager, ledger, ledgerPage, (page) => {
        ledgerPage = page;
        reload();
      });
      ledgerTable.innerHTML = !ledgerRows.length
        ? emptyRow("该期间暂无流水", 8)
        : `<thead><tr><th>时间</th><th>类型</th><th>业务类型</th><th>单据号</th><th>商品</th><th>仓库</th><th>数量</th><th>金额（元）</th></tr></thead>
           <tbody>${ledgerRows
             .map(
               (row) => `<tr>
                 <td>${escapeHtml(row.biz_time)}</td>
                 <td>${escapeHtml(row.biz_type_label || bizTypeText(row.biz_type))}</td>
                 <td>${escapeHtml(bizCategoryText(row.biz_category) || "—")}</td>
                 <td>${escapeHtml(row.doc_no)}</td>
                 <td>${escapeHtml(row.item_code + " " + row.item_name)}</td>
                 <td>${escapeHtml(row.warehouse_name)}</td>
                 <td class="${qtyIsNegative(row.qty) ? "negative" : ""}">${qtyText(row.qty)}</td>
                 <td>${money(row.amount_cents)}</td>
               </tr>`
             )
             .join("")}</tbody>
           <tfoot><tr><td>合计</td><td colspan="5"></td>
             <td>${qtyText(ledger.totals ? ledger.totals.qty : 0)}</td>
             <td>${money(ledger.totals ? ledger.totals.amount_cents : 0)}</td></tr></tfoot>`;

      // 数量跨单位不可加总，合计只汇总收入 / 发出 / 其他调整金额。
      const flowInAmount = flows.reduce((sum, row) => sum + row.in_amount_cents, 0);
      const flowOutAmount = flows.reduce((sum, row) => sum + row.out_amount_cents, 0);
      const flowAdjustAmount = flows.reduce((sum, row) => sum + (row.adjust_amount_cents || 0), 0);
      flowsTable.innerHTML = !flows.length
        ? emptyRow("该期间无收发", 10)
        : `<thead><tr><th>商品编码</th><th>商品名称</th><th>仓库</th><th>期初</th><th>收入</th>
             <th>发出</th><th>期末</th><th>收入金额（元）</th><th>发出金额（元）</th>
             <th>其他调整（元）</th></tr></thead>
           <tbody>${flows
             .map(
               (row) => `<tr>
                 <td>${escapeHtml(row.item_code)}</td>
                 <td>${escapeHtml(row.item_name)}</td>
                 <td>${escapeHtml(row.warehouse_name)}</td>
                 <td>${qtyText(row.opening_qty)}</td>
                 <td>${qtyText(row.in_qty)}</td>
                 <td>${qtyText(row.out_qty)}</td>
                 <td class="${qtyIsNegative(row.closing_qty) ? "negative" : ""}">${qtyText(row.closing_qty)}</td>
                 <td>${money(row.in_amount_cents)}</td>
                 <td>${money(row.out_amount_cents)}</td>
                 <td class="${(row.adjust_amount_cents || 0) < 0 ? "negative" : ""}">${money(
                   row.adjust_amount_cents || 0
                 )}</td>
               </tr>`
             )
             .join("")}</tbody>
           <tfoot><tr><td>合计</td><td colspan="5"></td>
             <td></td>
             <td>${money(flowInAmount)}</td>
             <td>${money(flowOutAmount)}</td>
             <td>${money(flowAdjustAmount)}</td></tr></tfoot>`;

      // 与 CSV / Excel 列保持一致：四个数量列 + 五个金额列；数量跨单位不可加总。
      const sumOpeningAmount = summary.reduce((sum, row) => sum + row.opening_amount_cents, 0);
      const sumInAmount = summary.reduce((sum, row) => sum + row.in_amount_cents, 0);
      const sumOutAmount = summary.reduce((sum, row) => sum + row.out_amount_cents, 0);
      const sumAdjustAmount = summary.reduce((sum, row) => sum + (row.adjust_amount_cents || 0), 0);
      const sumClosingAmount = summary.reduce((sum, row) => sum + row.closing_amount_cents, 0);
      summaryTable.innerHTML = !summary.length
        ? emptyRow("该期间无进销存发生", 12)
        : `<thead><tr><th>商品编码</th><th>商品名称</th><th>单位</th><th>期初数量</th>
             <th>期初金额（元）</th><th>入库数量</th><th>入库金额（元）</th>
             <th>出库数量</th><th>出库金额（元）</th><th>其他调整（元）</th><th>期末数量</th>
             <th>期末金额（元）</th></tr></thead>
           <tbody>${summary
             .map(
               (row) => `<tr>
                 <td>${escapeHtml(row.item_code)}</td>
                 <td>${escapeHtml(row.item_name)}</td>
                 <td>${escapeHtml(row.unit)}</td>
                 <td>${qtyText(row.opening_qty)}</td>
                 <td>${money(row.opening_amount_cents)}</td>
                 <td>${qtyText(row.in_qty)}</td>
                 <td>${money(row.in_amount_cents)}</td>
                 <td>${qtyText(row.out_qty)}</td>
                 <td>${money(row.out_amount_cents)}</td>
                 <td class="${(row.adjust_amount_cents || 0) < 0 ? "negative" : ""}">${money(
                   row.adjust_amount_cents || 0
                 )}</td>
                 <td class="${qtyIsNegative(row.closing_qty) ? "negative" : ""}">${qtyText(row.closing_qty)}</td>
                 <td>${money(row.closing_amount_cents)}</td>
               </tr>`
             )
             .join("")}</tbody>
           <tfoot><tr><td>合计</td><td colspan="2"></td>
             <td></td><td>${money(sumOpeningAmount)}</td>
             <td></td><td>${money(sumInAmount)}</td>
             <td></td><td>${money(sumOutAmount)}</td>
             <td>${money(sumAdjustAmount)}</td>
             <td></td><td>${money(sumClosingAmount)}</td></tr></tfoot>`;

      // 合计行：收入、成本、毛利汇总后重算毛利率；数量跨单位不加总。
      // 社区版不渲染毛利卡片，profitTable 为空时整段跳过。
      if (profitTable) {
      const profitRevenue = profit.reduce((sum, row) => sum + row.revenue_cents, 0);
      const profitCost = profit.reduce((sum, row) => sum + row.cost_cents, 0);
      const profitValue = profit.reduce((sum, row) => sum + row.profit_cents, 0);
      profitTable.innerHTML = !profit.length
        ? emptyRow("该期间无销售", 7)
        : `<thead><tr><th>商品编码</th><th>商品名称</th><th>数量</th><th>销售收入（元）</th>
             <th>销售成本（元）</th><th>毛利（元）</th><th>毛利率</th></tr></thead>
           <tbody>${profit
             .map(
               (row) => `<tr>
                 <td>${escapeHtml(row.item_code)}</td>
                 <td>${escapeHtml(row.item_name)}</td>
                 <td>${qtyText(row.qty)}</td>
                 <td>${money(row.revenue_cents)}</td>
                 <td>${money(row.cost_cents)}</td>
                 <td class="${row.profit_cents < 0 ? "negative" : ""}">${money(row.profit_cents)}</td>
                 <td>${
                   row.revenue_cents === 0
                     ? "—"
                     : ((row.profit_cents / row.revenue_cents) * 100).toFixed(1) + "%"
                 }</td>
               </tr>`
             )
             .join("")}</tbody>
           <tfoot><tr><td>合计</td><td></td><td></td>
             <td>${money(profitRevenue)}</td>
             <td>${money(profitCost)}</td>
             <td class="${profitValue < 0 ? "negative" : ""}">${money(profitValue)}</td>
             <td>${profitRevenue === 0 ? "—" : ((profitValue / profitRevenue) * 100).toFixed(1) + "%"}</td>
           </tr></tfoot>`;
      }

      const agingTotal = aging.reduce((sum, row) => sum + row.total_cents, 0);
      const agingNotDue = aging.reduce((sum, row) => sum + row.not_due_cents, 0);
      const agingD1 = aging.reduce((sum, row) => sum + row.d1_30_cents, 0);
      const agingD31 = aging.reduce((sum, row) => sum + row.d31_60_cents, 0);
      const agingD61 = aging.reduce((sum, row) => sum + row.d61_90_cents, 0);
      const agingD90 = aging.reduce((sum, row) => sum + row.d90_plus_cents, 0);
      agingTable.innerHTML = !aging.length
        ? emptyRow("暂无未核销余额", 7)
        : `<thead><tr><th>往来单位</th><th>合计（元）</th><th>未到期</th><th>1-30 天</th>
             <th>31-60 天</th><th>61-90 天</th><th>90 天以上</th></tr></thead>
           <tbody>${aging
             .map(
               (row) => `<tr>
                 <td>${escapeHtml(row.partner_name || row.partner_key)}</td>
                 <td>${money(row.total_cents)}</td>
                 <td>${money(row.not_due_cents)}</td>
                 <td>${money(row.d1_30_cents)}</td>
                 <td>${money(row.d31_60_cents)}</td>
                 <td>${money(row.d61_90_cents)}</td>
                 <td>${money(row.d90_plus_cents)}</td>
               </tr>`
             )
             .join("")}</tbody>
           <tfoot><tr><td>合计</td>
             <td>${money(agingTotal)}</td>
             <td>${money(agingNotDue)}</td>
             <td>${money(agingD1)}</td>
             <td>${money(agingD31)}</td>
             <td>${money(agingD61)}</td>
             <td>${money(agingD90)}</td>
           </tr></tfoot>`;

      const purchaseRows = purchaseDetail.rows || [];
      purchaseDetailHint.textContent =
        purchaseDetail.total > purchaseRows.length
          ? `共 ${purchaseDetail.total} 行，当前显示第 ${purchaseDetail.offset + 1}–${
              purchaseDetail.offset + purchaseRows.length
            } 行；可翻页或导出 CSV / Excel 查看全部`
          : `共 ${purchaseDetail.total || 0} 行`;
      renderPager(purchasePager, purchaseDetail, purchasePage, (page) => {
        purchasePage = page;
        reload();
      });
      purchaseDetailTable.innerHTML = !purchaseRows.length
        ? emptyRow("该期间无采购", 9)
        : `<thead><tr><th>日期</th><th>单号</th><th>类型</th><th>供应商</th><th>商品</th>
             <th>规格</th><th>数量</th><th>单价（元）</th><th>金额（元）</th></tr></thead>
           <tbody>${purchaseRows
             .map(
               (row) => `<tr>
                 <td>${escapeHtml(row.date)}</td>
                 <td>${escapeHtml(row.doc_no)}</td>
                 <td>${escapeHtml(row.kind)}</td>
                 <td>${escapeHtml(row.supplier)}</td>
                 <td>${escapeHtml(row.item_code + " " + row.item_name)}</td>
                 <td>${escapeHtml(row.spec)}</td>
                 <td class="${qtyIsNegative(row.qty) ? "negative" : ""}">${qtyText(row.qty)}</td>
                 <td>${money(row.price_cents)}</td>
                 <td class="${row.amount_cents < 0 ? "negative" : ""}">${money(
                   row.amount_cents
                 )}</td>
               </tr>`
             )
             .join("")}</tbody>
           <tfoot><tr><td>合计</td><td colspan="5"></td>
             <td>${qtyText(purchaseDetail.totals ? purchaseDetail.totals.qty : 0)}</td>
             <td></td>
             <td>${money(purchaseDetail.totals ? purchaseDetail.totals.amount_cents : 0)}</td>
           </tr></tfoot>`;

      const summaryByItem = purchaseBySelect.value === "item";
      const summaryRows = purchaseSummary.rows || [];
      const summaryTotals = purchaseSummary.totals || {};
      // 入库均价只计 PUR_IN；无入库行时显示「—」，不显示 0。
      const inboundAvgText = (row) => (row.has_inbound ? money(row.avg_price_cents) : "—");
      // 净数量按入库减退货如实显示，负数额外标注「净退货」。
      const netQtyCell = (row) =>
        `<td class="${qtyIsNegative(row.qty) ? "negative" : ""}">${qtyText(row.qty)}${
          row.qty < 0 ? "（净退货）" : ""
        }</td>`;
      purchaseSummaryTable.innerHTML = !summaryRows.length
        ? emptyRow("该期间无采购", summaryByItem ? 10 : 7)
        : summaryByItem
          ? `<thead><tr><th>商品编码</th><th>商品名称</th><th>规格</th><th>单位</th>
               <th>入库数量</th><th>入库金额（元）</th><th>退货额（元）</th>
               <th>净数量</th><th>净金额（元）</th><th>入库均价（元）</th></tr></thead>
             <tbody>${summaryRows
               .map(
                 (row) => `<tr>
                   <td>${escapeHtml(row.item_code)}</td>
                   <td>${escapeHtml(row.item_name)}</td>
                   <td>${escapeHtml(row.spec)}</td>
                   <td>${escapeHtml(row.unit)}</td>
                   <td>${qtyText(row.in_qty)}</td>
                   <td>${money(row.in_amount_cents)}</td>
                   <td>${money(row.return_amount_cents)}</td>
                   ${netQtyCell(row)}
                   <td>${money(row.amount_cents)}</td>
                   <td>${inboundAvgText(row)}</td>
                 </tr>`
               )
               .join("")}</tbody>
           <tfoot><tr><td>合计</td><td colspan="3"></td>
             <td>${qtyText(summaryTotals.in_qty || 0)}</td>
             <td>${money(summaryTotals.in_amount_cents || 0)}</td>
             <td>${money(summaryTotals.return_amount_cents || 0)}</td>
             <td>${qtyText(summaryTotals.qty || 0)}</td>
             <td>${money(summaryTotals.amount_cents || 0)}</td>
             <td>${inboundAvgText(summaryTotals)}</td>
           </tr></tfoot>`
          : `<thead><tr><th>供应商</th><th>入库数量</th><th>入库金额（元）</th><th>退货额（元）</th>
               <th>净数量</th><th>净金额（元）</th><th>入库均价（元）</th></tr></thead>
             <tbody>${summaryRows
               .map(
                 (row) => `<tr>
                   <td>${escapeHtml(row.supplier)}</td>
                   <td>${qtyText(row.in_qty)}</td>
                   <td>${money(row.in_amount_cents)}</td>
                   <td>${money(row.return_amount_cents)}</td>
                   ${netQtyCell(row)}
                   <td>${money(row.amount_cents)}</td>
                   <td>${inboundAvgText(row)}</td>
                 </tr>`
               )
               .join("")}</tbody>
           <tfoot><tr><td>合计</td>
             <td>${qtyText(summaryTotals.in_qty || 0)}</td>
             <td>${money(summaryTotals.in_amount_cents || 0)}</td>
             <td>${money(summaryTotals.return_amount_cents || 0)}</td>
             <td>${qtyText(summaryTotals.qty || 0)}</td>
             <td>${money(summaryTotals.amount_cents || 0)}</td>
             <td>${inboundAvgText(summaryTotals)}</td>
           </tr></tfoot>`;

      otherFlowTable.innerHTML = !otherFlow.length
        ? emptyRow("该期间无其他出入库", 5)
        : `<thead><tr><th>业务类型</th><th>入库数量</th><th>入库金额（元）</th>
             <th>出库数量</th><th>出库金额（元）</th></tr></thead>
           <tbody>${otherFlow
             .map(
               (row) => `<tr>
                 <td>${escapeHtml(bizCategoryText(row.category) || row.category)}</td>
                 <td>${qtyText(row.in_qty)}</td>
                 <td>${money(row.in_amount_cents)}</td>
                 <td>${qtyText(row.out_qty)}</td>
                 <td>${money(row.out_amount_cents)}</td>
               </tr>`
             )
             .join("")}</tbody>`;
    }

    // 报表导出统一走后端流式写出，不再有单次行数上限。
    function wireExport(role, pathFn) {
      mount.querySelector(`[data-role="${role}"]`).addEventListener("click", async () => {
        try {
          await downloadCsv(pathFn());
          showNotice("已导出 CSV", "ok");
        } catch (error) {
          showNotice(error.message, "error");
        }
      });
    }

    wireExport("export-balance", () => "/api/reports/stock-balance" + balanceQuery() + "&format=csv");
    wireExport(
      "export-ledger",
      () => "/api/reports/stock-ledger" + ledgerQuery() + "&format=csv"
    );
    wireExport("export-flows", () => "/api/reports/goods-flow" + periodQuery() + "&format=csv");
    wireExport(
      "export-summary",
      () => "/api/reports/inventory-summary" + periodQuery() + "&format=csv"
    );
    if (profitAllowed) {
      wireExport("export-profit", () => "/api/reports/sales-profit" + periodQuery() + "&format=csv");
    }
    wireExport(
      "export-purchase-detail",
      () => "/api/reports/purchase-detail" + purchaseQuery() + "&format=csv"
    );
    wireExport(
      "export-purchase-summary",
      () =>
        "/api/reports/purchase-summary" +
        purchaseQuery() +
        "&by=" +
        purchaseBySelect.value +
        "&format=csv"
    );
    wireExport(
      "export-other-flow",
      () => "/api/reports/other-flow-summary" + otherFlowQuery() + "&format=csv"
    );
    wireExport(
      "export-aging",
      () => "/api/reports/ar-ap-aging?direction=" + directionSelect.value + "&format=csv"
    );
    wireExport(
      "export-fund",
      () =>
        "/api/reports/fund-ledger" +
        periodQuery() +
        "&account_id=" +
        encodeURIComponent(fundAccountSelect.value || "0") +
        "&format=csv"
    );
    wireExport(
      "export-fund-balances",
      () => "/api/reports/fund-balances?as_of=" + encodeURIComponent(toInput.value) + "&format=csv"
    );

    function wireExcel(role, pathFn) {
      mount.querySelector(`[data-role="${role}"]`).addEventListener("click", async () => {
        try {
          await downloadXlsx(pathFn());
          showNotice("已导出 Excel", "ok");
        } catch (error) {
          showNotice(error.message, "error");
        }
      });
    }

    wireExcel("excel-balance", () => "/api/reports/stock-balance" + balanceQuery() + "&format=xlsx");
    wireExcel(
      "excel-ledger",
      () => "/api/reports/stock-ledger" + ledgerQuery() + "&format=xlsx"
    );
    wireExcel("excel-flows", () => "/api/reports/goods-flow" + periodQuery() + "&format=xlsx");
    wireExcel(
      "excel-summary",
      () => "/api/reports/inventory-summary" + periodQuery() + "&format=xlsx"
    );
    if (profitAllowed) {
      wireExcel("excel-profit", () => "/api/reports/sales-profit" + periodQuery() + "&format=xlsx");
    }
    wireExcel(
      "excel-purchase-detail",
      () => "/api/reports/purchase-detail" + purchaseQuery() + "&format=xlsx"
    );
    wireExcel(
      "excel-purchase-summary",
      () =>
        "/api/reports/purchase-summary" +
        purchaseQuery() +
        "&by=" +
        purchaseBySelect.value +
        "&format=xlsx"
    );
    wireExcel(
      "excel-other-flow",
      () => "/api/reports/other-flow-summary" + otherFlowQuery() + "&format=xlsx"
    );
    wireExcel(
      "excel-aging",
      () => "/api/reports/ar-ap-aging?direction=" + directionSelect.value + "&format=xlsx"
    );
    wireExcel(
      "excel-fund",
      () =>
        "/api/reports/fund-ledger" +
        periodQuery() +
        "&account_id=" +
        encodeURIComponent(fundAccountSelect.value || "0") +
        "&format=xlsx"
    );
    wireExcel(
      "excel-fund-balances",
      () =>
        "/api/reports/fund-balances?as_of=" + encodeURIComponent(toInput.value) + "&format=xlsx"
    );

    function wirePrint(role, pathFn) {
      mount.querySelector(`[data-role="${role}"]`).addEventListener("click", () => printTarget(pathFn()));
      const pdf = mount.querySelector(`[data-role="${role.replace(/^print-/, "pdf-")}"]`);
      if (pdf) pdf.addEventListener("click", () => exportPdfTarget(pathFn(), null));
    }

    wirePrint(
      "print-balance",
      () => withQuery("/api/print/render?target_type=REPORT&target_key=stock-balance", balanceQuery())
    );
    wirePrint(
      "print-ledger",
      () =>
        withQuery(
          "/api/print/render?target_type=REPORT&target_key=stock-ledger",
          ledgerQuery() + "&limit=100000"
        )
    );
    wirePrint(
      "print-flows",
      () => withQuery("/api/print/render?target_type=REPORT&target_key=goods-flow", periodQuery())
    );
    wirePrint(
      "print-summary",
      () =>
        withQuery("/api/print/render?target_type=REPORT&target_key=inventory-summary", periodQuery())
    );
    if (profitAllowed) {
      wirePrint(
        "print-profit",
        () => withQuery("/api/print/render?target_type=REPORT&target_key=sales-profit", periodQuery())
      );
    }
    wirePrint(
      "print-purchase-detail",
      () =>
        withQuery(
          "/api/print/render?target_type=REPORT&target_key=purchase-detail",
          purchaseQuery() + "&limit=100000"
        )
    );
    wirePrint(
      "print-purchase-summary",
      () =>
        withQuery(
          "/api/print/render?target_type=REPORT&target_key=purchase-summary-" +
            purchaseBySelect.value,
          purchaseQuery()
        )
    );
    wirePrint(
      "print-other-flow",
      () =>
        withQuery(
          "/api/print/render?target_type=REPORT&target_key=other-flow-summary",
          otherFlowQuery()
        )
    );
    wirePrint(
      "print-aging",
      () =>
        "/api/print/render?target_type=REPORT&target_key=ar-ap-aging&direction=" +
        directionSelect.value
    );
    wirePrint(
      "print-fund",
      () =>
        withQuery(
          "/api/print/render?target_type=REPORT&target_key=fund-ledger",
          periodQuery() + "&account_id=" + encodeURIComponent(fundAccountSelect.value || "0")
        )
    );

    mount.querySelector('[data-role="refresh"]').addEventListener("click", reload);
    directionSelect.addEventListener("change", reload);
    fundAccountSelect.addEventListener("change", reload);
    balanceWarehouseSelect.addEventListener("change", reload);
    balanceItemSelect.addEventListener("change", reload);
    balanceHideZero.addEventListener("change", reload);
    ledgerFlowSelect.addEventListener("change", () => {
      ledgerPage = 1;
      reload();
    });
    ledgerCategorySelect.addEventListener("change", () => {
      ledgerPage = 1;
      reload();
    });
    const resetPurchasePage = () => {
      purchasePage = 1;
      reload();
    };
    purchaseBySelect.addEventListener("change", resetPurchasePage);
    purchaseSupplierSelect.addEventListener("change", resetPurchasePage);
    purchaseItemSelect.addEventListener("change", resetPurchasePage);
    purchaseWarehouseSelect.addEventListener("change", resetPurchasePage);
    otherFlowCategorySelect.addEventListener("change", reload);
    otherFlowWarehouseSelect.addEventListener("change", reload);
    await reload();
  }

  // ---------- 销售报表（客户 / 业务员） ----------

  async function salesReportPage(mount) {
    const today = new Date().toISOString().slice(0, 10);
    const monthStart = today.slice(0, 7) + "-01";
    const PAGE_SIZE = 200;
    let dimension = "customer";
    let page = 1;
    mount.innerHTML = `
      <div class="page-head">
        <div>
          <h2>销售报表</h2>
          <p class="page-sub">只计已审核的销售出库与销售退货；期间按单据日期，退货冲减；毛利按出库时的移动加权成本</p>
        </div>
        <div class="toolbar">
          <button class="btn" data-role="refresh">查询</button>
          <button class="btn" data-role="export">导出 CSV</button>
          <button class="btn" data-role="excel">导出 Excel</button>
          <button class="btn" data-role="print">打印</button>
          <button class="btn" data-role="pdf">导出 PDF</button>
        </div>
      </div>
      <div class="card filters">
        <label class="field"><span>期间从</span><input type="date" data-role="from" value="${monthStart}" /></label>
        <label class="field"><span>期间到</span><input type="date" data-role="to" value="${today}" /></label>
        <label class="field"><span>客户</span><select data-role="customer"></select></label>
        <label class="field"><span>业务员</span><select data-role="salesperson"></select></label>
        <label class="field"><span>商品</span><select data-role="item"></select></label>
        <label class="field"><span>仓库</span><select data-role="warehouse"></select></label>
      </div>
      <div class="card">
        <div class="card-head">
          <div class="tabs">
            <button class="tab active" data-role="dim-customer">按客户</button>
            <button class="tab" data-role="dim-salesperson">按业务员</button>
          </div>
          <span class="page-sub" data-role="summary-hint"></span>
        </div>
        <table class="table" data-role="summary"></table>
      </div>
      <div class="card">
        <div class="card-head">
          <h3>明细</h3>
          <span class="page-sub" data-role="detail-hint"></span>
          <div class="pager" data-role="pager"></div>
        </div>
        <table class="table" data-role="detail"></table>
      </div>`;

    const fromInput = mount.querySelector('[data-role="from"]');
    const toInput = mount.querySelector('[data-role="to"]');
    const customerSelect = mount.querySelector('[data-role="customer"]');
    const salespersonSelect = mount.querySelector('[data-role="salesperson"]');
    const itemSelect = mount.querySelector('[data-role="item"]');
    const warehouseSelect = mount.querySelector('[data-role="warehouse"]');
    const summaryTable = mount.querySelector('[data-role="summary"]');
    const detailTable = mount.querySelector('[data-role="detail"]');
    const summaryHint = mount.querySelector('[data-role="summary-hint"]');
    const detailHint = mount.querySelector('[data-role="detail-hint"]');
    const pager = mount.querySelector('[data-role="pager"]');
    const dimCustomer = mount.querySelector('[data-role="dim-customer"]');
    const dimSalesperson = mount.querySelector('[data-role="dim-salesperson"]');

    const [customers, employees, items, warehouses] = await Promise.all([
      api("GET", "/api/customers"),
      api("GET", "/api/employees"),
      api("GET", "/api/items"),
      api("GET", "/api/warehouses"),
    ]);
    const option = (value, label) => `<option value="${value}">${escapeHtml(label)}</option>`;
    customerSelect.innerHTML =
      option("0", "全部客户") +
      customers.map((row) => option(row.id, row.code + " " + row.name)).join("");
    salespersonSelect.innerHTML =
      option("0", "全部业务员") +
      employees
        .map((row) =>
          option(row.id, row.code + " " + row.name + (row.status ? "" : "（停用）"))
        )
        .join("");
    itemSelect.innerHTML =
      option("0", "全部商品") +
      items.map((row) => option(row.id, itemOptionText(row))).join("");
    warehouseSelect.innerHTML =
      option("0", "全部仓库") +
      warehouses.map((row) => option(row.id, row.code + " " + row.name)).join("");

    function query(extra) {
      return (
        "?from=" +
        encodeURIComponent(fromInput.value) +
        "&to=" +
        encodeURIComponent(toInput.value) +
        "&customer_id=" +
        encodeURIComponent(customerSelect.value) +
        "&salesperson_id=" +
        encodeURIComponent(salespersonSelect.value) +
        "&item_id=" +
        encodeURIComponent(itemSelect.value) +
        "&warehouse_id=" +
        encodeURIComponent(warehouseSelect.value) +
        "&dimension=" +
        dimension +
        (extra || "")
      );
    }

    function renderSummary(result) {
      const bySalesperson = dimension === "salesperson";
      const header = bySalesperson
        ? "<th>业务员</th><th>单数</th><th>客户数</th><th>销售数量</th><th>销售额（元）</th><th>退货额（元）</th><th>净销售额（元）</th><th>成本（元）</th><th>毛利（元）</th><th>毛利率</th><th>销售额占比</th>"
        : "<th>客户</th><th>单数</th><th>销售数量</th><th>销售额（元）</th><th>退货额（元）</th><th>净销售额（元）</th><th>成本（元）</th><th>毛利（元）</th><th>毛利率</th><th>销售额占比</th>";
      const rows = result.rows || [];
      const totals = result.totals || {};
      summaryTable.innerHTML = !rows.length
        ? emptyRow("该期间没有销售数据", bySalesperson ? 11 : 10)
        : `<thead><tr>${header}</tr></thead><tbody>${rows
            .map(
              (row) => `<tr>
                <td>${escapeHtml(row.name)}</td>
                <td>${row.orders}</td>
                ${bySalesperson ? `<td>${row.customers}</td>` : ""}
                <td>${row.qty}</td>
                <td>${money(row.sales_cents)}</td>
                <td>${money(row.return_cents)}</td>
                <td>${money(row.net_cents)}</td>
                <td>${money(row.cost_cents)}</td>
                <td>${money(row.profit_cents)}</td>
                <td>${escapeHtml(row.margin)}</td>
                <td>${escapeHtml(row.ratio)}</td>
              </tr>`
            )
            .join("")}
          <tr class="total-row">
            <td>合计</td><td>${totals.orders || 0}</td>
            ${bySalesperson ? "<td>—</td>" : ""}
            <td>${totals.qty || "0"}</td>
            <td>${money(totals.sales_cents)}</td>
            <td>${money(totals.return_cents)}</td>
            <td>${money(totals.net_cents)}</td>
            <td>${money(totals.cost_cents)}</td>
            <td>${money(totals.profit_cents)}</td>
            <td>${escapeHtml(totals.margin || "—")}</td>
            <td>100.00%</td>
          </tr></tbody>`;
      summaryHint.textContent = `共 ${rows.length} 组；毛利率 = 毛利 ÷ 净销售额，净销售额为 0 时显示「—」`;
    }

    function renderDetail(result) {
      const bySalesperson = dimension === "salesperson";
      const rows = result.rows || [];
      const totals = result.totals || {};
      const header = bySalesperson
        ? "<th>日期</th><th>单号</th><th>业务员</th><th>客户</th><th>数量</th><th>金额（元）</th><th>毛利（元）</th>"
        : "<th>日期</th><th>单号</th><th>客户</th><th>商品编码</th><th>商品名称</th><th>规格</th><th>数量</th><th>单价（元）</th><th>金额（元）</th><th>成本（元）</th><th>毛利（元）</th>";
      detailTable.innerHTML = !rows.length
        ? emptyRow("该期间没有明细", bySalesperson ? 7 : 11)
        : `<thead><tr>${header}</tr></thead><tbody>${rows
            .map(
              (row) => `<tr>
                <td>${escapeHtml(row.date)}</td>
                <td>${escapeHtml(row.doc_no)}</td>
                ${bySalesperson ? "" : `<td>${escapeHtml(row.customer)}</td>`}
                ${
                  bySalesperson
                    ? `<td>${escapeHtml(row.salesperson)}</td><td>${escapeHtml(row.customer)}</td>`
                    : `<td>${escapeHtml(row.item_code)}</td><td>${escapeHtml(
                        row.item_name
                      )}</td><td>${escapeHtml(row.spec)}</td>`
                }
                <td>${row.qty}</td>
                ${bySalesperson ? "" : `<td>${money(row.price_cents)}</td>`}
                <td>${money(row.amount_cents)}</td>
                ${bySalesperson ? "" : `<td>${money(row.cost_cents)}</td>`}
                <td>${money(row.profit_cents)}</td>
              </tr>`
            )
            .join("")}
          <tr class="total-row">
            <td>合计</td><td>—</td><td>—</td>
            ${bySalesperson ? "" : "<td>—</td><td>—</td><td>—</td>"}
            <td>${totals.qty || "0"}</td>
            ${bySalesperson ? "" : "<td>—</td>"}
            <td>${money(totals.amount_cents)}</td>
            ${bySalesperson ? "" : `<td>${money(totals.cost_cents)}</td>`}
            <td>${money(totals.profit_cents)}</td>
          </tr></tbody>`;
      const total = Number(result.total || 0);
      const offset = Number(result.offset || 0);
      detailHint.textContent =
        total > rows.length
          ? `共 ${total} 行，当前显示第 ${offset + 1}–${offset + rows.length} 行；导出始终全量`
          : `共 ${total} 行`;
      renderReportPager(pager, total, page, (next) => {
        page = next;
        reload().catch((error) => showNotice(error.message, "error"));
      });
    }

    function renderReportPager(element, total, current, onChange) {
      if (total <= PAGE_SIZE) {
        element.innerHTML = "";
        return;
      }
      const pages = Math.max(1, Math.ceil(total / PAGE_SIZE));
      element.innerHTML = `<button class="btn" type="button" data-page="${current - 1}" ${
        current <= 1 ? "disabled" : ""
      }>上一页</button><span>第 ${current} / ${pages} 页</span>
        <button class="btn" type="button" data-page="${current + 1}" ${
          current >= pages ? "disabled" : ""
        }>下一页</button>`;
      element.onclick = (event) => {
        const button = event.target.closest("button[data-page]");
        if (!button || button.disabled) return;
        const next = Number(button.dataset.page);
        if (next >= 1 && next <= pages) onChange(next);
      };
    }

    async function reload() {
      showNotice("");
      const [summary, detail] = await Promise.all([
        api("GET", "/api/reports/sales-summary" + query()),
        api(
          "GET",
          "/api/reports/sales-detail" +
            query("&limit=" + PAGE_SIZE + "&offset=" + (page - 1) * PAGE_SIZE)
        ),
      ]);
      renderSummary(summary);
      renderDetail(detail);
    }

    const setDimension = (next) => {
      if (dimension === next) return;
      dimension = next;
      page = 1;
      dimCustomer.classList.toggle("active", next === "customer");
      dimSalesperson.classList.toggle("active", next === "salesperson");
      reload().catch((error) => showNotice(error.message, "error"));
    };
    dimCustomer.addEventListener("click", () => setDimension("customer"));
    dimSalesperson.addEventListener("click", () => setDimension("salesperson"));

    const resetAndReload = () => {
      page = 1;
      reload().catch((error) => showNotice(error.message, "error"));
    };
    for (const input of [fromInput, toInput, customerSelect, salespersonSelect, itemSelect, warehouseSelect]) {
      input.addEventListener("change", resetAndReload);
    }

    const targetKey = () => (dimension === "salesperson" ? "sales-salesperson" : "sales-customer");
    const renderPath = () =>
      withQuery("/api/print/render?target_type=REPORT&target_key=" + targetKey(), query());
    mount.querySelector('[data-role="refresh"]').addEventListener("click", resetAndReload);
    mount.querySelector('[data-role="export"]').addEventListener("click", async () => {
      try {
        await downloadCsv("/api/reports/sales-summary" + query("&format=csv"));
        showNotice("已导出 CSV", "ok");
      } catch (error) {
        showNotice(error.message, "error");
      }
    });
    mount.querySelector('[data-role="excel"]').addEventListener("click", async () => {
      try {
        await downloadXlsx("/api/reports/sales-summary" + query("&format=xlsx"));
        showNotice("已导出 Excel", "ok");
      } catch (error) {
        showNotice(error.message, "error");
      }
    });
    mount.querySelector('[data-role="print"]').addEventListener("click", () => printTarget(renderPath()));
    mount
      .querySelector('[data-role="pdf"]')
      .addEventListener("click", () => exportPdfTarget(renderPath(), null));

    await reload();
  }

  // ---------- 经营看板 ----------

  async function dashboardPage(mount) {
    mount.innerHTML = `
      <div class="page-head">
        <div>
          <h2>经营看板</h2>
          <p class="page-sub">今日 / 本月按单据日期归属；退货冲减销售额；毛利按出库时的移动加权成本；库存总额取当前结存</p>
        </div>
        <button class="btn" data-role="refresh">刷新</button>
      </div>
      <div class="dashboard-grid" data-role="cards"></div>
      <div class="card">
        <div class="card-head"><h3>应收欠款 Top 10</h3><span class="page-sub">按客户未核销应收合计降序</span></div>
        <table class="table" data-role="debtors"></table>
      </div>`;
    const cards = mount.querySelector('[data-role="cards"]');
    const debtors = mount.querySelector('[data-role="debtors"]');

    async function reload() {
      showNotice("");
      const data = await api("GET", "/api/reports/dashboard");
      const card = (title, value, note) =>
        `<div class="card metric"><span class="metric-label">${escapeHtml(
          title
        )}</span><strong>${escapeHtml(value)}</strong><span class="page-sub">${escapeHtml(
          note || ""
        )}</span></div>`;
      cards.innerHTML =
        card("今日销售额", money(data.today.net_cents), `出库 ${money(data.today.sales_cents)}，退货 ${money(data.today.return_cents)}`) +
        card("今日毛利", money(data.today.profit_cents), `成本 ${money(data.today.cost_cents)}`) +
        card("本月销售额", money(data.month.net_cents), `${data.month_start} 至 ${data.as_of}`) +
        card("本月毛利", money(data.month.profit_cents), `成本 ${money(data.month.cost_cents)}`) +
        card("库存总额", money(data.stock_value_cents), "按当前结存金额");
      const rows = data.top_debtors || [];
      debtors.innerHTML = !rows.length
        ? emptyRow("暂无应收欠款", 3)
        : `<thead><tr><th>客户</th><th>未核销应收（元）</th></tr></thead><tbody>${rows
            .map(
              (row) =>
                `<tr><td>${escapeHtml(row.name)}</td><td>${money(row.outstanding_cents)}</td></tr>`
            )
            .join("")}</tbody>`;
    }

    mount.querySelector('[data-role="refresh"]').addEventListener("click", () => {
      reload().catch((error) => showNotice(error.message, "error"));
    });
    await reload();
  }

  // ---------- 对账单 ----------

  async function statementPage(mount) {
    const today = new Date().toISOString().slice(0, 10);
    mount.innerHTML = `
      <div class="page-head">
        <div>
          <h2>对账单</h2>
          <p class="page-sub">期初 + 期间单据 − 期间核销 = 期末；只读，金额单位为元</p>
          <p class="page-sub">统计范围：仅统计 v7 迁移之后审核的单据，升级前已审核单据不计入</p>
        </div>
        <div class="toolbar">
          <select data-role="direction">
            <option value="1">应收对账单</option>
            <option value="-1">应付对账单</option>
          </select>
          <select data-role="partner"></select>
          <input type="date" data-role="from" value="${today.slice(0, 8)}01" />
          <input type="date" data-role="to" value="${today}" />
          <button class="btn" data-role="refresh">查询</button>
          <button class="btn" data-role="export">导出 CSV</button>
          <button class="btn" data-role="excel">导出 Excel</button>
          <button class="btn" data-role="print">打印</button>
          <button class="btn" data-role="pdf">导出 PDF</button>
        </div>
      </div>
      <div class="card">
        <div class="card-head">
          <h3 data-role="title">往来对账单</h3>
        </div>
        <table class="table" data-role="summary"></table>
      </div>
      <div class="card">
        <table class="table" data-role="lines"></table>
      </div>`;

    const directionSelect = mount.querySelector('[data-role="direction"]');
    const partnerSelect = mount.querySelector('[data-role="partner"]');
    const fromInput = mount.querySelector('[data-role="from"]');
    const toInput = mount.querySelector('[data-role="to"]');
    const title = mount.querySelector('[data-role="title"]');
    const summaryTable = mount.querySelector('[data-role="summary"]');
    const linesTable = mount.querySelector('[data-role="lines"]');
    function queryPath() {
      return (
        "/api/reports/statement?partner_key=" +
        encodeURIComponent(partnerSelect.value) +
        "&direction=" +
        directionSelect.value +
        "&from=" +
        encodeURIComponent(fromInput.value) +
        "&to=" +
        encodeURIComponent(toInput.value)
      );
    }

    async function loadPartners() {
      const isPayable = directionSelect.value === "-1";
      const rows = await api("GET", isPayable ? "/api/suppliers" : "/api/customers");
      const previous = partnerSelect.value;
      const prefix = isPayable ? "S:" : "C:";
      partnerSelect.innerHTML = rows
        .filter((row) => row.status)
        .map(
          (row) =>
            `<option value="${prefix}${row.id}">${escapeHtml(row.code + " " + row.name)}</option>`
        )
        .join("");
      if (previous && [...partnerSelect.options].some((option) => option.value === previous)) {
        partnerSelect.value = previous;
      }
    }

    function render(result) {
      title.textContent =
        (result.partner_name || "未命名往来单位") +
        (result.direction === -1 ? " 应付对账单" : " 应收对账单");
      summaryTable.innerHTML = `
        <thead><tr><th>期初余额（元）</th><th>本期增加（元）</th>
          <th>本期减少（元）</th><th>其中撤销（元）</th><th>期间核销净额（元）</th>
          <th>期末余额（元）</th></tr></thead>
        <tbody><tr>
          <td>${money(result.opening_cents)}</td>
          <td>${money(result.increase_cents)}</td>
          <td>${money(result.decrease_cents)}</td>
          <td>${result.reverse_count ? money(result.reverse_cents) + "（" + result.reverse_count + " 笔）" : "—"}</td>
          <td class="${result.settle_net_cents < 0 ? "negative" : ""}">${money(result.settle_net_cents)}</td>
          <td>${money(result.closing_cents)}</td>
        </tr></tbody>`;
      linesTable.innerHTML = !result.lines.length
        ? `<tbody><tr><td class="empty" colspan="7">该期间没有发生额</td></tr></tbody>`
        : `<thead><tr><th>日期</th><th>类型</th><th>单号</th><th>摘要</th>
             <th>增加（元）</th><th>减少（元）</th><th>余额（元）</th></tr></thead>
           <tbody>${result.lines
             .map(
               (line) => `<tr${line.is_reverse ? ' class="reverse-row"' : ""}>
                 <td>${escapeHtml(line.date)}</td>
                 <td>${escapeHtml(line.kind_label)}</td>
                 <td>${escapeHtml(line.doc_no)}</td>
                 <td>${escapeHtml(line.summary)}</td>
                 <td>${line.increase_cents ? money(line.increase_cents) : "—"}</td>
                 <td>${line.decrease_cents ? money(line.decrease_cents) : "—"}</td>
                 <td>${money(line.balance_cents)}</td>
               </tr>`
             )
             .join("")}</tbody>`;
    }

    function renderPath() {
      return (
        "/api/print/render?target_type=REPORT&target_key=statement&partner_key=" +
        encodeURIComponent(partnerSelect.value) +
        "&direction=" +
        directionSelect.value +
        "&from=" +
        encodeURIComponent(fromInput.value) +
        "&to=" +
        encodeURIComponent(toInput.value)
      );
    }

    async function reload() {
      if (!partnerSelect.value) {
        showNotice("请先在客户或供应商档案中新增往来单位", "error");
        summaryTable.innerHTML = "";
        linesTable.innerHTML = "";
        return;
      }
      showNotice("");
      const result = await api("GET", queryPath());
      render(result);
    }

    directionSelect.addEventListener("change", async () => {
      try {
        await loadPartners();
        await reload();
      } catch (error) {
        showNotice(error.message, "error");
      }
    });
    partnerSelect.addEventListener("change", () => {
      reload().catch((error) => showNotice(error.message, "error"));
    });
    mount.querySelector('[data-role="refresh"]').addEventListener("click", () => {
      reload().catch((error) => showNotice(error.message, "error"));
    });
    mount.querySelector('[data-role="export"]').addEventListener("click", async () => {
      if (!partnerSelect.value) {
        showNotice("请先选择往来单位", "error");
        return;
      }
      try {
        await downloadCsv(queryPath() + "&format=csv");
        showNotice("已导出 CSV", "ok");
      } catch (error) {
        showNotice(error.message, "error");
      }
    });
    mount.querySelector('[data-role="excel"]').addEventListener("click", async () => {
      if (!partnerSelect.value) {
        showNotice("请先选择往来单位", "error");
        return;
      }
      try {
        await downloadXlsx(queryPath() + "&format=xlsx");
        showNotice("已导出 Excel", "ok");
      } catch (error) {
        showNotice(error.message, "error");
      }
    });
    mount.querySelector('[data-role="print"]').addEventListener("click", () => {
      if (!partnerSelect.value) {
        showNotice("请先选择往来单位", "error");
        return;
      }
      printTarget(renderPath());
    });
    mount.querySelector('[data-role="pdf"]').addEventListener("click", () => {
      if (!partnerSelect.value) {
        showNotice("请先选择往来单位", "error");
        return;
      }
      exportPdfTarget(renderPath(), null);
    });

    await loadPartners();
    await reload();
  }

  // ---------- 打印模板管理 ----------

  async function printTemplatePage(mount) {
    mount.innerHTML = `
      <div class="page-head">
        <div>
          <h2>打印模板</h2>
          <p class="page-sub">模板只保存纸张、页眉页脚与列显示；列来自目标注册表，每个目标可设一个默认模板</p>
        </div>
        <div class="toolbar">
          <select data-role="filter-target"></select>
          <button class="btn" data-role="new">新建模板</button>
          <button class="btn" data-role="refresh">刷新</button>
        </div>
      </div>
      <div class="card" data-role="editor-card" hidden>
        <div class="card-head">
          <h3 data-role="editor-title">新建模板</h3>
          <button class="btn small" type="button" data-role="copy-default">从内置默认复制列</button>
        </div>
        <form data-role="editor">
          <div class="doc-head">
            <label class="field"><span>名称</span><input data-role="name" maxlength="100" /></label>
            <label class="field"><span>编码</span><input data-role="code" maxlength="50" /></label>
            <label class="field"><span>目标</span><select data-role="target"></select></label>
            <label class="field"><span>纸张</span>
              <select data-role="page-size">
                <option value="A4">A4</option>
                <option value="A5">A5</option>
                <option value="241x140">针式 241×140 mm</option>
                <option value="241x93">针式 241×93 mm</option>
                <option value="CUSTOM">自定义</option>
              </select>
            </label>
            <label class="field"><span>方向</span>
              <select data-role="orientation">
                <option value="PORTRAIT">纵向</option>
                <option value="LANDSCAPE">横向</option>
              </select>
            </label>
            <label class="field"><span>纸张类别</span>
              <select data-role="paper-kind">
                <option value="SHEET">单页纸</option>
                <option value="CONTINUOUS">连续纸（针式）</option>
              </select>
            </label>
            <label class="field"><span>缩放</span>
              <select data-role="scale-mode">
                <option value="FIT_WIDTH">按纸宽自适应</option>
                <option value="ORIGINAL">原始尺寸</option>
              </select>
            </label>
            <label class="field"><span>页边距（mm）</span>
              <input type="number" min="0" max="50" data-role="margin" />
            </label>
            <label class="field"><span>字号（pt）</span>
              <input type="number" min="6" max="24" data-role="font-size" />
            </label>
            <label class="field" data-role="custom-size"><span>纸张宽（mm）</span>
              <input type="number" min="1" max="1000" data-role="paper-width" />
            </label>
            <label class="field" data-role="custom-size"><span>纸张高（mm）</span>
              <input type="number" min="1" max="1000" data-role="paper-height" />
            </label>
            <label class="field"><span>页眉文本</span>
              <input data-role="header" maxlength="500" placeholder="可用 {title} {date} {doc_no} 等占位符" />
            </label>
            <label class="field"><span>页脚文本</span>
              <input data-role="footer" maxlength="500" placeholder="纯文本，打印时按占位符替换" />
            </label>
            <label class="checkbox"><input type="checkbox" data-role="show-page-no" />显示页码</label>
            <label class="checkbox"><input type="checkbox" data-role="is-default" />设为该目标默认</label>
            <label class="checkbox"><input type="checkbox" data-role="active" />启用</label>
          </div>
          <p class="page-sub" data-role="layout-hint"></p>
          <table class="table print-columns" data-role="columns"></table>
          <div class="form-actions">
            <button class="btn primary" type="button" data-role="save">保存</button>
            <button class="btn" type="button" data-role="cancel">取消</button>
          </div>
        </form>
      </div>
      <div class="card">
        <table class="table" data-role="list"></table>
      </div>`;

    const filterSelect = mount.querySelector('[data-role="filter-target"]');
    const editorCard = mount.querySelector('[data-role="editor-card"]');
    const editorTitle = mount.querySelector('[data-role="editor-title"]');
    const editorTarget = mount.querySelector('[data-role="target"]');
    const nameInput = mount.querySelector('[data-role="name"]');
    const codeInput = mount.querySelector('[data-role="code"]');
    const pageSizeSelect = mount.querySelector('[data-role="page-size"]');
    const orientationSelect = mount.querySelector('[data-role="orientation"]');
    const paperKindSelect = mount.querySelector('[data-role="paper-kind"]');
    const scaleModeSelect = mount.querySelector('[data-role="scale-mode"]');
    const layoutHint = mount.querySelector('[data-role="layout-hint"]');
    const marginInput = mount.querySelector('[data-role="margin"]');
    const fontInput = mount.querySelector('[data-role="font-size"]');
    const paperWidthInput = mount.querySelector('[data-role="paper-width"]');
    const paperHeightInput = mount.querySelector('[data-role="paper-height"]');
    const headerInput = mount.querySelector('[data-role="header"]');
    const footerInput = mount.querySelector('[data-role="footer"]');
    const showPageNoInput = mount.querySelector('[data-role="show-page-no"]');
    const isDefaultInput = mount.querySelector('[data-role="is-default"]');
    const activeInput = mount.querySelector('[data-role="active"]');
    const customSizeFields = mount.querySelectorAll('[data-role="custom-size"]');
    const columnsTable = mount.querySelector('[data-role="columns"]');
    const listTable = mount.querySelector('[data-role="list"]');

    let targets = [];
    let templates = [];
    let editing = null;
    let editingColumns = [];

    function splitTarget(value) {
      const index = value.indexOf("|");
      return { type: value.slice(0, index), key: value.slice(index + 1) };
    }

    function targetLabel(type, key) {
      const target = targets.find((item) => item.type === type && item.key === key);
      return target ? target.name : type + "/" + key;
    }

    function fillTargetOptions(select, includeAll) {
      const groups = [
        { label: "单据", type: "DOC" },
        { label: "报表", type: "REPORT" },
      ];
      select.innerHTML =
        (includeAll ? '<option value="">全部目标</option>' : "") +
        groups
          .map((group) => {
            const options = targets
              .filter((target) => target.type === group.type)
              .map(
                (target) =>
                  `<option value="${target.type}|${target.key}">${escapeHtml(target.name)}</option>`
              )
              .join("");
            return options ? `<optgroup label="${group.label}">${options}</optgroup>` : "";
          })
          .join("");
    }

    function builtinColumns(type, key) {
      const target = targets.find((item) => item.type === type && item.key === key);
      if (!target) return [];
      return target.builtin.columns.map((column) => ({
        column_key: column.column_key,
        visible: column.visible,
        width_mm: column.width_mm,
        align: column.align,
        font_size_pt: column.font_size_pt,
        bold: column.bold,
      }));
    }

    // 针式预设只改前端选择项，落库仍是 CUSTOM + 宽高，不需要改纸张枚举与表结构。
    const paperPresets = {
      "241x140": { width: 241, height: 140 },
      "241x93": { width: 241, height: 93 },
    };

    function paperPresetKey(template) {
      if (template && template.page_size === "CUSTOM") {
        for (const [key, size] of Object.entries(paperPresets)) {
          if (Number(template.paper_width_mm) === size.width &&
              Number(template.paper_height_mm) === size.height) {
            return key;
          }
        }
      }
      return template ? template.page_size : "A4";
    }

    function updateCustomSize() {
      const preset = paperPresets[pageSizeSelect.value];
      if (preset) {
        paperWidthInput.value = preset.width;
        paperHeightInput.value = preset.height;
        // 针式预设默认走连续纸，避免和 A4 的按纸宽缩放混在一起。
        paperKindSelect.value = "CONTINUOUS";
      } else if (pageSizeSelect.value === "A4" || pageSizeSelect.value === "A5") {
        paperKindSelect.value = "SHEET";
      }
      const custom = pageSizeSelect.value === "CUSTOM";
      for (const field of customSizeFields) field.hidden = !custom;
      updateLayoutHint();
    }

    function pageDimensionsMm() {
      const preset = paperPresets[pageSizeSelect.value];
      let width = 210;
      let height = 297;
      if (preset) {
        width = preset.width;
        height = preset.height;
      } else if (pageSizeSelect.value === "A5") {
        width = 148;
        height = 210;
      } else if (pageSizeSelect.value === "CUSTOM") {
        width = Number(paperWidthInput.value || 0) || 210;
        height = Number(paperHeightInput.value || 0) || 297;
      }
      if (orientationSelect.value === "LANDSCAPE") [width, height] = [height, width];
      return { width, height };
    }

    function updateLayoutHint() {
      if (!layoutHint) return;
      const { width } = pageDimensionsMm();
      const margin = Number(marginInput.value || 0);
      const printable = Math.max(0, width - margin * 2);
      const tableWidth = editingColumns
        .filter((column) => column.visible)
        .reduce((sum, column) => sum + (Number(column.width_mm) || 18), 0);
      const continuous = paperKindSelect.value === "CONTINUOUS";
      const scaleMode = scaleModeSelect.value;
      let text = `可打印宽 ${printable}mm，当前可见列合计 ${tableWidth}mm。`;
      if (continuous) {
        text += " 连续纸固定原始尺寸与套打位置，不参与缩放。";
        if (tableWidth > printable) {
          text += ` 超出 ${tableWidth - printable}mm，请调整纸宽或精简列。`;
        }
      } else if (scaleMode === "ORIGINAL") {
        text += " 原始尺寸";
        text += tableWidth > printable
          ? `，超出 ${tableWidth - printable}mm，打印/导出前会提示改横向或精简列。`
          : "，不会缩放。";
      } else if (tableWidth > printable) {
        const scale = Math.max(10, Math.floor((printable * 100) / tableWidth));
        const font = ((Number(fontInput.value || 10) * scale) / 100).toFixed(1);
        text += ` 将按 ${scale}% 缩放，字号约 ${font}pt`;
        text += Number(font) < 7 ? "，低于 7pt，建议改横向或精简列。" : "。";
      } else {
        text += " 宽度足够，按 100% 输出。";
      }
      if (showPageNoInput && showPageNoInput.checked) {
        text += " 页码随打印引擎而定；网络版可在浏览器打印对话框勾选页眉和页脚。";
      }
      layoutHint.textContent = text;
    }

    function renderColumns() {
      const parts = splitTarget(editorTarget.value);
      const target = targets.find(
        (item) => item.type === parts.type && item.key === parts.key
      );
      const defs = target ? target.columns : [];
      if (!editingColumns.length) {
        columnsTable.innerHTML =
          '<tbody><tr><td class="empty">请先选择打印目标</td></tr></tbody>';
        return;
      }
      columnsTable.innerHTML = `
        <thead><tr><th>显示</th><th>列</th><th>宽度（mm，0 自动）</th><th>对齐</th>
          <th>字号（0 跟随）</th><th>加粗</th><th class="actions">排序</th></tr></thead>
        <tbody>${editingColumns
          .map((column, index) => {
            const def = defs.find((item) => item.key === column.column_key);
            return `<tr data-index="${index}">
              <td><input type="checkbox" data-role="col-visible" ${column.visible ? "checked" : ""} /></td>
              <td>${escapeHtml(def ? def.label : column.column_key)}
                <div class="page-sub">${escapeHtml(column.column_key)}</div></td>
              <td><input type="number" min="0" max="500" class="col-num" data-role="col-width"
                value="${Number(column.width_mm) || 0}" /></td>
              <td><select data-role="col-align">
                <option value="LEFT" ${column.align === "LEFT" ? "selected" : ""}>左</option>
                <option value="CENTER" ${column.align === "CENTER" ? "selected" : ""}>中</option>
                <option value="RIGHT" ${column.align === "RIGHT" ? "selected" : ""}>右</option>
              </select></td>
              <td><input type="number" min="0" max="24" class="col-num" data-role="col-font"
                value="${Number(column.font_size_pt) || 0}" /></td>
              <td><input type="checkbox" data-role="col-bold" ${column.bold ? "checked" : ""} /></td>
              <td class="actions">
                <button class="link" type="button" data-role="col-up" ${index === 0 ? "disabled" : ""}>上移</button>
                <button class="link" type="button" data-role="col-down"
                  ${index === editingColumns.length - 1 ? "disabled" : ""}>下移</button>
              </td>
            </tr>`;
          })
          .join("")}</tbody>`;
    }

    function syncColumnsFromDom() {
      const rows = columnsTable.querySelectorAll("tbody tr[data-index]");
      rows.forEach((row, index) => {
        const column = editingColumns[index];
        if (!column) return;
        column.visible = row.querySelector('[data-role="col-visible"]').checked;
        column.width_mm = Number(row.querySelector('[data-role="col-width"]').value || 0);
        column.align = row.querySelector('[data-role="col-align"]').value;
        column.font_size_pt = Number(row.querySelector('[data-role="col-font"]').value || 0);
        column.bold = row.querySelector('[data-role="col-bold"]').checked;
      });
    }

    function openEditor(template) {
      editing = template;
      editorCard.hidden = false;
      editorTitle.textContent = template ? "编辑模板" : "新建模板";
      editorTarget.disabled = !!template;
      nameInput.value = template ? template.name : "";
      codeInput.value = template ? template.code : "";
      editorTarget.value = template
        ? template.target_type + "|" + template.target_key
        : filterSelect.value || (targets[0] ? targets[0].type + "|" + targets[0].key : "");
      pageSizeSelect.value = template ? paperPresetKey(template) : "A4";
      orientationSelect.value = template ? template.orientation : "PORTRAIT";
      marginInput.value = template ? template.margin_mm : 12;
      fontInput.value = template ? template.font_size_pt : 10;
      paperWidthInput.value = template ? template.paper_width_mm : 0;
      paperHeightInput.value = template ? template.paper_height_mm : 0;
      headerInput.value = template ? template.header_text : "";
      footerInput.value = template ? template.footer_text : "";
      showPageNoInput.checked = template ? template.show_page_no : true;
      isDefaultInput.checked = template ? template.is_default : false;
      activeInput.checked = template ? template.active : true;
      updateCustomSize();
      paperKindSelect.value = template ? template.paper_kind || "SHEET" : paperKindSelect.value;
      scaleModeSelect.value = template ? template.scale_mode || "FIT_WIDTH" : "FIT_WIDTH";
      if (template) {
        editingColumns = template.columns.map((column) => ({ ...column }));
      } else {
        const parts = splitTarget(editorTarget.value);
        editingColumns = builtinColumns(parts.type, parts.key);
      }
      renderColumns();
      updateLayoutHint();
      editorCard.scrollIntoView({ behavior: "smooth", block: "start" });
    }

    function closeEditor() {
      editing = null;
      editorCard.hidden = true;
    }

    function renderList() {
      const filtered = filterSelect.value
        ? templates.filter(
            (template) =>
              template.target_type + "|" + template.target_key === filterSelect.value
          )
        : templates;
      if (!filtered.length) {
        listTable.innerHTML =
          '<tbody><tr><td class="empty">还没有模板，点「新建模板」时会从内置默认复制一份</td></tr></tbody>';
        return;
      }
      listTable.innerHTML = `
        <thead><tr><th>名称</th><th>编码</th><th>目标</th><th>默认</th><th>状态</th>
          <th>版本</th><th>更新时间</th><th class="actions">操作</th></tr></thead>
        <tbody>${filtered
          .map(
            (template) => `<tr data-id="${template.id}">
              <td>${escapeHtml(template.name)}</td>
              <td>${escapeHtml(template.code)}</td>
              <td>${escapeHtml(targetLabel(template.target_type, template.target_key))}</td>
              <td>${template.is_default ? "是" : "—"}</td>
              <td>${template.active ? "启用" : "停用"}</td>
              <td>${template.version}</td>
              <td>${escapeHtml(template.updated_at)}</td>
              <td class="actions">
                <button class="link" data-role="edit">编辑</button>
                ${
                  template.is_default
                    ? ""
                    : '<button class="link" data-role="default">设为默认</button>'
                }
                <button class="link danger" data-role="delete">删除</button>
              </td>
            </tr>`
          )
          .join("")}</tbody>`;
    }

    async function reload() {
      templates = await api("GET", "/api/print-templates");
      renderList();
    }

    async function saveEditor() {
      syncColumnsFromDom();
      const parts = splitTarget(editorTarget.value);
      if (!parts.type || !parts.key) {
        showNotice("请选择打印目标", "error");
        return;
      }
      if (!nameInput.value.trim() || !codeInput.value.trim()) {
        showNotice("请填写模板名称与编码", "error");
        return;
      }
      const paperPreset = paperPresets[pageSizeSelect.value];
      const body = {
        name: nameInput.value.trim(),
        code: codeInput.value.trim(),
        target_type: parts.type,
        target_key: parts.key,
        page_size: paperPreset ? "CUSTOM" : pageSizeSelect.value,
        orientation: orientationSelect.value,
        paper_kind: paperKindSelect.value,
        scale_mode: scaleModeSelect.value,
        paper_width_mm: paperPreset
          ? paperPreset.width
          : Number(paperWidthInput.value || 0),
        paper_height_mm: paperPreset
          ? paperPreset.height
          : Number(paperHeightInput.value || 0),
        margin_mm: Number(marginInput.value || 0),
        font_size_pt: Number(fontInput.value || 10),
        header_text: headerInput.value,
        footer_text: footerInput.value,
        show_page_no: showPageNoInput.checked,
        is_default: isDefaultInput.checked,
        active: activeInput.checked,
        columns: editingColumns.map((column) => ({
          column_key: column.column_key,
          visible: column.visible,
          width_mm: Number(column.width_mm) || 0,
          align: column.align,
          font_size_pt: Number(column.font_size_pt) || 0,
          bold: column.bold,
        })),
      };
      try {
        if (editing) {
          body.version = editing.version;
          await api("PUT", "/api/print-templates/" + editing.id, body);
          showNotice("模板已保存", "ok");
        } else {
          await api("POST", "/api/print-templates", body);
          showNotice("模板已创建", "ok");
        }
        closeEditor();
        await reload();
      } catch (error) {
        showNotice(error.message, "error");
      }
    }

    editorTarget.addEventListener("change", () => {
      if (editing) return;
      const parts = splitTarget(editorTarget.value);
      editingColumns = builtinColumns(parts.type, parts.key);
      renderColumns();
    });
    pageSizeSelect.addEventListener("change", updateCustomSize);
    for (const select of [orientationSelect, paperKindSelect, scaleModeSelect]) {
      select.addEventListener("change", updateLayoutHint);
    }
    for (const input of [marginInput, fontInput, paperWidthInput, paperHeightInput]) {
      input.addEventListener("input", updateLayoutHint);
    }
    if (showPageNoInput) showPageNoInput.addEventListener("change", updateLayoutHint);
    columnsTable.addEventListener("input", () => {
      syncColumnsFromDom();
      updateLayoutHint();
    });
    columnsTable.addEventListener("change", () => {
      syncColumnsFromDom();
      updateLayoutHint();
    });
    mount.querySelector('[data-role="save"]').addEventListener("click", saveEditor);
    mount.querySelector('[data-role="cancel"]').addEventListener("click", closeEditor);
    mount.querySelector('[data-role="copy-default"]').addEventListener("click", () => {
      syncColumnsFromDom();
      const parts = splitTarget(editorTarget.value);
      editingColumns = builtinColumns(parts.type, parts.key);
      renderColumns();
      showNotice("已按内置默认重置列", "ok");
    });
    columnsTable.addEventListener("click", (event) => {
      const button = event.target.closest("button");
      if (!button) return;
      const row = event.target.closest("tr");
      const index = Number(row.dataset.index);
      syncColumnsFromDom();
      if (button.dataset.role === "col-up" && index > 0) {
        [editingColumns[index - 1], editingColumns[index]] = [
          editingColumns[index],
          editingColumns[index - 1],
        ];
      } else if (button.dataset.role === "col-down" && index < editingColumns.length - 1) {
        [editingColumns[index + 1], editingColumns[index]] = [
          editingColumns[index],
          editingColumns[index + 1],
        ];
      } else {
        return;
      }
      renderColumns();
    });
    listTable.addEventListener("click", async (event) => {
      const button = event.target.closest("button");
      if (!button) return;
      const id = Number(event.target.closest("tr").dataset.id);
      const template = templates.find((item) => item.id === id);
      if (!template) return;
      try {
        if (button.dataset.role === "edit") {
          openEditor(template);
        } else if (button.dataset.role === "default") {
          await api("POST", "/api/print-templates/" + id + "/default");
          showNotice("已设为默认模板", "ok");
          await reload();
        } else if (button.dataset.role === "delete") {
          if (!(await askConfirm("确认删除模板「" + template.name + "」？删除后该目标回退到内置默认。"))) {
            return;
          }
          await api("DELETE", "/api/print-templates/" + id);
          if (editing && editing.id === id) closeEditor();
          showNotice("模板已删除", "ok");
          await reload();
        }
      } catch (error) {
        showNotice(error.message, "error");
      }
    });
    filterSelect.addEventListener("change", renderList);
    mount.querySelector('[data-role="refresh"]').addEventListener("click", () => {
      reload().catch((error) => showNotice(error.message, "error"));
    });
    mount.querySelector('[data-role="new"]').addEventListener("click", () => openEditor(null));

    targets = await api("GET", "/api/print/targets");
    fillTargetOptions(filterSelect, true);
    fillTargetOptions(editorTarget, false);
    await reload();
  }

  async function stockPage(mount) {
    let itemFilter = 0;

    mount.innerHTML = `
      <div class="page-head">
        <div>
          <h2>库存</h2>
          <p class="page-sub">余额由库存流水派生；流水只增不改</p>
        </div>
        <button class="btn" data-role="refresh">刷新</button>
      </div>
      <div class="card">
        <h3>库存余额</h3>
        <table class="table" data-role="balances"></table>
      </div>
      <div class="card">
        <div class="card-head">
          <h3>库存流水</h3>
          <select data-role="flow">
            <option value="">全部方向</option>
            <option value="in">仅入库</option>
            <option value="out">仅出库</option>
          </select>
        </div>
        <table class="table" data-role="ledger"></table>
      </div>
      <div class="card">
        <div class="card-head">
          <h3>成本重算核对</h3>
          <button class="btn small" data-role="recalc">开始核对</button>
        </div>
        <p class="page-sub">按库存流水重放移动加权平均，与当前账面结存比对；只读，不修改任何数据</p>
        <p class="page-sub" data-role="recalc-summary"></p>
        <table class="table" data-role="recalc"></table>
      </div>`;

    const balancesTable = mount.querySelector('[data-role="balances"]');
    const ledgerTable = mount.querySelector('[data-role="ledger"]');
    const ledgerFlowSelect = mount.querySelector('[data-role="flow"]');
    const recalcTable = mount.querySelector('[data-role="recalc"]');
    const recalcSummary = mount.querySelector('[data-role="recalc-summary"]');

    async function reload() {
      const [balances, ledger] = await Promise.all([
        api("GET", "/api/stock/balances"),
        api("GET", "/api/stock/ledger" + (itemFilter ? "?item_id=" + itemFilter : "")),
      ]);
      balancesTable.innerHTML = !balances.length
        ? '<tbody><tr><td class="empty">暂无库存</td></tr></tbody>'
        : `<thead><tr><th>商品编码</th><th>商品名称</th><th>仓库</th><th>数量</th><th>金额（元）</th></tr></thead>
           <tbody>${balances
             .map(
               (row) => `<tr>
                 <td>${escapeHtml(row.item_code)}</td>
                 <td>${escapeHtml(row.item_name)}</td>
                 <td>${escapeHtml(row.warehouse_name)}</td>
                 <td class="${qtyIsNegative(row.qty) ? "negative" : ""}">${qtyText(row.qty)}</td>
                 <td>${money(row.amount_cents)}</td>
               </tr>`
             )
             .join("")}</tbody>`;

      const visibleLedger = ledger.filter((row) => {
        if (!ledgerFlowSelect.value) return true;
        return ledgerFlowSelect.value === "in"
          ? Number(row.qty) > 0
          : Number(row.qty) < 0;
      });
      ledgerTable.innerHTML = !visibleLedger.length
        ? '<tbody><tr><td class="empty">暂无流水</td></tr></tbody>'
        : `<thead><tr><th>时间</th><th>类型</th><th>单据号</th><th>商品</th><th>仓库</th><th>数量</th><th>金额（元）</th></tr></thead>
           <tbody>${visibleLedger
             .map(
               (row) => `<tr>
                 <td>${escapeHtml(row.biz_time)}</td>
                 <td>${escapeHtml(row.biz_type_label || bizTypeText(row.biz_type))}</td>
                 <td>${escapeHtml(row.doc_no)}</td>
                 <td>${escapeHtml(row.item_code + " " + row.item_name)}</td>
                 <td>${escapeHtml(row.warehouse_name)}</td>
                 <td class="${qtyIsNegative(row.qty) ? "negative" : ""}">${qtyText(row.qty)}</td>
                 <td>${money(row.amount_cents)}</td>
               </tr>`
             )
             .join("")}</tbody>`;
    }

    mount.querySelector('[data-role="refresh"]').addEventListener("click", () => reload());
    ledgerFlowSelect.addEventListener("change", () => reload());
    mount.querySelector('[data-role="recalc"]').addEventListener("click", async () => {
      try {
        const result = await api(
          "GET",
          "/api/stock/cost-recalc" + (itemFilter ? "?item_id=" + itemFilter : "")
        );
        recalcSummary.textContent =
          `读取 ${result.ledger_rows} 条流水，核对 ${result.item_count} 个商品/仓库组合，` +
          `差异 ${result.difference_count} 条（数量差异 ${result.qty_difference_count} 条）`;
        if (!result.differences.length) {
          recalcTable.innerHTML =
            '<tbody><tr><td class="empty">账面结存与重算结果一致</td></tr></tbody>';
          return;
        }
        recalcTable.innerHTML = `
          <thead><tr><th>商品</th><th>仓库</th><th>账面数量</th><th>重算数量</th><th>数量差</th>
            <th>账面金额（元）</th><th>重算金额（元）</th><th>金额差（元）</th><th>处理</th></tr></thead>
          <tbody>${result.differences
            .map((row) => {
              const qtyDiff = qtyText(row.qty_diff) !== "0";
              const action = qtyDiff
                ? '<span class="page-sub">请用盘点单</span>'
                : can("document.write")
                ? `<button class="btn small" data-adjust-draft="${row.item_id}|${row.warehouse_id}">生成调整草稿</button>`
                : "";
              return `<tr>
                <td>${escapeHtml(row.item_code + " " + row.item_name)}</td>
                <td>${escapeHtml(row.warehouse_name)}</td>
                <td class="${qtyIsNegative(row.book_qty) ? "negative" : ""}">${qtyText(row.book_qty)}</td>
                <td class="${qtyIsNegative(row.replay_qty) ? "negative" : ""}">${qtyText(row.replay_qty)}</td>
                <td class="${qtyDiff ? "negative" : ""}">${qtyText(row.qty_diff)}</td>
                <td>${money(row.book_amount_cents)}</td>
                <td>${money(row.replay_amount_cents)}</td>
                <td class="${row.amount_diff_cents !== 0 ? "negative" : ""}">${money(
                  row.amount_diff_cents
                )}</td>
                <td>${action}</td>
              </tr>`;
            })
            .join("")}</tbody>`;
        recalcTable.querySelectorAll("[data-adjust-draft]").forEach((button) => {
          button.addEventListener("click", async () => {
            const [itemId, warehouseId] = button.dataset.adjustDraft.split("|");
            try {
              const created = await api("POST", "/api/cost-adjustments/drafts", {
                lines: [
                  {
                    item_id: Number(itemId),
                    warehouse_id: Number(warehouseId),
                    reason: "成本重算差异",
                  },
                ],
              });
              showNotice("已生成成本调整草稿 " + (created.doc_no || ""), "ok");
            } catch (error) {
              showNotice(error.message, "error");
            }
          });
        });
      } catch (error) {
        showNotice(error.message, "error");
      }
    });
    await reload();
  }

  // ---------- 用户管理 ----------

  const USER_ROLES = [
    { value: "admin", label: "管理员" },
    { value: "operator", label: "操作员" },
    { value: "viewer", label: "只读" },
  ];

  function roleLabel(code) {
    const role = USER_ROLES.find((item) => item.value === code);
    return role ? role.label : code;
  }

  // 内置管理员是单机版与命令行重置口令的兜底入口，服务端不允许改角色、停用或删除。
  // 这里把这些限制直接摆在界面上，而不是等提交后报错。
  function isBuiltinAdmin(row) {
    return row.username === "admin";
  }

  async function userPage(mount, ui) {
    const canWrite = ui.can("user.write");
    let rows = [];
    let editing = null;
    let mode = "create";

    mount.innerHTML = `
      <div class="page-head">
        <div>
          <h2>用户</h2>
          <p class="page-sub">账号、角色与口令。内置管理员 admin 不能改名、停用、删除或降级。</p>
        </div>
        ${canWrite ? '<button class="btn primary" data-role="new">新建用户</button>' : ""}
      </div>
      <div class="card" data-role="form-card" hidden>
        <form class="grid-form" data-role="form"></form>
        <div class="form-actions">
          <button class="btn primary" type="button" data-role="save">保存</button>
          <button class="btn" type="button" data-role="cancel">取消</button>
        </div>
      </div>
      <div class="card"><table class="table" data-role="table"></table></div>`;

    const formCard = mount.querySelector('[data-role="form-card"]');
    const form = mount.querySelector('[data-role="form"]');
    const table = mount.querySelector('[data-role="table"]');

    function roleOptions(selected) {
      return USER_ROLES.map(
        (role) =>
          `<option value="${role.value}" ${
            role.value === selected ? "selected" : ""
          }>${role.label}</option>`
      ).join("");
    }

    function openForm(row) {
      editing = row || null;
      mode = row ? "edit" : "create";
      const locked = row ? isBuiltinAdmin(row) : false;
      form.innerHTML = `
        <label class="field"><span>用户名 *</span>
          <input type="text" data-field="username" value="${escapeHtml(
            row ? row.username : ""
          )}" ${locked ? "disabled" : ""} required />
        </label>
        <label class="field"><span>显示名</span>
          <input type="text" data-field="display_name" value="${escapeHtml(
            row ? row.display_name : ""
          )}" />
        </label>
        <label class="field"><span>角色</span>
          <select data-field="role" ${locked ? "disabled" : ""}>${roleOptions(
        row ? row.role : "viewer"
      )}</select>
        </label>
        <label class="checkbox">
          <input type="checkbox" data-field="active" ${
            !row || row.active ? "checked" : ""
          } ${locked ? "disabled" : ""} /> 启用
        </label>
        ${
          row
            ? ""
            : `<label class="field"><span>初始口令 *</span>
                 <input type="password" data-field="password" autocomplete="new-password" required />
               </label>`
        }
        ${
          locked
            ? '<p class="page-sub">内置管理员只能修改显示名与口令：角色固定为管理员，账号不能停用或删除。</p>'
            : ""
        }`;
      formCard.hidden = false;
    }

    function openPassword(row) {
      editing = row;
      mode = "password";
      form.innerHTML = `
        <label class="field"><span>为 ${escapeHtml(row.username)} 设置新口令（8–128 个字符）</span>
          <input type="password" data-field="password" autocomplete="new-password" required />
        </label>`;
      formCard.hidden = false;
    }

    function renderTable() {
      if (!rows.length) {
        table.innerHTML = '<tbody><tr><td class="empty">暂无用户</td></tr></tbody>';
        return;
      }
      const body = rows
        .map((row) => {
          const builtin = isBuiltinAdmin(row);
          const lockTitle = builtin ? ' title="内置管理员不能停用、删除或降级"' : "";
          const actions = canWrite
            ? `<button class="link" data-role="edit">编辑</button>
               <button class="link" data-role="password">重置口令</button>
               ${
                 builtin
                   ? `<button class="link" disabled${lockTitle}>${row.active ? "停用" : "启用"}</button>
                      <button class="link danger" disabled${lockTitle}>删除</button>`
                   : `<button class="link" data-role="toggle">${row.active ? "停用" : "启用"}</button>
                      <button class="link danger" data-role="delete">删除</button>`
               }`
            : "";
          const status = builtin
            ? `${row.active ? "启用" : "停用"}<br /><span class="page-sub">内置账号，不可停用或删除</span>`
            : row.active
            ? "启用"
            : "停用";
          return `<tr data-id="${row.id}">
            <td>${escapeHtml(row.username)}</td>
            <td>${escapeHtml(row.display_name)}</td>
            <td>${escapeHtml(roleLabel(row.role))}</td>
            <td>${status}</td>
            <td>${escapeHtml(row.updated_at || row.created_at)}</td>
            <td class="actions">${actions}</td>
          </tr>`;
        })
        .join("");
      table.innerHTML = `<thead><tr><th>用户名</th><th>显示名</th><th>角色</th><th>状态</th><th>最后修改</th><th class="actions">操作</th></tr></thead><tbody>${body}</tbody>`;
    }

    async function reload() {
      rows = await api("GET", "/api/users");
      renderTable();
    }

    const newButton = mount.querySelector('[data-role="new"]');
    if (newButton) {
      newButton.addEventListener("click", () => openForm(null));
    }
    mount.querySelector('[data-role="cancel"]').addEventListener("click", () => {
      formCard.hidden = true;
    });

    mount.querySelector('[data-role="save"]').addEventListener("click", async () => {
      const field = (name) => form.querySelector('[data-field="' + name + '"]');
      try {
        if (mode === "password") {
          const password = field("password").value;
          if (!password) {
            showNotice("口令不能为空", "error");
            return;
          }
          await api("PUT", "/api/users/" + editing.id, {
            username: editing.username,
            display_name: editing.display_name,
            role: editing.role,
            active: editing.active,
            password: password,
          });
        } else {
          const payload = {
            username: field("username").value.trim(),
            display_name: field("display_name").value.trim(),
            role: field("role").value,
            active: field("active").checked,
          };
          if (!payload.username) {
            showNotice("用户名不能为空", "error");
            return;
          }
          if (mode === "create") {
            payload.password = field("password").value;
            if (!payload.password) {
              showNotice("请填写初始口令", "error");
              return;
            }
            await api("POST", "/api/users", payload);
          } else {
            await api("PUT", "/api/users/" + editing.id, payload);
          }
        }
        formCard.hidden = true;
        showNotice("");
        await reload();
      } catch (error) {
        showNotice(error.message, "error");
      }
    });

    table.addEventListener("click", async (event) => {
      const button = event.target.closest("button");
      if (!button || button.disabled) return;
      const row = rows.find(
        (item) => item.id === Number(event.target.closest("tr").dataset.id)
      );
      if (!row) return;
      const action = button.dataset.role;
      if (action === "edit") {
        openForm(row);
      } else if (action === "password") {
        openPassword(row);
      } else if (action === "toggle") {
        try {
          await api("PUT", "/api/users/" + row.id, {
            username: row.username,
            display_name: row.display_name,
            role: row.role,
            active: !row.active,
          });
          await reload();
        } catch (error) {
          showNotice(error.message, "error");
        }
      } else if (action === "delete") {
        if (!(await askConfirm("确认删除用户 " + row.username + "？该账号会立即退出登录。"))) return;
        try {
          await api("DELETE", "/api/users/" + row.id);
          await reload();
        } catch (error) {
          showNotice(error.message, "error");
        }
      }
    });

    await reload();
  }

  // ---------- 审计日志 ----------

  const AUDIT_ENTITIES = [
    ["", "全部对象"],
    ["document", "单据"],
    ["doc_header", "单据（信用额度）"],
    ["item", "商品"],
    ["item_category", "商品分类"],
    ["unit", "计量单位"],
    ["warehouse", "仓库"],
    ["customer", "客户"],
    ["supplier", "供应商"],
    ["employee", "职员"],
    ["settlement", "核销单"],
    ["fund_account", "资金账户"],
    ["print_template", "打印模板"],
    ["user", "用户"],
    ["auth", "登录"],
    ["report", "报表"],
    ["database", "数据库"],
    ["setting", "系统参数"],
    ["opening_stock", "库存期初"],
    ["opening_receivable", "应收期初"],
    ["opening_payable", "应付期初"],
    ["opening_fund", "资金期初"],
  ];

  const AUDIT_ACTIONS = {
    create: "新建",
    update: "修改",
    delete: "删除",
    save_draft: "存草稿",
    update_draft: "修改草稿",
    delete_draft: "删除草稿",
    approve: "审核",
    reverse: "反审核",
    convert_order: "订单转出库",
    close_order: "关闭订单",
    set_password: "重置口令",
    change_password: "修改口令",
    account_locked: "账号锁定",
    login: "登录",
    login_failed: "登录失败",
    export: "导出",
    backup: "备份",
    restore: "恢复",
    import: "导入",
    credit_over_limit: "超信用额度",
    role_change: "角色变更",
  };

  const AUDIT_ROLES = { admin: "管理员", operator: "操作员", viewer: "只读" };

  const AUDIT_SETTING_KEYS = {
    company_name: "公司名称",
    company_phone: "联系电话",
    company_address: "公司地址",
  };

  function auditEntityLabel(code) {
    const found = AUDIT_ENTITIES.find(([value]) => value === code);
    return found ? found[1] : code;
  }

  function auditActionLabel(row) {
    if (row.action === "reverse") {
      return row.entity === "settlement" ? "反核销" : "反审核";
    }
    return AUDIT_ACTIONS[row.action] || row.action;
  }

  function auditRoleLabel(code) {
    return AUDIT_ROLES[code] || code || "—";
  }

  function auditMoney(cents) {
    const value = Number(cents || 0) / 100;
    return (
      value.toLocaleString("zh-CN", { minimumFractionDigits: 2, maximumFractionDigits: 2 }) +
      " 元"
    );
  }

  function auditHasValue(value) {
    return value !== undefined && value !== null && String(value).trim() !== "";
  }

  // 摘要只做展示层翻译：detail 仍按后端写入的 JSON/纯文本保存，
  // 未知结构或解析失败一律原样显示，不吞内容也不报错。
  function auditDetailText(row, accountNames) {
    const raw = row.detail || "";
    if (!raw) {
      return row.action === "reverse" ? "红字回冲" : "—";
    }
    let data = null;
    try {
      data = JSON.parse(raw);
    } catch (error) {
      return raw;
    }
    if (!data || typeof data !== "object" || Array.isArray(data)) {
      return raw;
    }
    if (data.doc_type && ["approve", "save_draft", "update_draft"].includes(row.action)) {
      const parts = [bizTypeText(data.doc_type)];
      if (auditHasValue(data.qty)) parts.push("数量 " + data.qty);
      if (auditHasValue(data.amount_cents)) parts.push("金额 " + auditMoney(data.amount_cents));
      return parts.join(" · ");
    }
    if (row.action === "convert_order" && data.out_doc_no) {
      return (
        "转出库 " + data.out_doc_no + (auditHasValue(data.qty) ? " · 数量 " + data.qty : "")
      );
    }
    if (row.entity === "settlement" && row.action === "create") {
      const parts = [Number(data.direction) === -1 ? "付款" : "收款"];
      if (auditHasValue(data.amount_cents)) parts.push("金额 " + auditMoney(data.amount_cents));
      if (auditHasValue(data.account_id)) {
        const id = Number(data.account_id);
        const name = id > 0 ? accountNames.get(id) : "";
        parts.push("账户 " + (name || (id > 0 ? "#" + id : "未指定")));
      }
      return parts.join(" · ");
    }
    if (row.entity === "settlement" && row.action === "reverse") {
      return "原单 " + (data.reverses || "—") + "（红字冲回）";
    }
    if (row.entity === "user") {
      if (row.action === "role_change") {
        return "角色从 " + auditRoleLabel(data.from) + " 改为 " + auditRoleLabel(data.to);
      }
      if (row.action === "create" || row.action === "update") {
        const parts = [];
        if (auditHasValue(data.role)) parts.push("角色 " + auditRoleLabel(data.role));
        if (typeof data.active === "boolean") parts.push(data.active ? "启用" : "停用");
        return parts.length ? parts.join(" · ") : raw;
      }
      return raw;
    }
    if (row.entity === "setting" && row.action === "update") {
      const label = AUDIT_SETTING_KEYS[row.entity_no] || row.entity_no || "参数";
      const from = auditHasValue(data.from) ? data.from : "未设置";
      const to = auditHasValue(data.to) ? data.to : "未设置";
      return label + "从 " + from + " 改为 " + to;
    }
    return raw;
  }

  async function auditPage(mount) {
    let page = 1;
    let pages = 1;
    const pageSize = 50;

    const entityOptions = AUDIT_ENTITIES.map(
      ([value, label]) => `<option value="${value}">${label}</option>`
    ).join("");

    mount.innerHTML = `
      <div class="page-head">
        <div>
          <h2>审计日志</h2>
          <p class="page-sub">只读记录：谁在何时对什么对象做了什么</p>
        </div>
      </div>
      <div class="card">
        <form class="grid-form" data-role="filters">
          <label class="field"><span>开始日期</span><input type="date" data-field="from" /></label>
          <label class="field"><span>结束日期</span><input type="date" data-field="to" /></label>
          <label class="field"><span>对象类型</span><select data-field="entity">${entityOptions}</select></label>
          <label class="field"><span>对象 id</span><input type="number" min="0" step="1" data-field="entity_id" /></label>
          <label class="field"><span>操作人</span><input type="text" data-field="actor" placeholder="用户名" /></label>
          <label class="field"><span>来源 IP</span><input type="text" data-field="client_ip" placeholder="如 192.168.1.20" /></label>
          <label class="field"><span>请求号</span><input type="text" data-field="request_id" placeholder="排障用请求标识" /></label>
          <div class="form-actions">
            <button class="btn primary" type="submit">查询</button>
            <button class="btn" type="button" data-role="reset">重置</button>
          </div>
        </form>
      </div>
      <div class="card">
        <table class="table" data-role="table"></table>
        <div class="form-actions">
          <button class="btn" type="button" data-role="prev">上一页</button>
          <span class="page-sub" data-role="page-info"></span>
          <button class="btn" type="button" data-role="next">下一页</button>
          <button class="btn" type="button" data-role="export-csv">导出 CSV</button>
          <button class="btn" type="button" data-role="export-xlsx">导出 Excel</button>
        </div>
      </div>`;

    const form = mount.querySelector('[data-role="filters"]');
    const table = mount.querySelector('[data-role="table"]');
    const info = mount.querySelector('[data-role="page-info"]');
    const prevButton = mount.querySelector('[data-role="prev"]');
    const nextButton = mount.querySelector('[data-role="next"]');

    const field = (name) => form.querySelector('[data-field="' + name + '"]');

    const filterNames = ["from", "to", "entity", "entity_id", "actor", "client_ip", "request_id"];
    const filterParams = () => {
      const params = new URLSearchParams();
      for (const name of filterNames) {
        const value = field(name).value.trim();
        if (value) params.set(name, value);
      }
      return params;
    };

    // 核销审计只存 account_id，页面尽量换成账户名；没有权限或取不到时退回 #id。
    const accountNames = new Map();
    api("GET", "/api/fund-accounts")
      .then((result) => {
        const rows = Array.isArray(result) ? result : result.rows || [];
        for (const account of rows) {
          accountNames.set(Number(account.id), account.name || account.code || "#" + account.id);
        }
      })
      .catch(() => {});

    async function reload() {
      const params = filterParams();
      params.set("page", String(page));
      params.set("page_size", String(pageSize));
      try {
        const result = await api("GET", "/api/audit-logs?" + params.toString());
        const total = Number(result.total || 0);
        pages = Math.max(1, Math.ceil(total / Number(result.page_size || pageSize)));
        info.textContent = "第 " + result.page + " / " + pages + " 页 · 共 " + total + " 条";
        prevButton.disabled = result.page <= 1;
        nextButton.disabled = result.page >= pages;
        if (!result.items.length) {
          table.innerHTML = '<tbody><tr><td class="empty">没有符合条件的记录</td></tr></tbody>';
          return;
        }
        table.innerHTML = `<thead><tr><th>时间</th><th>操作人</th><th>对象</th><th>对象 id</th><th>动作</th><th>来源 IP</th><th>请求号</th><th>摘要</th></tr></thead><tbody>${result.items
          .map(
            (row) => `<tr>
              <td>${escapeHtml(row.at)}</td>
              <td>${escapeHtml(row.actor)}</td>
              <td>${escapeHtml(auditEntityLabel(row.entity))}${
              row.entity_no ? " " + escapeHtml(row.entity_no) : ""
            }</td>
              <td>${row.entity_id || "—"}</td>
              <td>${escapeHtml(auditActionLabel(row))}</td>
              <td>${row.client_ip ? escapeHtml(row.client_ip) : "—"}</td>
              <td>${row.request_id ? escapeHtml(row.request_id) : "—"}</td>
              <td class="audit-detail"><span class="audit-detail-text">${escapeHtml(
                auditDetailText(row, accountNames)
              )}</span></td>
            </tr>`
          )
          .join("")}</tbody>`;
      } catch (error) {
        showNotice(error.message, "error");
      }
    }

    form.addEventListener("submit", (event) => {
      event.preventDefault();
      page = 1;
      reload();
    });
    mount.querySelector('[data-role="reset"]').addEventListener("click", () => {
      for (const name of ["from", "to", "entity_id", "actor", "client_ip", "request_id"]) {
        field(name).value = "";
      }
      field("entity").value = "";
      page = 1;
      reload();
    });
    mount.querySelector('[data-role="export-csv"]').addEventListener("click", async () => {
      const params = filterParams();
      params.set("format", "csv");
      try {
        await downloadCsv("/api/audit-logs/export?" + params.toString());
        showNotice("已导出 CSV", "ok");
      } catch (error) {
        showNotice(error.message, "error");
      }
    });
    mount.querySelector('[data-role="export-xlsx"]').addEventListener("click", async () => {
      const params = filterParams();
      params.set("format", "xlsx");
      try {
        await downloadXlsx("/api/audit-logs/export?" + params.toString());
        showNotice("已导出 Excel", "ok");
      } catch (error) {
        showNotice(error.message, "error");
      }
    });
    prevButton.addEventListener("click", () => {
      if (page > 1) {
        page -= 1;
        reload();
      }
    });
    nextButton.addEventListener("click", () => {
      if (page < pages) {
        page += 1;
        reload();
      }
    });

    await reload();
  }

  // ---------- 系统参数 ----------

  // 只开放公司抬头三项；编号规则、日期与金额格式仍是固定口径，只读展示。
  async function settingPage(mount) {
    const writable = can("setting.write");
    const showLocalRemember =
      state.mode === "local" &&
      (state.localGate || {}).password_required === true &&
      writable &&
      keychainAvailable();
    mount.innerHTML = `
      <div class="page-head">
        <div>
          <h2>系统参数</h2>
          <p class="page-sub">公司抬头用于打印占位符与默认抬头；其余展示口径当前固定</p>
        </div>
      </div>
      <div class="card">
        <form class="grid-form" data-role="form">
          <label class="field"><span>公司名称</span><input type="text" maxlength="100" data-field="name" ${writable ? "" : "disabled"} /></label>
          <label class="field"><span>联系电话</span><input type="text" maxlength="50" data-field="phone" ${writable ? "" : "disabled"} /></label>
          <label class="field"><span>公司地址</span><input type="text" maxlength="200" data-field="address" ${writable ? "" : "disabled"} /></label>
          <div class="field">
            <span>允许负库存出库</span>
            <label class="inline-check"><input type="checkbox" data-field="allow_negative_stock" ${writable ? "" : "disabled"} /> <span>开启后出库可把结存做成负数</span></label>
            <span class="page-sub" data-role="negative-state"></span>
          </div>
          <label class="field"><span>会话空闲超时（分钟）</span><input type="number" min="1" max="1440" step="1" data-field="session_idle_minutes" ${writable ? "" : "disabled"} /></label>
          <label class="field"><span>会话绝对上限（小时）</span><input type="number" min="1" max="720" step="1" data-field="session_absolute_hours" ${writable ? "" : "disabled"} /></label>
          <label class="field"><span>库存成本口径</span>
            <select data-field="cost_basis" ${writable ? "" : "disabled"}>
              <option value="INCLUSIVE">含税价</option>
              <option value="NET">不含税净额</option>
            </select>
            <span class="page-sub">一般纳税人可切「不含税」；小规模纳税人保持「含税」。切换只影响此后审核的单据，历史结存与毛利不重算。</span>
            <span class="page-sub" data-role="basis-state"></span>
          </label>
          <span class="page-sub">空闲超时按最近访问滑动续期；绝对上限从登录时刻起算，超过后需重新登录。</span>
          <div class="form-actions">
            ${writable ? '<button class="btn primary" type="submit">保存</button>' : '<span class="page-sub">当前角色只读</span>'}
          </div>
        </form>
      </div>
      <div class="card">
        <h3>固定口径（只读）</h3>
        <p class="page-sub">单据编号：前缀 + 日期 YYYYMMDD + 4 位序号，例如 XSCK20261113-0001；前缀按单据类型固定，暂不可改。</p>
        <p class="page-sub">日期显示固定 YYYY-MM-DD；金额为人民币元、2 位小数；数量小数位跟随商品单位设置（0–4 位）。</p>
        <p class="page-sub">打印占位符：{{company_name}}、{{company_phone}}、{{company_address}}；模板抬头为空时默认打印公司名称。</p>
      </div>
      ${
        showLocalRemember
          ? `<div class="card" data-role="local-remember">
        <h3>本机免输</h3>
        <p class="page-sub">钥匙串只保存服务端签发的随机凭据，不保存本机口令；凭据 30 天后失效。</p>
        <label class="inline-check"><input type="radio" name="local-remember" value="on" /> <span>30 天内免输本机口令</span></label>
        <label class="inline-check"><input type="radio" name="local-remember" value="off" /> <span>每次启动都要输入本机口令</span></label>
        <p class="page-sub" data-role="local-remember-state"></p>
        <div class="form-actions">
          <button class="btn" type="button" data-role="local-remember-save">保存本机免输设置</button>
          <button class="btn" type="button" data-role="local-remember-clear">清除本机记忆</button>
        </div>
      </div>`
          : ""
      }`;

    const form = mount.querySelector('[data-role="form"]');
    const input = (name) => form.querySelector('[data-field="' + name + '"]');
    const negativeState = mount.querySelector('[data-role="negative-state"]');
    const basisState = mount.querySelector('[data-role="basis-state"]');

    function renderNegativeState() {
      negativeState.textContent = input("allow_negative_stock").checked
        ? "当前：开启（出库超库存会形成负结存）"
        : "当前：关闭（出库超库存会被拒绝）";
    }

    function renderBasisState(settings) {
      const label = settings.cost_basis === "NET" ? "不含税净额" : "含税价";
      basisState.textContent = settings.cost_basis_changed_at
        ? "当前：" + label + "；切换于 " + settings.cost_basis_changed_at + "，只影响新审核单据"
        : "当前：" + label + "；尚未切换过口径，只影响新审核单据";
    }

    try {
      const data = await api("GET", "/api/settings");
      const company = data.company || {};
      input("name").value = company.name || "";
      input("phone").value = company.phone || "";
      input("address").value = company.address || "";
      input("allow_negative_stock").checked = data.allow_negative_stock === true;
      input("session_idle_minutes").value = data.session_idle_minutes || 30;
      input("session_absolute_hours").value = data.session_absolute_hours || 12;
      input("cost_basis").value = data.cost_basis === "NET" ? "NET" : "INCLUSIVE";
      renderNegativeState();
      renderBasisState(data);
    } catch (error) {
      showNotice(error.message, "error");
      return;
    }

    input("allow_negative_stock").addEventListener("change", renderNegativeState);

    form.addEventListener("submit", async (event) => {
      event.preventDefault();
      if (!writable) return;
      try {
        await api("PUT", "/api/settings", {
          company: {
            name: input("name").value.trim(),
            phone: input("phone").value.trim(),
            address: input("address").value.trim(),
          },
          allow_negative_stock: input("allow_negative_stock").checked,
          session_idle_minutes: Number(input("session_idle_minutes").value),
          session_absolute_hours: Number(input("session_absolute_hours").value),
          cost_basis: input("cost_basis").value,
        });
        renderNegativeState();
        renderBasisState(await api("GET", "/api/settings"));
        showNotice("系统参数已保存", "ok");
      } catch (error) {
        showNotice(error.message, "error");
      }
    });

    // 单机版「本机免输」：开关改服务端策略，凭据本体在 macOS 钥匙串里。
    const rememberCard = mount.querySelector('[data-role="local-remember"]');
    if (rememberCard) {
      const rememberState = rememberCard.querySelector('[data-role="local-remember-state"]');
      const onInput = rememberCard.querySelector('input[value="on"]');
      const offInput = rememberCard.querySelector('input[value="off"]');
      async function refreshRemember() {
        const data = await api("GET", "/api/local/remember/setting");
        const enabled = data.enabled !== false;
        onInput.checked = enabled;
        offInput.checked = !enabled;
        rememberState.textContent = enabled
          ? data.has_token
            ? "当前：30 天内免输；系统钥匙串中已有凭据。"
            : "当前：30 天内免输；下次输入口令后写入系统钥匙串。"
          : "当前：每次启动都要输入本机口令。";
      }
      try {
        await refreshRemember();
      } catch (error) {
        showNotice(error.message, "error");
      }
      rememberCard
        .querySelector('[data-role="local-remember-save"]')
        .addEventListener("click", async () => {
          const enabled = onInput.checked;
          try {
            const result = await api("PUT", "/api/local/remember/setting", { enabled });
            if (enabled && result.remember_token) {
              if (!(await saveRememberToken(result.remember_token))) {
                showNotice("设置已保存，但未能写入系统钥匙串；下次启动仍需输入口令", "error");
                await refreshRemember();
                return;
              }
            } else if (!enabled) {
              await clearRememberToken();
            }
            await refreshRemember();
            showNotice("本机免输设置已保存", "ok");
          } catch (error) {
            showNotice(error.message, "error");
          }
        });
      rememberCard
        .querySelector('[data-role="local-remember-clear"]')
        .addEventListener("click", async () => {
          if (
            !(await askConfirm("清除后，下次启动需要重新输入本机口令。", {
              okText: "清除",
              cancelText: "取消",
            }))
          ) {
            return;
          }
          try {
            await api("POST", "/api/local/remember/clear");
            await clearRememberToken();
            await refreshRemember();
            showNotice("本机记忆已清除", "ok");
          } catch (error) {
            showNotice(error.message, "error");
          }
        });
    }
  }

  // ---------- 关于与诊断 ----------

  function formatBytes(bytes) {
    const value = Number(bytes || 0);
    if (value < 1024) return value + " B";
    if (value < 1024 * 1024) return (value / 1024).toFixed(1) + " KB";
    if (value < 1024 * 1024 * 1024) return (value / 1024 / 1024).toFixed(1) + " MB";
    return (value / 1024 / 1024 / 1024).toFixed(2) + " GB";
  }

  // 强制升级原因文案：清单 mandatory_reason 与服务端 MIN_SUPPORTED 共用。
  function mandatoryReasonText(reason) {
    const labels = {
      SECURITY: "安全漏洞修复",
      PROTOCOL: "协议不兼容",
      REGULATORY: "法规或税控要求",
      MIN_SUPPORTED: "低于服务端最低支持版本",
    };
    return labels[reason] || "";
  }

  // macOS v2 清单没有构建号（build 固定 0）：没有构建号时只显示版本号。
  function versionWithBuild(version, build) {
    const text = version || "新版本";
    return build ? `${text}（构建 ${build}）` : text;
  }

  // ---------- 桌面壳更新桥 ----------
  // macOS：WKWebView messageHandlers（方法名 xiaotuUpdateXxx，结果由原生调 window.xxxDone）
  // Windows：WebView2 HostObject updater_win（方法名 checkNow/statusJson…，结果走 chrome.webview
  // 的 message 事件）。两边字段名也不同，这里统一适配成 renderNativeUpdate 期望的形状。
  let winUpdaterBridgeInstance = null;
  let winUpdaterListenerInstalled = false;
  let winUpdaterStatusCache = null;

  function normalizeWinUpdateStatus(raw) {
    const s = raw || {};
    const state = s.download_state || "";
    let prefetchState = "";
    if (state === "RUNNING") prefetchState = "running";
    else if (state === "DONE") prefetchState = "done";
    else if (state === "FAILED") prefetchState = "failed";
    else if (state === "CANCELED") prefetchState = "canceled";
    // 壳内的 download_progress 形如 "1234/5678 (22%)"，取出已下载字节用于进度展示。
    let prefetchBytes = 0;
    const progressMatch = /^(\d+)\//.exec(s.download_progress || "");
    if (progressMatch) prefetchBytes = Number(progressMatch[1]) || 0;
    return {
      channel: s.channel || "stable",
      last_check_at: s.last_check_epoch || 0,
      last_result: s.last_result || "",
      update_available: !!s.manifest_version,
      pending_version: s.manifest_version || "",
      pending_build: s.manifest_build || 0,
      pending_size: s.manifest_size || s.pending_size || 0,
      pending_deadline: s.mandatory_deadline || "",
      mandatory: !!s.mandatory,
      reason: s.mandatory_reason || "",
      auto_check: s.auto_check !== false,
      prefetch: s.prefetch !== false,
      prefetch_state: prefetchState,
      prefetch_bytes: prefetchBytes,
      prefetch_total: s.manifest_size || 0,
      prefetch_version: s.manifest_version || "",
      prefetch_file: s.pending_local_path || "",
      previous: { available: false },
      recovery_required: false,
    };
  }

  // 把 Windows 壳的检查结果码映射成界面文案表的键
  function mapWinCheckResult(code) {
    if (code === "UPDATE_AVAILABLE" || code === "NO_UPDATE") return code;
    if (code === "UPDATE_UNAVAILABLE") return "NOT_CONFIGURED";
    return "CHECK_FAILED";
  }

  function parseWebviewMessage(event) {
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

  function winUpdateBridge() {
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
      winUpdaterStatusCache = parsed;
      return parsed;
    };

    if (!winUpdaterListenerInstalled) {
      winUpdaterListenerInstalled = true;
      window.chrome.webview.addEventListener("message", (event) => {
        const msg = parseWebviewMessage(event);
        if (!msg || !msg.event || msg.event === "printPdf") return;
        if (msg.status) winUpdaterStatusCache = msg.status;
        const status = normalizeWinUpdateStatus(msg.status || winUpdaterStatusCache || {});
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
        }
        const failed = /FAILED|INVALID|MISMATCH|PARSE_ERROR|UNAVAILABLE/.test(msg.code || "");
        finish(done, {
          status,
          code: msg.code,
          ok: msg.event === "checkNow" ? !failed : msg.code !== "FAILED",
          update_available: status.update_available,
        });
      });
    }

    const noop = { postMessage: () => {} };
    winUpdaterBridgeInstance = {
      xiaotuUpdateStatus: {
        postMessage: () => {
          (async () => {
            try {
              const s = await readStatus();
              finish("xiaotuUpdateStatusDone", normalizeWinUpdateStatus(s));
            } catch (error) {
              finish("xiaotuUpdateStatusDone", normalizeWinUpdateStatus({}));
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
              if (s.pending_local_path) {
                updater.installUpdate();
                return;
              }
              // 还没下载：先下载，收到 DOWNLOADED 后自动安装
              const onMessage = (event) => {
                const msg = parseWebviewMessage(event);
                if (!msg || msg.event !== "downloadUpdate") return;
                window.chrome.webview.removeEventListener("message", onMessage);
                if (msg.code === "DOWNLOADED") updater.installUpdate();
                else finish("xiaotuUpdateInstallDone", { ok: false, code: msg.code });
              };
              window.chrome.webview.addEventListener("message", onMessage);
              updater.downloadUpdate();
            } catch (error) {
              finish("xiaotuUpdateInstallDone", { ok: false, code: "BRIDGE_ERROR" });
            }
          })();
        },
      },
      // Windows 壳暂不支持回退 / 恢复 / 打开备份目录：给空实现，避免调到 undefined
      xiaotuUpdateRollback: noop,
      xiaotuUpdateResume: noop,
      xiaotuUpdateOpenBackupFolder: noop,
    };
    return winUpdaterBridgeInstance;
  }

  function nativeUpdateBridge() {
    if (window.webkit && window.webkit.messageHandlers) return window.webkit.messageHandlers;
    return winUpdateBridge();
  }

  // 强制升级拦截：壳内状态一到就弹不可跳过的提示，只能点「立即安装」。
  // blocker 只弹一次；安装由壳交给 Sparkle 完成，安装包会重新下载。
  let mandatoryBlockOpen = false;
  function blockMandatoryUpdate(status) {
    if (mandatoryBlockOpen || !status || !status.mandatory || !status.update_available) return;
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
  function requestNativeUpdateStatus(onStatus) {
    const bridge = nativeUpdateBridge();
    if (!bridge || !bridge.xiaotuUpdateStatus) return;
    const previous = window.xiaotuUpdateStatusDone;
    window.xiaotuUpdateStatusDone = (payload) => {
      window.xiaotuUpdateStatusDone = previous;
      const status = payload && payload.status ? payload.status : payload;
      if (status) onStatus(status);
    };
    bridge.xiaotuUpdateStatus.postMessage({});
  }

  // 软件更新：状态、下载、安装都在服务端完成；前端只展示结果与发起动作。
  // 不自动安装：下载后仍需管理员点「安装更新」并二次确认。
  // 原生自动更新卡片：由壳内 XiaotuUpdater 提供状态、检查与安装。
  function renderNativeUpdate(card, nativeBridge) {
    const resultText = (status) => {
      const labels = {
        UPDATE_AVAILABLE: "发现新版本",
        NO_UPDATE: "已是最新版本",
        CHECK_FAILED: "检查失败（网络或清单验签未通过）",
        PAUSED: "自动更新已暂停（连续启动失败）",
        SKIPPED: "已跳过该版本",
        INCOMPATIBLE_SCHEMA: "版本不兼容，需先升级到中间版本",
        INTERMEDIATE_REQUIRED: "需先安装中间版本",
        OS_UNSUPPORTED: "当前系统版本过低，不能安装该更新",
        INCOMPATIBLE_API: "客户端接口版本不兼容",
        NOT_IN_ROLLOUT: "该版本暂未对当前安装开放（灰度）",
        NOT_CONFIGURED: "未配置更新源",
      };
      const last = status && status.last_result;
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
      if (state === "retrying") {
        return status.prefetch_message || "下载失败，稍后自动重试";
      }
      if (state === "done") {
        return `已下载并校验：${status.prefetch_file || ""}（${bytes}）`;
      }
      if (state === "failed") {
        return `后台下载失败：${status.prefetch_message || "可手动下载"}`;
      }
      if (state === "canceled") {
        return "已取消本次后台下载，可手动下载";
      }
      if (state === "needs_consent") {
        return "首次后台下载前需要确认流量提示";
      }
      return "—";
    }

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
      const prefetchRunning =
        status.prefetch_state === "running" || status.prefetch_state === "retrying";
      const prefetchDone = status.prefetch_state === "done";
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
        ["后台下载", prefetchText(status)],
      ];
      const consent =
        status.prefetch_state === "needs_consent"
          ? `<p class="page-sub" style="margin-top:10px">
               发现新版本时会在后台自动下载安装包，可能产生网络流量并占用磁盘空间。
               可随时关闭上方开关。
             </p>
             <button class="btn" type="button" data-role="ack-prefetch">我知道了，开始后台下载</button>`
          : "";
      // 强制升级：阻塞式提示，不提供“跳过此版本”，安装入口保持可用。
      const reason = mandatoryReasonText(status.reason);
      const mandatory =
        status.update_available && status.mandatory
          ? `<div class="notice error" style="margin:10px 0">
               <strong>必须升级</strong>：${
                 reason ? escapeHtml(reason) + "，" : ""
               }当前版本需升级到 ${escapeHtml(
                 status.pending_version || "新版本"
               )} 后才能继续使用，此更新不能跳过。${
                 status.pending_deadline
                   ? `请于 ${escapeHtml(status.pending_deadline)} 前完成升级。`
                   : ""
               }
             </div>`
          : "";
      // 恢复模式：连续启动失败后引导回退。回退只替换程序，数据库结构变更需先恢复升级前备份。
      const previous = status.previous || {};
      const recovery = status.recovery_required
        ? `<div style="margin:8px 0;padding:12px;border:1px solid #e0b252;background:#fff8e6">
             <p style="margin:0 0 6px"><strong>恢复模式</strong>：当前版本连续 3 次启动未稳定，自动检查已暂停。</p>
             <p class="page-sub" style="margin:0 0 6px">${
               previous.available
                 ? `可回退到安装前保留的上一版本 ${escapeHtml(
                     previous.version || ""
                   )}（构建 ${escapeHtml(String(previous.build || ""))}）。`
                 : "旧版本副本不可用，请从官网重新下载上一版本安装包。"
             }</p>
             <p class="page-sub" style="margin:0 0 10px">
               若升级包含数据库结构变更，回退后旧版本可能无法直接打开数据；
               请先用升级前的自动备份（backups/app-*-pre-v*.db）按用户手册恢复。
             </p>
             <div style="display:flex;gap:8px;flex-wrap:wrap">
               ${
                 previous.available
                   ? `<button class="btn primary" type="button" data-role="rollback">回退到上一版本</button>`
                   : ""
               }
               <button class="btn" type="button" data-role="recovery-dismiss">继续使用当前版本</button>
               <button class="btn" type="button" data-role="open-backups">打开备份目录</button>
             </div>
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
        ${recovery}
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
          <label class="checkbox"><input type="checkbox" data-role="prefetch" ${
            status.prefetch ? "checked" : ""
          } /> 发现新版本时自动下载安装包</label>
        </div>
        ${consent}
        <div style="display:flex;gap:8px;flex-wrap:wrap;margin-top:12px">
          <button class="btn" type="button" data-role="check">检查更新</button>
          ${
            status.update_available
              ? `<button class="btn" type="button" data-role="download" ${
                  prefetchRunning || prefetchDone ? "disabled" : ""
                }>${prefetchDone ? "已下载" : "下载并校验"}</button>`
              : ""
          }
          ${
            prefetchRunning
              ? `<button class="btn" type="button" data-role="cancel">取消下载</button>`
              : ""
          }
          <button class="btn primary" type="button" data-role="install" ${
            status.update_available ? "" : "disabled"
          }>安装更新</button>
        </div>`;

      const checkButton = card.querySelector('[data-role="check"]');
      checkButton.addEventListener("click", () => {
        checkButton.disabled = true;
        checkButton.textContent = "检查中…";
        nativeBridge.xiaotuUpdateCheckNow.postMessage({});
      });
      const autoInput = card.querySelector('[data-role="auto-check"]');
      autoInput.addEventListener("change", () => {
        autoInput.disabled = true;
        nativeBridge.xiaotuUpdateOptions.postMessage({ auto_check: autoInput.checked });
      });
      const prefetchInput = card.querySelector('[data-role="prefetch"]');
      prefetchInput.addEventListener("change", async () => {
        if (prefetchInput.checked && status.prefetch_notice_acked !== true) {
          const confirmed = await askConfirm(
            "发现新版本时会在后台自动下载安装包，可能产生网络流量并占用磁盘空间。" +
              "可随时在此处关闭，是否开启？",
            { okText: "开启", cancelText: "取消" }
          );
          if (!confirmed) {
            prefetchInput.checked = false;
            return;
          }
          nativeBridge.xiaotuUpdateOptions.postMessage({
            prefetch: true,
            ack_prefetch_notice: true,
          });
          return;
        }
        nativeBridge.xiaotuUpdateOptions.postMessage({ prefetch: prefetchInput.checked });
      });
      const downloadButton = card.querySelector('[data-role="download"]');
      if (downloadButton) {
        downloadButton.addEventListener("click", () => {
          downloadButton.disabled = true;
          nativeBridge.xiaotuUpdateDownload.postMessage({});
        });
      }
      const cancelButton = card.querySelector('[data-role="cancel"]');
      if (cancelButton) {
        cancelButton.addEventListener("click", () => {
          cancelButton.disabled = true;
          nativeBridge.xiaotuUpdateCancelPrefetch.postMessage({});
        });
      }
      const ackButton = card.querySelector('[data-role="ack-prefetch"]');
      if (ackButton) {
        ackButton.addEventListener("click", () => {
          ackButton.disabled = true;
          nativeBridge.xiaotuUpdateOptions.postMessage({
            prefetch: true,
            ack_prefetch_notice: true,
          });
        });
      }
      card.querySelector('[data-role="install"]').addEventListener("click", async () => {
        const size = status.pending_size
          ? `约 ${formatBytes(status.pending_size)}`
          : "安装包";
        const confirmed = await askConfirm(
          status.mandatory
            ? `此更新为必须升级（${mandatoryReasonText(status.reason) || "服务端要求"}），不能跳过。` +
                `安装时会重新下载${size}，随后关闭程序并完成安装。是否继续？`
            : `将安装 ${versionWithBuild(
                status.pending_version,
                status.pending_build
              )}。安装时会重新下载${size}（后台预下载只用于提前校验），` +
                "随后关闭程序并完成安装。是否继续？",
          { okText: "安装", cancelText: "取消" }
        );
        if (!confirmed) return;
        nativeBridge.xiaotuUpdateInstall.postMessage({});
      });
      const rollbackButton = card.querySelector('[data-role="rollback"]');
      if (rollbackButton) {
        rollbackButton.addEventListener("click", async () => {
          const confirmed = await askConfirm(
            "回退会退出程序，用上一版本替换当前程序并重新启动。" +
              "若数据库结构已升级，请先恢复升级前备份；是否继续？",
            { okText: "回退", cancelText: "取消" }
          );
          if (!confirmed) return;
          rollbackButton.disabled = true;
          nativeBridge.xiaotuUpdateRollback.postMessage({});
        });
      }
      const dismissRecoveryButton = card.querySelector('[data-role="recovery-dismiss"]');
      if (dismissRecoveryButton) {
        dismissRecoveryButton.addEventListener("click", () => {
          dismissRecoveryButton.disabled = true;
          nativeBridge.xiaotuUpdateResume.postMessage({});
        });
      }
      const openBackupsButton = card.querySelector('[data-role="open-backups"]');
      if (openBackupsButton) {
        openBackupsButton.addEventListener("click", () => {
          openBackupsButton.disabled = true;
          nativeBridge.xiaotuUpdateOpenBackupFolder.postMessage({});
        });
      }
      schedulePoll(status);
    }

    // 状态回调直接传 status 本体；检查回调里是 {status: ...} 包装。
    window.xiaotuUpdateStatusDone = (payload) =>
      render(payload && payload.status ? payload.status : payload);
    window.xiaotuUpdateCheckDone = (payload) => {
      const status = (payload && payload.status) || {};
      render(status);
      if (payload && payload.code === "COOLDOWN") {
        showNotice("检查过于频繁，请稍后再试", "error");
      } else if (payload && payload.update_available) {
        showNotice("发现新版本，可点「安装更新」完成替换", "ok");
      } else if (payload && payload.ok === false) {
        showNotice(resultText(status), "error");
      } else {
        showNotice(resultText(status), "ok");
      }
    };
    window.xiaotuUpdateInstallDone = (payload) => {
      if (payload && payload.ok === false) {
        showNotice("无法启动安装流程：" + (payload.code || ""), "error");
      } else if (payload && payload.code === "OPENED") {
        showNotice(payload.message || "已打开安装包，请退出本程序后完成安装", "ok");
      }
    };
    window.xiaotuUpdateOptionsDone = (payload) => {
      if (payload && payload.ok === false) {
        showNotice("设置未保存：" + (payload.code || ""), "error");
      }
      render((payload && payload.status) || {});
    };
    window.xiaotuUpdateDownloadDone = (payload) => {
      const status = (payload && payload.status) || {};
      render(status);
      if (payload && payload.ok === false) {
        showNotice(
          payload.code === "NO_PENDING_UPDATE"
            ? "没有可下载的待更新版本"
            : "下载未能开始：" + (payload.code || ""),
          "error"
        );
      } else if (payload && payload.code === "STARTED") {
        showNotice("已开始下载，完成后会自动校验", "ok");
      }
    };
    window.xiaotuUpdateCancelDone = (payload) => {
      render((payload && payload.status) || {});
      if (payload && payload.code === "CANCELED") {
        showNotice("已取消本次后台下载", "ok");
      }
    };
    window.xiaotuUpdateResumeDone = (payload) => {
      render((payload && payload.status) || {});
      if (payload && payload.ok === false) {
        showNotice("恢复失败：" + (payload.code || ""), "error");
      } else {
        showNotice("已继续使用当前版本，自动更新已恢复", "ok");
      }
    };
    window.xiaotuUpdateRollbackDone = (payload) => {
      if (payload && payload.ok === false) {
        showNotice(payload.message || "回退未能开始：" + (payload.code || ""), "error");
        nativeBridge.xiaotuUpdateStatus.postMessage({});
      } else if (payload && payload.code === "ROLLING_BACK") {
        showNotice(payload.message || "正在退出并回退到上一版本…", "ok");
      }
    };
    window.xiaotuUpdateOpenBackupFolderDone = (payload) => {
      if (payload && payload.ok === false) {
        showNotice(payload.message || "无法打开备份目录", "error");
      }
    };
    nativeBridge.xiaotuUpdateStatus.postMessage({});
  }

  async function renderUpdateCard(mount) {
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
      // 强制升级：服务端 min_supported_version 或清单 mandatory 都会置位。
      const mandatoryReason = mandatoryReasonText(data.mandatory_reason || "");
      const mandatory =
        data.update_available && data.mandatory
          ? `<div class="notice error" style="margin:0 0 10px">
               <strong>必须升级</strong>：${
                 mandatoryReason ? escapeHtml(mandatoryReason) + "，" : ""
               }当前版本需升级到 ${escapeHtml(
                 latest ? latest.version : "新版本"
               )} 后才能继续使用，此更新不能跳过。
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
  async function restorePage(mount) {
    const canRestore = can("*");
    mount.innerHTML = `
      <div class="page-head">
        <div>
          <h2>备份与恢复</h2>
          <p class="page-sub">从备份目录选择一份恢复；恢复前会自动生成安全备份，失败会回滚</p>
        </div>
      </div>
      <div class="card" data-role="result"></div>
      <div class="card" style="margin-top:12px">
        <div class="page-head" style="margin-bottom:8px">
          <div><h3 style="margin:0">备份文件</h3></div>
          <button class="btn" type="button" data-role="reload">刷新</button>
        </div>
        <div data-role="list">正在读取…</div>
      </div>
      <div class="card" data-role="precheck" style="margin-top:12px;display:none"></div>`;

    const resultCard = mount.querySelector('[data-role="result"]');
    const listBox = mount.querySelector('[data-role="list"]');
    const precheckCard = mount.querySelector('[data-role="precheck"]');

    async function loadResult() {
      try {
        const data = await api("GET", "/api/restore/result");
        const result = data.result;
        if (!result) {
          resultCard.innerHTML = `<p class="page-sub" style="margin:0">暂无恢复记录。</p>`;
          return;
        }
        const ok = result.ok;
        const text = ok
          ? `上次恢复成功：${result.file}（schema v${result.schema_version}）` +
            (result.safety_backup ? `，恢复前安全备份 ${result.safety_backup}` : "")
          : `上次恢复失败：${result.error_message || result.error_code || "未知原因"}`;
        resultCard.innerHTML = `
          <p style="margin:0;color:${ok ? "inherit" : "#b42318"}">${escapeHtml(text)}</p>
          <p class="page-sub" style="margin:4px 0 0">${escapeHtml(result.at || "")}</p>
          ${canRestore ? '<button class="btn" type="button" data-role="clear" style="margin-top:8px">清除记录</button>' : ""}`;
        const clear = resultCard.querySelector('[data-role="clear"]');
        if (clear) {
          clear.addEventListener("click", async () => {
            try {
              await api("POST", "/api/restore/result/clear", {});
              await loadResult();
              showNotice("恢复记录已清除", "ok");
            } catch (error) {
              showNotice(error.message, "error");
            }
          });
        }
      } catch (error) {
        resultCard.innerHTML = `<p class="empty">${escapeHtml(error.message)}</p>`;
      }
    }

    function renderPrecheck(inspection) {
      precheckCard.style.display = "";
      const rows = [
        ["备份文件", inspection.file],
        ["修改时间", inspection.modified_at || "—"],
        ["大小", formatBytes(inspection.bytes)],
        ["schema", inspection.schema_version ? `v${inspection.schema_version}` : "—"],
        ["当前程序 schema", `v${inspection.current_schema_version}`],
        ["完整性检查", inspection.integrity_ok ? "通过" : "未通过"],
        ["本程序数据库", inspection.is_app_database ? "是" : "否"],
        ["可恢复", inspection.ok ? "是" : "否"],
      ];
      if (inspection.will_migrate) {
        rows.push(["恢复后", "启动时会自动迁移到当前 schema"]);
      }
      precheckCard.innerHTML = `
        <h3 style="margin-top:0">预检查结果</h3>
        <table class="table"><tbody>${rows
          .map(
            ([label, value]) =>
              `<tr><th style="width: 140px">${escapeHtml(label)}</th><td>${escapeHtml(
                value
              )}</td></tr>`
          )
          .join("")}</tbody></table>
        ${
          inspection.ok
            ? ""
            : `<p class="page-sub" style="color:#b42318">${escapeHtml(
                (inspection.error && inspection.error.message) || "该备份不可用于恢复"
              )}</p>`
        }
        ${
          inspection.ok && canRestore
            ? '<button class="btn primary" type="button" data-role="apply" style="margin-top:12px">恢复到该备份</button>'
            : ""
        }`;
      const apply = precheckCard.querySelector('[data-role="apply"]');
      if (apply) {
        apply.addEventListener("click", async () => {
          const confirmed = await askConfirm(
            `将用 ${inspection.file} 覆盖当前数据库，程序随后退出并在下次启动时完成恢复。` +
              (inspection.will_migrate ? "该备份 schema 较旧，恢复后会自动迁移。" : "") +
              "是否继续？",
            { okText: "确认恢复", cancelText: "取消" }
          );
          if (!confirmed) return;
          apply.disabled = true;
          try {
            await api("POST", "/api/restore/apply", { file: inspection.file, confirm: true });
            showNotice("已进入维护状态，程序即将退出；重新打开后将完成恢复", "ok");
            precheckCard.innerHTML =
              '<p style="margin:0">恢复已安排，程序正在退出。请重新启动程序，完成后会在本页显示结果。</p>';
          } catch (error) {
            apply.disabled = false;
            if (error.network) {
              showNotice("程序可能已退出，请重新启动后在顶部查看恢复结果", "ok");
            } else {
              showNotice(error.message, "error");
            }
          }
        });
      }
    }

    async function loadList() {
      listBox.textContent = "正在读取…";
      try {
        const data = await api("GET", "/api/restore/backups");
        if (!data.rows.length) {
          listBox.innerHTML = `<p class="empty">备份目录内没有可恢复的 .db 文件</p>`;
          return;
        }
        listBox.innerHTML = `
          <table class="table">
            <thead><tr><th>文件</th><th>修改时间</th><th>大小</th><th>schema</th><th class="actions">操作</th></tr></thead>
            <tbody>${data.rows
              .map(
                (row) => `<tr>
                  <td>${escapeHtml(row.file)}</td>
                  <td>${escapeHtml(row.modified_at || "—")}</td>
                  <td>${formatBytes(row.bytes)}</td>
                  <td>${row.schema_version ? "v" + row.schema_version : "—"}</td>
                  <td class="actions"><button class="btn" type="button" data-file="${escapeHtml(
                    row.file
                  )}">预检查</button></td>
                </tr>`
              )
              .join("")}</tbody>
          </table>`;
        listBox.querySelectorAll("button[data-file]").forEach((button) => {
          button.addEventListener("click", async () => {
            try {
              const inspection = await api("POST", "/api/restore/precheck", {
                file: button.dataset.file,
              });
              renderPrecheck(inspection);
            } catch (error) {
              showNotice(error.message, "error");
            }
          });
        });
      } catch (error) {
        listBox.innerHTML = `<p class="empty">${escapeHtml(error.message)}</p>`;
      }
    }

    mount.querySelector('[data-role="reload"]').addEventListener("click", loadList);
    await Promise.all([loadResult(), loadList()]);
  }

  async function aboutPage(mount) {
    mount.innerHTML = `
      <div class="page-head">
        <div>
          <img src="/icons/about-512.png" alt="小兔进销存" width="64" height="64"
               style="display:block;margin-bottom:10px;border-radius:15px" />
          <h2>关于与诊断</h2>
          <p class="page-sub">只读信息，可直接复制贴给维护人员；不包含口令、密钥或会话令牌</p>
        </div>
        <button class="btn" type="button" data-role="copy">复制诊断信息</button>
      </div>
      <div class="card" data-role="body">正在读取…</div>
      <div class="card" data-role="update" style="margin-top:12px">正在读取更新状态…</div>`;

    const body = mount.querySelector('[data-role="body"]');
    let diagnosticText = "";
    try {
      const data = await api("GET", "/api/diagnostics");
      const mode = data.mode === "local" ? "单机版" : "网络版";
      const edition = data.edition || {};
      const build = data.build || {};
      const rows = [
        ["应用", `${data.app} ${data.version}`],
        ["运行形态", `${mode}（${edition.label || edition.code || "—"} / ${edition.code || "—"}）`],
        ["schema", `v${data.schema_version}，迁移 ${data.migrations} 条`],
        ["构建", `${build.profile || "—"} · ${build.compiler || "—"}`],
        ["构建时间", `${build.date || "—"} ${build.time || "—"}`],
        ["数据目录", data.data_dir || "—"],
        ["数据库文件", `${data.database_path || "—"}（${formatBytes(data.database_bytes)}）`],
        ["备份目录", data.backup_dir || "—"],
        ["监听", data.listen || "—"],
        ["当前用户", `${data.user || "—"}（${data.role || "—"}）`],
      ];
      body.innerHTML = `<table class="table"><tbody>${rows
        .map(
          ([label, value]) =>
            `<tr><th style="width: 120px">${escapeHtml(label)}</th><td>${escapeHtml(
              value
            )}</td></tr>`
        )
        .join("")}</tbody></table>`;
      diagnosticText = rows.map(([label, value]) => `${label}：${value}`).join("\n");
    } catch (error) {
      body.innerHTML = `<p class="empty">${escapeHtml(error.message)}</p>`;
    }

    mount.querySelector('[data-role="copy"]').addEventListener("click", async () => {
      if (!diagnosticText) {
        showNotice("诊断信息尚未读取成功", "error");
        return;
      }
      try {
        await navigator.clipboard.writeText(diagnosticText);
      } catch (error) {
        const area = document.createElement("textarea");
        area.value = diagnosticText;
        document.body.appendChild(area);
        area.select();
        document.execCommand("copy");
        area.remove();
      }
      showNotice("诊断信息已复制", "ok");
    });

    await renderUpdateCard(mount);
  }

  // ---------- 导航与启动 ----------

  // 二级导航：组只影响展示，页面 key 与权限判定不变。
  // 报表的归属还没定，先作为顶层入口，不放进任何组。
  const NAV_GROUPS = [
    { key: "purchase", title: "采购" },
    { key: "sales", title: "销售" },
    { key: "stock", title: "库存" },
    { key: "finance", title: "财务" },
    { key: "master", title: "基础资料" },
    { key: "system", title: "系统" },
  ];
  // 一级顺序：组与顶层页面可以交错；没有列出的组或页面按原顺序补在后面。
  const NAV_LAYOUT = [
    { type: "group", key: "purchase" },
    { type: "group", key: "sales" },
    { type: "group", key: "stock" },
    { type: "group", key: "finance" },
    { type: "page", key: "dashboard" },
    { type: "page", key: "sales-report" },
    { type: "page", key: "reports" },
    { type: "group", key: "master" },
    { type: "group", key: "system" },
  ];

  function buildNav(all) {
    const groups = new Map(
      NAV_GROUPS.map((group) => [group.key, { ...group, pages: [] }])
    );
    const direct = new Map();
    for (const page of all) {
      const group = page.group ? groups.get(page.group) : null;
      if (group) {
        group.pages.push(page);
      } else {
        direct.set(page.key, page);
      }
    }

    const entries = [];
    const usedGroups = new Set();
    const usedPages = new Set();
    for (const item of NAV_LAYOUT) {
      if (item.type === "group") {
        const group = groups.get(item.key);
        if (group && group.pages.length) {
          entries.push({ type: "group", group });
          usedGroups.add(group.key);
        }
      } else if (direct.has(item.key)) {
        entries.push({ type: "page", page: direct.get(item.key) });
        usedPages.add(item.key);
      }
    }
    for (const group of groups.values()) {
      if (group.pages.length && !usedGroups.has(group.key)) {
        entries.push({ type: "group", group });
      }
    }
    for (const [key, page] of direct) {
      if (!usedPages.has(key)) entries.push({ type: "page", page });
    }
    return entries;
  }

  function navHtml(entries) {
    return entries
      .map((entry) => {
        if (entry.type === "page") {
          return `<button type="button" class="nav-top" data-key="${entry.page.key}">${escapeHtml(
            entry.page.title
          )}</button>`;
        }
        const items = entry.group.pages
          .map(
            (page) =>
              `<button type="button" data-key="${page.key}" data-group="${entry.group.key}">${escapeHtml(
                page.title
              )}</button>`
          )
          .join("");
        return `<div class="nav-group" data-group="${entry.group.key}">
            <button type="button" class="nav-group-title" data-group-toggle="${entry.group.key}" aria-expanded="false">
              <span>${escapeHtml(entry.group.title)}</span><span class="nav-chevron">›</span>
            </button>
            <div class="nav-group-items">${items}</div>
          </div>`;
      })
      .join("");
  }

  const FIXED_PAGES = [
    {
      key: "purchase",
      title: "采购入库",
      group: "purchase",
      render: (mount) =>
        documentPage(mount, {
          docType: "PUR_IN",
          title: "采购入库",
          subtitle: "保存即审核：同一事务内增加库存并写入库存流水",
          partnerLabel: "供应商",
          partnerPlaceholder: "可选",
          partnerApi: "/api/suppliers",
          docLabel: "入库",
        }),
    },
    {
      key: "purchase-return",
      title: "采购退货",
      group: "purchase",
      render: (mount) =>
        documentPage(mount, {
          docType: "PUR_RET",
          title: "采购退货",
          subtitle: "保存即审核：按退货单价冲减库存并写入库存流水",
          partnerLabel: "供应商",
          partnerPlaceholder: "可选",
          partnerApi: "/api/suppliers",
          docLabel: "采购退货",
        }),
    },
    {
      key: "purchase-orders",
      title: "采购订单",
      group: "purchase",
      render: (mount) =>
        orderPage(mount, {
          docType: "PUR_ORDER",
          title: "采购订单",
          subtitle: "只做需求确认与入库来源，不占库存、不写流水、不产生往来",
          partnerLabel: "供应商",
          partnerApi: "/api/suppliers",
          warehouseLabel: "预计入库仓",
          convertTitle: "入库",
          convertWarehouseLabel: "入库仓库",
          convertDateLabel: "入库日期",
          convertAction: "入库",
          convertSubmit: "确认入库",
          convertNotice: "已生成采购入库单：",
          convertedLabel: "已全部入库",
          partialLabel: "部分入库",
          emptyLabel: "还没有采购订单",
        }),
    },
    {
      key: "sales",
      title: "销售出库",
      group: "sales",
      render: (mount) =>
        documentPage(mount, {
          docType: "SAL_OUT",
          title: "销售出库",
          subtitle: "保存即审核：同一事务内扣减库存并写入库存流水",
          partnerLabel: "客户",
          partnerPlaceholder: "可选",
          partnerApi: "/api/customers",
          docLabel: "出库",
        }),
    },
    {
      key: "sales-return",
      title: "销售退货",
      group: "sales",
      render: (mount) =>
        documentPage(mount, {
          docType: "SAL_RET",
          title: "销售退货",
          subtitle: "保存即审核：按成本单价增加库存并写入库存流水",
          partnerLabel: "客户",
          partnerPlaceholder: "可选",
          partnerApi: "/api/customers",
          docLabel: "销售退货",
        }),
    },
    {
      key: "other-in",
      title: "其他入库",
      group: "stock",
      render: (mount) =>
        documentPage(mount, {
          docType: "OTH_IN",
          title: "其他入库",
          subtitle: "保存即审核：按录入单价增加库存，不产生应收应付",
          partnerLabel: "备注",
          partnerPlaceholder: "可选",
          docLabel: "入库",
        }),
    },
    {
      key: "other-out",
      title: "其他出库",
      group: "stock",
      render: (mount) =>
        documentPage(mount, {
          docType: "OTH_OUT",
          title: "其他出库",
          subtitle: "保存即审核：按当前移动加权平均结转成本，不产生应收应付",
          partnerLabel: "备注",
          partnerPlaceholder: "可选",
          docLabel: "出库",
        }),
    },
    {
      key: "sales-orders",
      title: "销售订单",
      group: "sales",
      render: (mount) => orderPage(mount),
    },
    {
      key: "transfer",
      title: "调拨",
      group: "stock",
      render: (mount) =>
        documentPage(mount, {
          docType: "STK_TRF",
          title: "库存调拨",
          subtitle: "保存即审核：源仓出库与目标仓入库在同一事务内完成",
          partnerLabel: "备注",
          partnerPlaceholder: "可选",
          docLabel: "调拨",
          twoWarehouse: true,
        }),
    },
    {
      key: "stocktake",
      title: "盘点",
      group: "stock",
      render: (mount) =>
        documentPage(mount, {
          docType: "STK_CNT",
          title: "库存盘点",
          subtitle: "保存即审核：明细数量填实盘数，按与账面的差额生成调整流水",
          partnerLabel: "备注",
          partnerPlaceholder: "可选",
          docLabel: "盘点",
        }),
    },
    {
      key: "cost-adjust",
      title: "成本调整",
      group: "finance",
      render: (mount) =>
        documentPage(mount, {
          docType: "CST_ADJ",
          title: "成本调整单",
          subtitle: "只调整结存金额、不改数量；仓库与原因逐行填写，审核权限仅管理员",
          partnerLabel: "备注",
          partnerPlaceholder: "（不适用）",
          docLabel: "成本调整",
          costAdjust: true,
        }),
    },
    {
      key: "ar-ap",
      title: "收付款",
      group: "finance",
      render: financePage,
    },
    { key: "statement", title: "对账单", group: "finance", render: statementPage },
    {
      key: "item-categories",
      title: "商品分类",
      group: "master",
      permission: "item.read",
      render: itemCategoryPage,
    },
    {
      key: "openings",
      title: "期初导入",
      group: "master",
      permission: "opening.write",
      render: openingPage,
    },
    { key: "stock", title: "库存", group: "stock", render: stockPage },
    { key: "print-templates", title: "打印模板", group: "system", render: printTemplatePage },
    { key: "reports", title: "报表", group: "", render: reportPage },
    {
      key: "dashboard",
      title: "经营看板",
      group: "",
      permission: "finance.read",
      capability: "report.dashboard",
      render: dashboardPage,
    },
    {
      key: "sales-report",
      title: "销售报表",
      group: "",
      permission: "finance.read",
      render: salesReportPage,
    },
    { key: "users", title: "用户", group: "system", permission: "user.read", render: userPage },
    {
      key: "audit",
      title: "审计日志",
      group: "system",
      permission: "audit.read",
      render: auditPage,
    },
    {
      key: "settings",
      title: "系统参数",
      group: "system",
      permission: "setting.read",
      render: settingPage,
    },
    {
      key: "restore",
      title: "备份与恢复",
      group: "system",
      permission: "setting.read",
      render: restorePage,
    },
    { key: "about", title: "关于与诊断", group: "system", render: aboutPage },
  ];

  // 导航状态：手风琴只记住一个展开组；页面 key 供下次打开回到原页。
  function readNavState(key) {
    try {
      return localStorage.getItem(key) || "";
    } catch (error) {
      return "";
    }
  }

  function writeNavState(key, value) {
    try {
      if (value) localStorage.setItem(key, value);
      else localStorage.removeItem(key);
    } catch (error) {
      // 隐私模式等禁用存储时静默降级，导航仍然可用。
    }
  }

  function expandGroup(key) {
    for (const group of nav.querySelectorAll(".nav-group")) {
      const open = key !== "" && group.dataset.group === key;
      group.classList.toggle("open", open);
      const toggle = group.querySelector("[data-group-toggle]");
      if (toggle) toggle.setAttribute("aria-expanded", open ? "true" : "false");
    }
  }

  // 页面路由：#/<key>。key 与 nav 的 data-key 一致；只排除空白与斜杠这类
  // 结构性非法字符，点号、中文等交给 encodeURIComponent/decodeURIComponent。
  function hashPageKey() {
    const hash = location.hash || "";
    if (!hash.startsWith("#/")) return "";
    const raw = hash.slice(2);
    if (!raw) return "";
    let key = raw;
    try {
      key = decodeURIComponent(raw);
    } catch (error) {
      return "";
    }
    if (/[\s/]/.test(key)) return "";
    return key;
  }

  function replaceHash(key) {
    const target = key
      ? "#/" + encodeURIComponent(key)
      : location.pathname + location.search;
    history.replaceState(null, "", target);
  }

  let routeToken = 0;
  // 页面挂载序号：切页只做一次原子替换，旧页在新页首屏就绪前保持不动。
  let mountSeq = 0;

  async function handleHashChange() {
    const pages = state.allPages;
    if (!pages.length) return;
    const token = ++routeToken;
    const page = pages.find((item) => item.key === hashPageKey());
    if (page && page.key === state.current) return;
    const leave = await confirmLeavePage();
    // 等待确认期间又有新的跳转，交给最新一次处理。
    if (token !== routeToken) return;
    if (!leave) {
      // 返回/前进已经改了 hash，取消时把地址还原到当前页。
      replaceHash(state.current);
      return;
    }
    if (!page) {
      const fallback = pages[0];
      replaceHash(fallback.key);
      if (fallback.key !== state.current) mountPage(fallback);
      return;
    }
    mountPage(page);
  }

  async function goToPage(page) {
    if (page.key === state.current) return;
    if (!(await confirmLeavePage())) return;
    const target = "#/" + encodeURIComponent(page.key);
    if (location.hash === target) return;
    // 用 pushState 改地址再手动路由：直接赋值 location.hash 在 Safari 里会被当成
    // 一次文档导航，整页重新加载（服务端表现为 GET / + 全套静态资源），侧栏跟着
    // 一起重建，看起来就是「点菜单整页抖一下」。pushState 不触发任何浏览器导航。
    history.pushState(null, "", target);
    handleHashChange();
  }

  function mountPage(page, options) {
    // 离开旧页后重新注册：列表页、报表页不设防，只有录入页会自己再注册。
    setUnsavedGuard(null);
    const persist = !options || options.persist !== false;
    state.current = page.key;
    if (page.group) expandGroup(page.group);
    for (const link of nav.querySelectorAll("button[data-key]")) {
      link.classList.toggle("active", link.dataset.key === page.key);
    }
    for (const group of nav.querySelectorAll(".nav-group")) {
      group.classList.toggle("active", group.dataset.group === page.group);
    }
    if (persist) {
      writeNavState("xiaotu.nav.page", page.key);
      writeNavState("xiaotu.nav.group", page.group || "");
    }
    // 切页过程中不动已经显示的内容：新页先渲染到脱离文档的容器里，等它的
    // render（各页都会 await 首次数据）落地后，再一次性换掉旧页。这样
    // main 的 scrollHeight 与滚动条不会经历「清空 → 空白 → 填回」的中间态，
    // main 也不会在切页中途被清空一次。期间旧页保持可交互，用户再点别的菜单
    // 会叠加新的序号，旧的那次替换自动作废。
    const seq = ++mountSeq;
    const container = document.createElement("section");
    container.className = "page";
    let swapped = false;
    let fallbackTimer = 0;
    const swap = () => {
      if (swapped) return;
      swapped = true;
      if (fallbackTimer) clearTimeout(fallbackTimer);
      // 期间已经切到别的页，这次结果直接丢弃。
      if (seq !== mountSeq) return;
      main.querySelectorAll(":scope > *").forEach((node) => {
        if (node !== notice && node.id !== "data-warning" && node.id !== "network-warning") {
          node.remove();
        }
      });
      showNotice("");
      main.appendChild(container);
      main.scrollTop = 0;
    };
    // 兜底：某页渲染卡住时不能让旧页一直留在屏幕上。
    fallbackTimer = setTimeout(swap, 1500);
    Promise.resolve(page.render(container, { crudPage, itemPage, money, escapeHtml, can }))
      .then(swap)
      .catch((error) => {
        showNotice(error.message, "error");
        swap();
      });
  }

  // 门禁页出现后，启动链接里的 #t= 一次性 token 已不再使用，清掉避免刷新时误判。
  function clearBootstrapHash() {
    if (location.hash) {
      history.replaceState(null, "", location.pathname + location.search);
    }
  }

  // 单机版本机口令门禁：只收口令，内部仍按内置 admin 登录；不带用户名输入框。
  // 登录成功后按「30 天免输 / 每次都要输」决定是否把服务端签发的凭据写进钥匙串。
  async function localPasswordLogin(password) {
    const gate = state.localGate || {};
    const remember = gate.remember_enabled !== false && keychainAvailable();
    const result = await api("POST", "/api/login", {
      username: "admin",
      password,
      remember,
    });
    state.csrf = result.csrf || "";
    if (result.remember_token) {
      await saveRememberToken(result.remember_token);
    } else {
      await clearRememberToken();
    }
    return result;
  }

  // 钥匙串里有凭据就先换会话；过期、被清除或时钟回拨时清掉本地凭据并回口令页。
  async function tryRememberedLogin() {
    if (!keychainAvailable()) return { ok: false, message: "" };
    const stored = await keychainCall("get");
    const token = stored && stored.ok ? stored.token : "";
    if (!token) return { ok: false, message: "" };
    try {
      const result = await api("POST", "/api/local/remember", { token });
      state.csrf = result.csrf || "";
      return { ok: true };
    } catch (error) {
      if (error.status === 401 || error.code === "REMEMBER_TOKEN_INVALID") {
        await clearRememberToken();
        return { ok: false, message: error.message || "本机免输已失效，请重新输入口令" };
      }
      return { ok: false, message: "" };
    }
  }

  function renderLocalGate(message) {
    state.current = null;
    nav.innerHTML = "";
    document.getElementById("mode-label").textContent = "单机版";
    document.getElementById("health").textContent = "";
    notice.hidden = true;
    main.querySelectorAll(":scope > *").forEach((node) => {
      if (node !== notice && node.id !== "data-warning" && node.id !== "network-warning") {
        node.remove();
      }
    });
    const card = document.createElement("div");
    card.className = "login-card";
    card.innerHTML = `
        <h2>小兔进销存</h2>
        <p class="hint">${escapeHtml(message || "请输入本机口令")}</p>
        <form data-role="local-gate-form">
          <label>本机口令<input name="password" type="password" autocomplete="current-password" required /></label>
          <button class="btn" type="submit">进入</button>
        </form>`;
    main.appendChild(card);
    updateNetworkBanner();
    const form = card.querySelector("form");
    form.addEventListener("submit", async (event) => {
      event.preventDefault();
      try {
        await localPasswordLogin(form.password.value);
        clearBootstrapHash();
        await bootstrap();
      } catch (error) {
        showNotice(error.message, "error");
      }
    });
  }

  // 老库第一次升级：已有数据但没有本机口令，问一次；跳过就保持 token 自动登录。
  function renderLocalPasswordSetup() {
    state.current = null;
    nav.innerHTML = "";
    document.getElementById("mode-label").textContent = "单机版";
    document.getElementById("health").textContent = "";
    notice.hidden = true;
    main.querySelectorAll(":scope > *").forEach((node) => {
      if (node !== notice && node.id !== "data-warning" && node.id !== "network-warning") {
        node.remove();
      }
    });
    const card = document.createElement("div");
    card.className = "login-card";
    card.innerHTML = `
        <h2>设置本机口令</h2>
        <p class="hint">这台机器已有数据。设置口令后，下次启动需要先输入口令才能进入；也可以跳过，跳过则保持双击直接进入。</p>
        <form data-role="local-password-setup">
          <label>口令<input name="password" type="password" autocomplete="new-password" minlength="8" required /></label>
          <label>确认口令<input name="confirm" type="password" autocomplete="new-password" minlength="8" required /></label>
          <p class="hint">口令长度 8–128 个字符</p>
          <div class="form-actions">
            <button class="btn primary" type="submit">设置口令</button>
            <button class="btn" type="button" data-role="skip">跳过</button>
          </div>
        </form>`;
    main.appendChild(card);
    updateNetworkBanner();
    const form = card.querySelector("form");
    form.addEventListener("submit", async (event) => {
      event.preventDefault();
      if (form.password.value !== form.confirm.value) {
        showNotice("两次输入的口令不一致", "error");
        return;
      }
      try {
        await api("POST", "/api/local/password", { password: form.password.value });
        await localPasswordLogin(form.password.value);
        clearBootstrapHash();
        await bootstrap();
      } catch (error) {
        showNotice(error.message, "error");
      }
    });
    card.querySelector('[data-role="skip"]').addEventListener("click", async () => {
      try {
        await api("POST", "/api/local/password/skip");
        await bootstrap();
      } catch (error) {
        showNotice(error.message, "error");
      }
    });
  }

  function renderLogin(message) {
    state.current = null;
    nav.innerHTML = "";
    document.getElementById("mode-label").textContent = "请登录";
    document.getElementById("health").textContent = "";
    notice.hidden = true;
    // 保留 #notice 与两条顶部提示，避免重新登录后 showNotice 写到已脱离文档的节点上。
    main.querySelectorAll(":scope > *").forEach((node) => {
      if (node !== notice && node.id !== "data-warning" && node.id !== "network-warning") {
        node.remove();
      }
    });
    const card = document.createElement("div");
    card.className = "login-card";
    card.innerHTML = `
        <h2>登录 小兔进销存</h2>
        <p class="hint">${escapeHtml(message || "请输入用户名与口令")}</p>
        <form data-role="login-form">
          <label>用户名<input name="username" autocomplete="username" required /></label>
          <label>口令<input name="password" type="password" autocomplete="current-password" required /></label>
          <button class="btn" type="submit">登录</button>
        </form>`;
    main.appendChild(card);
    updateNetworkBanner();
    const form = card.querySelector("form");
    form.addEventListener("submit", async (event) => {
      event.preventDefault();
      try {
        const result = await api("POST", "/api/login", {
          username: form.username.value.trim(),
          password: form.password.value,
        });
        state.csrf = result.csrf || "";
        if (result.must_change_password) {
          renderChangePassword("管理员已重置口令，请设置新口令");
          return;
        }
        await bootstrap();
      } catch (error) {
        showNotice(error.message, "error");
      }
    });
  }

  // 强制改密：登录后才进入；除改密与退出外的接口都会被服务端拒绝。
  function renderChangePassword(message) {
    state.current = null;
    nav.innerHTML = "";
    document.getElementById("mode-label").textContent = "请修改口令";
    document.getElementById("health").textContent = "";
    notice.hidden = true;
    main.querySelectorAll(":scope > *").forEach((node) => {
      if (node !== notice && node.id !== "data-warning" && node.id !== "network-warning") {
        node.remove();
      }
    });
    const card = document.createElement("div");
    card.className = "login-card";
    card.innerHTML = `
        <h2>修改口令</h2>
        <p class="hint">${escapeHtml(message || "首次登录或口令被重置，请先修改口令")}</p>
        <form data-role="change-password-form">
          <label>原口令<input name="old_password" type="password" autocomplete="current-password" required /></label>
          <label>新口令<input name="new_password" type="password" autocomplete="new-password" minlength="8" required /></label>
          <label>确认新口令<input name="confirm_password" type="password" autocomplete="new-password" minlength="8" required /></label>
          <p class="hint">口令长度 8–128 个字符</p>
          <button class="btn" type="submit">保存并继续</button>
        </form>`;
    main.appendChild(card);
    updateNetworkBanner();
    const form = card.querySelector("form");
    form.addEventListener("submit", async (event) => {
      event.preventDefault();
      if (form.new_password.value !== form.confirm_password.value) {
        showNotice("两次输入的新口令不一致", "error");
        return;
      }
      try {
        const result = await api("POST", "/api/password/change", {
          old_password: form.old_password.value,
          new_password: form.new_password.value,
        });
        state.csrf = result.csrf || state.csrf;
        await bootstrap();
      } catch (error) {
        showNotice(error.message, "error");
      }
    });
  }

  async function bootstrap() {
    try {
      // health 免登录，先拿单机版门禁状态；网络版这些字段恒为 false。
      const health = await api("GET", "/api/health");
      state.edition = health.edition || "";
      const isLocal = health.mode === "local";
      const localGate = health.local_gate || {};
      state.mode = health.mode || "";
      state.localGate = localGate;
      document.getElementById("mode-label").textContent = isLocal ? "单机版" : "网络版";
      document.getElementById("health").textContent =
        "v" + health.version + " · schema v" + health.schema_version;

      let me = null;
      try {
        me = await api("GET", "/api/me");
      } catch (error) {
        if (error.status !== 401) throw error;
      }
      if (!me) {
        // 已设本机口令：先试钥匙串里的长期凭据，失败再出门禁页；
        // 启动链接里的一次性 token 不参与自动登录。
        if (isLocal && localGate.password_required) {
          clearBootstrapHash();
          const remembered = await tryRememberedLogin();
          if (!remembered.ok) {
            renderLocalGate(remembered.message);
            return;
          }
          me = await api("GET", "/api/me");
        } else if (isLocal && localGate.prompt_set_password) {
          // 有数据但从未设过口令：问一次；新装无数据不打扰。
          renderLocalPasswordSetup();
          return;
        } else {
          const hash = location.hash || "";
          const match = /(?:^#|&)t=([0-9a-f]+)/.exec(hash);
          if (match) {
            try {
              const session = await api("POST", "/api/session", { token: match[1] });
              state.csrf = session.csrf || "";
            } catch (error) {
              // health 读取后口令被另一个页面设置：同样回到门禁页。
              if (error.code === "LOCAL_PASSWORD_REQUIRED") {
                clearBootstrapHash();
                renderLocalGate();
                return;
              }
              throw error;
            }
            clearBootstrapHash();
            me = await api("GET", "/api/me");
          } else {
            renderLogin("请使用用户名与口令登录");
            return;
          }
        }
      }
      state.csrf = me.csrf || state.csrf;
      state.permissions = me.permissions || [];
      if (me.must_change_password) {
        renderChangePassword("管理员已重置口令，请设置新口令");
        return;
      }

      // 强制升级拦截：壳内主动取一次状态；浏览器里没有桥，静默跳过。
      requestNativeUpdateStatus(blockMandatoryUpdate);

      // 数据目录位于同步盘时由服务端给出提示，登录后常驻在页面顶部。
      let warningNode = document.getElementById("data-warning");
      if (!warningNode) {
        warningNode = document.createElement("div");
        warningNode.id = "data-warning";
        warningNode.className = "notice warn";
        main.prepend(warningNode);
      }
      warningNode.textContent = health.data_dir_warning || "";
      warningNode.hidden = !health.data_dir_warning;

      const pages = await (await fetch(API_BASE + "/gen/pages.json")).json();
      state.pages = pages;
      for (const page of pages) {
        await loadScript(page.script);
      }

      const all = [
        ...pages.map((page) => ({
          key: page.key,
          title: page.title,
          group: page.group || "",
          permission: page.permission || "",
          render: (mount, ui) => window.XiaotuPages[page.key].render(mount, ui),
        })),
        ...FIXED_PAGES,
      ].filter(
        (page) =>
          (!page.permission || can(page.permission)) &&
          (!page.capability || editionAllows(page.capability))
      );

      nav.innerHTML = navHtml(buildNav(all));
      nav.addEventListener("click", (event) => {
        const toggle = event.target.closest("[data-group-toggle]");
        if (toggle) {
          const group = toggle.closest(".nav-group");
          const key = group.classList.contains("open") ? "" : group.dataset.group;
          expandGroup(key);
          writeNavState("xiaotu.nav.group", key);
          return;
        }
        const button = event.target.closest("button[data-key]");
        if (!button) return;
        const page = all.find((item) => item.key === button.dataset.key);
        if (page) goToPage(page);
      });

      // 路由优先：hash > 上次页面 > 默认页；无权或未知 key 回退到默认页。
      state.allPages = all;
      const hashKey = hashPageKey();
      const savedPage = readNavState("xiaotu.nav.page");
      const initial =
        all.find((page) => page.key === hashKey) ||
        all.find((page) => page.key === savedPage) ||
        all[0];
      if (initial) {
        if (hashKey !== initial.key) replaceHash(initial.key);
        mountPage(initial);
      }
    } catch (error) {
      if (error.status === 401) {
        renderLogin("请使用用户名与口令登录");
      } else {
        showNotice("加载失败: " + error.message, "error");
      }
    }
  }

  window.addEventListener("hashchange", handleHashChange);
  // goToPage 改用 pushState 之后，浏览器前进/后退不再触发 hashchange，改由 popstate 驱动。
  window.addEventListener("popstate", handleHashChange);
  // 关闭窗口或刷新时的浏览器原生提示。只对 --server 的普通浏览器有效；
  // 单机版 WKWebView 不执行 beforeunload，关窗不会弹确认，需在文档中注明。
  window.addEventListener("beforeunload", (event) => {
    flushActiveDraft();
    if (!hasUnsaved()) return;
    event.preventDefault();
    event.returnValue = "";
  });
  bootstrap();
})();
