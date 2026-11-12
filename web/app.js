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
  };

  const main = document.getElementById("main");
  const nav = document.getElementById("nav");
  const notice = document.getElementById("notice");

  // 录入页注册「是否有未保存内容」；只有注册过的页面才会拦导航与关闭窗口。
  let unsavedGuard = null;

  function setUnsavedGuard(guard) {
    unsavedGuard = guard;
  }

  function hasUnsaved() {
    return Boolean(unsavedGuard && unsavedGuard());
  }

  // WKWebView 没有实现 WKUIDelegate 的 confirm 面板，window.confirm 会静默返回
  // false；离开确认改用页内确认层，不依赖浏览器原生对话框。
  let confirmDialogOpen = false;

  // options: message / okText / cancelText（传 null 表示只有确认按钮）/ withInput / defaultValue
  function askDialog(options) {
    const okText = options.okText || "确定";
    const cancelText = options.cancelText === undefined ? "取消" : options.cancelText;
    const withInput = Boolean(options.withInput);
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
          close(false);
        } else if (event.key === "Enter") {
          event.preventDefault();
          close(true);
        }
      }
      cancel.addEventListener("click", () => close(false));
      ok.addEventListener("click", () => close(true));
      mask.addEventListener("click", (event) => {
        if (event.target === mask) close(false);
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

  function money(cents) {
    const value = Number(cents || 0);
    return (value / 100).toFixed(2);
  }

  const QTY_SCALE = 10000n;

  // 定点整数（×10000）→ 十进制文本，去掉末尾 0；全程 BigInt，不做浮点换算。
  function qtyText(value) {
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

  // 前端只负责隐藏入口，真正的拦截在各路由的 guard 里。
  function can(permission) {
    return (
      state.permissions.indexOf("*") >= 0 || state.permissions.indexOf(permission) >= 0
    );
  }

  async function api(method, path, body) {
    const options = { method, credentials: "include", headers: {} };
    if (state.csrf && method !== "GET") {
      options.headers["X-CSRF-Token"] = state.csrf;
    }
    if (body !== undefined) {
      options.headers["Content-Type"] = "application/json";
      options.body = JSON.stringify(body);
    }
    const response = await fetch(API_BASE + path, options);
    let payload = null;
    try {
      payload = await response.json();
    } catch (error) {
      payload = null;
    }
    if (!response.ok) {
      const message = payload && payload.message ? payload.message : "HTTP " + response.status;
      const error = new Error(message);
      error.status = response.status;
      error.code = payload && payload.code;
      throw error;
    }
    return payload;
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

  // 报表 / 对账单共用：浏览器走 Blob 下载，单机版 WKWebView 交给原生保存面板。
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

  function printPage() {
    const handlers = window.webkit && window.webkit.messageHandlers;
    if (handlers && handlers.xiaotuPrint) {
      handlers.xiaotuPrint.postMessage("");
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
    if (type === "MONEY") return money(value);
    return String(value);
  }

  function printTotalsText(totals) {
    if (!totals) return "";
    if (totals.total_amount_text !== undefined) {
      return "合计数量 " + totals.total_qty + "，合计金额 " + totals.total_amount_text + " 元";
    }
    if (totals.closing_text !== undefined) {
      return "期初余额 " + totals.opening_text + " 元，期末余额 " + totals.closing_text + " 元";
    }
    return "";
  }

  // 模板只提供展示配置，所有数据都按纯文本转义后拼进打印视图。
  function printViewHtml(data) {
    const tmpl = data.template || {};
    const columns = data.columns || [];
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
      <div class="print-sheet" style="font-size:${Number(tmpl.font_size_pt) || 10}pt">
        ${headerText}
        <h1 class="print-title">${escapeHtml(data.title || "")}</h1>
        <div class="print-subtitle">${escapeHtml(data.subtitle || "")}</div>
        ${fields ? `<div class="print-fields">${fields}</div>` : ""}
        <table class="print-table">
          <colgroup>${colgroup}</colgroup>
          <thead><tr>${head}</tr></thead>
          <tbody>${body}</tbody>
        </table>
        ${totals ? `<div class="print-totals">${escapeHtml(totals)}</div>` : ""}
        <div class="print-meta">打印时间：${escapeHtml(data.printed_at || "")}</div>
        ${footerText}
      </div>`;
  }

  function printPageCss(tmpl) {
    const size =
      tmpl.page_size === "CUSTOM"
        ? `${Number(tmpl.paper_width_mm)}mm ${Number(tmpl.paper_height_mm)}mm`
        : tmpl.page_size || "A4";
    const orientation = tmpl.orientation === "LANDSCAPE" ? " landscape" : " portrait";
    const pageNumber = tmpl.show_page_no
      ? ' @bottom-right { content: "第 " counter(page) " 页"; font-size: 9pt; }'
      : "";
    return `@page { size: ${size}${orientation}; margin: ${Number(tmpl.margin_mm)}mm;${pageNumber} }`;
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
    style.textContent = printPageCss(data.template || {});
    document.head.appendChild(style);
    document.body.classList.add("print-mode");
    return () => {
      document.body.classList.remove("print-mode");
      view.remove();
      style.remove();
    };
  }

  function printWithTemplate(data) {
    const cleanup = mountPrintView(data);
    // 浏览器打印结束（含取消）后清理；单机版桥接不触发时，下次打印会先清掉旧视图。
    window.addEventListener("afterprint", cleanup, { once: true });
    printPage();
  }

  function pdfBridge() {
    const handlers = window.webkit && window.webkit.messageHandlers;
    return handlers && handlers.xiaotuPrintPdf ? handlers.xiaotuPrintPdf : null;
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
      const cleanup = mountPrintView(data);
      window.xiaotuPdfDone = (result) => {
        cleanup();
        delete window.xiaotuPdfDone;
        if (result === "ok") showNotice("PDF 已导出", "ok");
        else if (result === "error") showNotice("PDF 导出失败", "error");
      };
      bridge.postMessage({ filename: pdfFileName(filename || data.title) });
    } catch (error) {
      showNotice(error.message, "error");
    }
  }

  async function printTarget(path) {
    try {
      printWithTemplate(await api("GET", path));
    } catch (error) {
      showNotice(error.message, "error");
    }
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

  // 文本类导出（错误清单等）：单机版交给原生保存面板，浏览器走 Blob。
  function saveText(filename, text) {
    const content = "\uFEFF" + text;
    const handlers = window.webkit && window.webkit.messageHandlers;
    if (handlers && handlers.xiaotuExport) {
      handlers.xiaotuExport.postMessage({ filename, content });
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
      <div class="card">
        <table class="table" data-role="table"></table>
      </div>`;

    const formCard = mount.querySelector('[data-role="form-card"]');
    const form = mount.querySelector('[data-role="form"]');
    const table = mount.querySelector('[data-role="table"]');

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
              <option value="0">（未指定）</option>
              ${options
                .filter((option) => option.status !== false)
                .map(
                  (option) =>
                    `<option value="${option.id}" ${
                      String(option.id) === String(value) ? "selected" : ""
                    }>${escapeHtml(option.code + " " + option.name)}</option>`
                )
                .join("")}
            </select>
          </label>`;
      }
      if (field.type === "int") {
        const min = field.min === undefined ? "" : ` min="${Number(field.min)}"`;
        const max = field.max === undefined ? "" : ` max="${Number(field.max)}"`;
        return `<label class="field">
          <span>${escapeHtml(field.label)}${field.required ? " *" : ""}</span>
          <input type="number" step="1"${min}${max} data-field="${escapeHtml(field.key)}" value="${escapeHtml(
          value === undefined || value === null ? "" : value
        )}" ${field.required ? "required" : ""} />
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
      form.innerHTML = def.fields
        .map((field) =>
          fieldInput(field, row ? row[field.key] : field.default)
        )
        .join("");
      formCard.hidden = false;
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
          payload[field.key] = Number(input.value || 0);
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
      rows = await api("GET", def.api);
      renderTable();
    }

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
        importButton.addEventListener("click", () => {
          fileInput.value = "";
          fileInput.click();
        });
        fileInput.addEventListener("change", async () => {
          const file = fileInput.files && fileInput.files[0];
          if (!file) return;
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
      try {
        if (editingId) {
          await api("PUT", def.api + "/" + editingId, payload);
        } else {
          await api("POST", def.api, payload);
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

  // ---------- 单据页面（销售出库 / 采购入库共用） ----------

  async function documentPage(mount, config) {
    const docType = config.docType;
    const costAdjust = !!config.costAdjust;
    // 只有其他出入库有业务类型；入库可用期初，出库可用领用与报损，赠品两方向通用。
    const otherDoc = docType === "OTH_IN" || docType === "OTH_OUT";
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
    let documents = [];
    let editingId = 0;
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
          </div>
          <table class="table lines" data-role="lines"></table>
          <div class="form-actions">
            <button class="btn" type="button" data-role="add-line">添加明细行</button>
            <button class="btn" type="button" data-role="save-draft">存为草稿</button>
            <button class="btn primary" type="button" data-role="submit">保存并审核</button>
            <button class="btn" type="button" data-role="cancel-edit" hidden>取消编辑</button>
          </div>
        </form>
      </div>
      <div class="card">
        <table class="table" data-role="documents"></table>
      </div>`;

    const form = mount.querySelector('[data-role="form"]');
    const warehouseSelect = mount.querySelector('[data-role="warehouse"]');
    const targetWarehouseSelect = mount.querySelector('[data-role="target-warehouse"]');
    const dateInput = mount.querySelector('[data-role="date"]');
    const linesTable = mount.querySelector('[data-role="lines"]');
    const partnerSelect = mount.querySelector('[data-role="partner-select"]');
    const bizCategorySelect = mount.querySelector('[data-role="biz-category"]');
    const documentsTable = mount.querySelector('[data-role="documents"]');

    dateInput.value = new Date().toISOString().slice(0, 10);
    if (costAdjust && !can("cost.adjust")) {
      mount.querySelector('[data-role="submit"]').hidden = true;
    }

    function itemLabel(item) {
      return [item.code, item.name, item.spec]
        .map((part) => (part || "").trim())
        .filter(Boolean)
        .join(" ");
    }

    function itemOptions(selected) {
      return items
        .map(
          (item) =>
            `<option value="${item.id}" ${String(item.id) === String(selected) ? "selected" : ""}>${escapeHtml(
              itemLabel(item)
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
      linesTable.innerHTML = costAdjust
        ? `<thead><tr><th>商品</th><th>仓库</th><th>调整金额（元）</th><th>调整原因</th><th class="actions">操作</th></tr></thead><tbody></tbody>`
        : `<thead><tr><th>商品</th><th>数量</th><th>单价（元）</th><th>金额（元）</th><th class="actions">操作</th></tr></thead><tbody></tbody>`;
      addLine();
    }

    function statusLabel(status) {
      return { APPROVED: "已审核", DRAFT: "草稿", VOIDED: "已作废" }[status] || status;
    }

    function actionsFor(doc) {
      const print =
        '<button class="link" data-role="print">打印</button>' +
        '<button class="link" data-role="pdf">导出 PDF</button>';
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

    function renderDocuments() {
      if (!documents.length) {
        documentsTable.innerHTML =
          `<tbody><tr><td class="empty">还没有${escapeHtml(config.docLabel)}单</td></tr></tbody>`;
        return;
      }
      documentsTable.innerHTML = `
        <thead><tr><th>单据号</th><th>日期</th><th>仓库</th><th>${escapeHtml(
          config.partnerLabel
        )}</th>${otherDoc ? "<th>业务类型</th>" : ""}<th>数量</th><th>金额（元）</th><th>状态</th><th class="actions">操作</th></tr></thead>
        <tbody>${documents
          .map(
            (doc) => `<tr data-id="${doc.id}">
              <td>${escapeHtml(doc.doc_no)}</td>
              <td>${escapeHtml(doc.doc_date)}</td>
              <td>${escapeHtml(doc.warehouse_name)}</td>
              <td>${escapeHtml(doc.customer || "—")}</td>
              ${
                otherDoc
                  ? `<td>${escapeHtml(bizCategoryText(doc.biz_category) || "—")}</td>`
                  : ""
              }
              <td>${costAdjust ? "—" : qtyText(doc.total_qty)}</td>
              <td>${money(doc.total_amount_cents)}</td>
              <td>${statusLabel(doc.status)}</td>
              <td class="actions">${actionsFor(doc)}</td>
            </tr>`
          )
          .join("")}</tbody>`;
    }

    async function reload() {
      const [warehouseList, itemList, docList, partnerList] = await Promise.all([
        api("GET", "/api/warehouses"),
        api("GET", "/api/items"),
        api("GET", "/api/documents?doc_type=" + docType),
        config.partnerApi ? api("GET", config.partnerApi) : Promise.resolve([]),
      ]);
      warehouses = warehouseList;
      items = itemList;
      documents = docList;
      if (config.partnerApi) {
        partnerSelect.innerHTML =
          '<option value="">（不指定）</option>' +
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
      renderDocuments();
      if (!linesTable.querySelector("tbody") || !linesTable.querySelector("tbody").children.length) {
        resetLines();
      }
    }

    function resetForm() {
      editingId = 0;
      dirty = false;
      mount.querySelector('[data-role="cancel-edit"]').hidden = true;
      form.querySelector('[data-role="partner"]').value = "";
      if (partnerSelect) partnerSelect.value = "";
      if (otherDoc) bizCategorySelect.value = "OTHER";
      dateInput.value = new Date().toISOString().slice(0, 10);
      resetLines();
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
        lines,
      };
      try {
        if (editingId) {
          await api("PUT", "/api/documents/" + editingId, body);
          if (!draft) await api("POST", "/api/documents/" + editingId + "/approve");
          showNotice(draft ? "草稿已保存" : "已保存并审核", "ok");
        } else {
          const result = await api("POST", "/api/documents", {
            ...body,
            save_as_draft: draft,
          });
          showNotice((draft ? "已存为草稿：" : "已保存并审核：") + result.doc_no, "ok");
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
          const doc = documents.find((row) => row.id === id);
          await exportPdfTarget(
            "/api/print/render?target_type=DOC&target_key=" + docType + "&doc_id=" + id,
            doc ? doc.doc_no : undefined
          );
          return;
        }
        if (button.dataset.role === "reverse") {
          if (!(await askConfirm("反审核会生成反向库存流水，确认继续？"))) return;
          await api("POST", "/api/documents/" + id + "/reverse");
          showNotice("已反审核", "ok");
        } else if (button.dataset.role === "approve") {
          if (!(await askConfirm("审核后才会写库存流水，确认继续？"))) return;
          await api("POST", "/api/documents/" + id + "/approve");
          showNotice("已审核", "ok");
        } else if (button.dataset.role === "delete") {
          if (!(await askConfirm("确认删除这张草稿？"))) return;
          await api("DELETE", "/api/documents/" + id);
          if (editingId === id) resetForm();
          showNotice("草稿已删除", "ok");
        } else if (button.dataset.role === "edit") {
          await startEdit(documents.find((doc) => doc.id === id));
          return;
        }
        await reload();
      } catch (error) {
        showNotice(error.message, "error");
      }
    });

    await reload();
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
    let documents = [];
    let editingId = 0;
    let convertingId = 0;
    let page = 1;
    const pageSize = 20;

    mount.innerHTML = `
      <div class="page-head">
        <div>
          <h2>${escapeHtml(config.title)}</h2>
          <p class="page-sub">${escapeHtml(config.subtitle)}</p>
        </div>
        <button class="btn" data-role="refresh">刷新</button>
      </div>
      <div class="card">
        <form class="grid-form" data-role="filters">
          <label class="field"><span>关键词</span>
            <input type="text" data-field="keyword" placeholder="单号 / ${escapeHtml(
              config.partnerLabel
            )}" />
          </label>
          <label class="field"><span>状态</span>
            <select data-field="status">
              <option value="">全部</option>
              <option value="DRAFT">草稿</option>
              <option value="CONFIRMED">已确认</option>
              <option value="PARTIAL">${escapeHtml(config.partialLabel)}</option>
              <option value="CONVERTED">${escapeHtml(config.convertedLabel)}</option>
              <option value="CLOSED">已关闭</option>
            </select>
          </label>
          <div class="form-actions">
            <button class="btn primary" type="submit">查询</button>
            <button class="btn" type="button" data-role="filter-reset">重置</button>
          </div>
        </form>
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
      <div class="card">
        <table class="table" data-role="documents"></table>
        <div class="form-actions">
          <button class="btn" type="button" data-role="prev">上一页</button>
          <span class="page-sub" data-role="page-info"></span>
          <button class="btn" type="button" data-role="next">下一页</button>
        </div>
      </div>`;

    const filters = mount.querySelector('[data-role="filters"]');
    const form = mount.querySelector('[data-role="form"]');
    const warehouseSelect = mount.querySelector('[data-role="warehouse"]');
    const dateInput = mount.querySelector('[data-role="date"]');
    const customerSelect = mount.querySelector('[data-role="customer"]');
    const linesTable = mount.querySelector('[data-role="lines"]');
    const documentsTable = mount.querySelector('[data-role="documents"]');
    const convertCard = mount.querySelector('[data-role="convert-card"]');
    const convertTitle = mount.querySelector('[data-role="convert-title"]');
    const convertWarehouse = mount.querySelector('[data-role="convert-warehouse"]');
    const convertDate = mount.querySelector('[data-role="convert-date"]');
    const convertLines = mount.querySelector('[data-role="convert-lines"]');
    const prevButton = mount.querySelector('[data-role="prev"]');
    const nextButton = mount.querySelector('[data-role="next"]');
    const pageInfo = mount.querySelector('[data-role="page-info"]');
    const keywordField = filters.querySelector('[data-field="keyword"]');
    const statusField = filters.querySelector('[data-field="status"]');

    dateInput.value = new Date().toISOString().slice(0, 10);
    convertDate.value = dateInput.value;

    function itemLabel(item) {
      return [item.code, item.name, item.spec]
        .map((part) => (part || "").trim())
        .filter(Boolean)
        .join(" ");
    }

    function itemLabelById(itemId) {
      const item = items.find((row) => row.id === itemId);
      return item ? itemLabel(item) : String(itemId);
    }

    function itemOptions(selected) {
      return items
        .map(
          (item) =>
            `<option value="${item.id}" ${String(item.id) === String(selected) ? "selected" : ""}>${escapeHtml(
              itemLabel(item)
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

    function filteredDocuments() {
      const keyword = keywordField.value.trim().toLowerCase();
      const status = statusField.value;
      return documents.filter((doc) => {
        if (status && orderState(doc) !== status) return false;
        if (!keyword) return true;
        return (
          doc.doc_no.toLowerCase().includes(keyword) ||
          (doc.customer || "").toLowerCase().includes(keyword)
        );
      });
    }

    function renderDocuments() {
      const rows = filteredDocuments();
      const total = rows.length;
      const pages = Math.max(1, Math.ceil(total / pageSize));
      page = Math.min(Math.max(1, page), pages);
      const start = (page - 1) * pageSize;
      const visible = rows.slice(start, start + pageSize);
      pageInfo.textContent = "第 " + page + " / " + pages + " 页 · 共 " + total + " 条";
      prevButton.disabled = page <= 1;
      nextButton.disabled = page >= pages;
      if (!visible.length) {
        documentsTable.innerHTML = `<tbody><tr><td class="empty">${escapeHtml(
          documents.length ? "没有符合条件的订单" : config.emptyLabel
        )}</td></tr></tbody>`;
        return;
      }
      documentsTable.innerHTML = `
        <thead><tr><th>订单号</th><th>日期</th><th>仓库</th><th>${escapeHtml(
          config.partnerLabel
        )}</th><th>数量</th>
          <th>金额（元）</th><th>状态</th><th class="actions">操作</th></tr></thead>
        <tbody>${visible
          .map(
            (doc) => `<tr data-id="${doc.id}">
              <td>${escapeHtml(doc.doc_no)}</td>
              <td>${escapeHtml(doc.doc_date)}</td>
              <td>${escapeHtml(doc.warehouse_name)}</td>
              <td>${escapeHtml(doc.customer || "—")}</td>
              <td>${qtyText(doc.total_qty)}</td>
              <td>${money(doc.total_amount_cents)}</td>
              <td>${orderStatusLabel(doc)}</td>
              <td class="actions">${actionsFor(doc)}</td>
            </tr>`
          )
          .join("")}</tbody>`;
    }

    async function reload() {
      const [warehouseList, itemList, docList, customerList] = await Promise.all([
        api("GET", "/api/warehouses"),
        api("GET", "/api/items"),
        api("GET", "/api/documents?doc_type=" + docType),
        api("GET", config.partnerApi),
      ]);
      warehouses = warehouseList;
      items = itemList;
      customers = customerList;
      documents = docList;
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
        '<option value="">（不指定）</option>' +
        customers
          .filter((customer) => customer.status)
          .map(
            (customer) =>
              `<option value="${customer.id}" data-name="${escapeHtml(customer.name)}">${escapeHtml(
                customer.code + " " + customer.name
              )}</option>`
          )
          .join("");
      renderDocuments();
      if (!linesTable.querySelector("tbody") || !linesTable.querySelector("tbody").children.length) {
        resetLines();
      }
    }

    function resetForm() {
      editingId = 0;
      mount.querySelector('[data-role="cancel-edit"]').hidden = true;
      customerSelect.value = "";
      dateInput.value = new Date().toISOString().slice(0, 10);
      resetLines();
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
        lines,
      };
      try {
        if (editingId) {
          await api("PUT", "/api/documents/" + editingId, body);
          if (!draft) await api("POST", "/api/documents/" + editingId + "/approve");
          showNotice(draft ? "订单草稿已保存" : "订单已确认", "ok");
        } else {
          const result = await api("POST", "/api/documents", { ...body, save_as_draft: draft });
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
        warehouseSelect.value = String(detail.warehouse_id);
        dateInput.value = detail.doc_date;
        customerSelect.value = detail.partner_id ? String(detail.partner_id) : "";
        linesTable.innerHTML = `<thead><tr><th>商品</th><th>数量</th><th>单价（元）</th><th>金额（元）</th><th class="actions">操作</th></tr></thead><tbody></tbody>`;
        for (const line of detail.lines) addLine(line);
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
              const remaining = BigInt(line.qty) - BigInt(line.converted_qty);
              return `<tr data-line-id="${line.id}" data-price-cents="${line.price_cents}">
                <td>${escapeHtml(itemLabelById(line.item_id))}</td>
                <td>${qtyText(line.qty)}</td>
                <td>${qtyText(line.converted_qty)}</td>
                <td>${qtyText(remaining)}</td>
                <td><input type="text" inputmode="decimal"
                  value="${qtyText(remaining)}" data-role="convert-qty" ${remaining <= 0n ? "disabled" : ""} /></td>
                <td><input type="number" min="0" step="0.01"
                  value="${(line.price_cents / 100).toFixed(2)}" data-role="convert-price" ${
                    remaining <= 0n ? "disabled" : ""
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
    mount.querySelector('[data-role="cancel-edit"]').addEventListener("click", resetForm);
    mount.querySelector('[data-role="save-draft"]').addEventListener("click", () => save(true));
    mount.querySelector('[data-role="submit"]').addEventListener("click", () => save(false));
    mount.querySelector('[data-role="convert-submit"]').addEventListener("click", submitConvert);
    mount.querySelector('[data-role="convert-cancel"]').addEventListener("click", () => {
      convertCard.hidden = true;
      convertingId = 0;
    });
    filters.addEventListener("submit", (event) => {
      event.preventDefault();
      page = 1;
      renderDocuments();
    });
    filters.querySelector('[data-role="filter-reset"]').addEventListener("click", () => {
      keywordField.value = "";
      statusField.value = "";
      page = 1;
      renderDocuments();
    });
    prevButton.addEventListener("click", () => {
      page -= 1;
      renderDocuments();
    });
    nextButton.addEventListener("click", () => {
      page += 1;
      renderDocuments();
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
          const doc = documents.find((row) => row.id === id);
          await exportPdfTarget(
            "/api/print/render?target_type=DOC&target_key=" + docType + "&doc_id=" + id,
            doc ? doc.doc_no : undefined
          );
          return;
        }
        if (button.dataset.role === "approve") {
          if (!(await askConfirm(`确认订单后不可再改单，只能${config.convertTitle}或关闭。继续？`))) return;
          await api("POST", "/api/documents/" + id + "/approve");
          showNotice("订单已确认", "ok");
        } else if (button.dataset.role === "edit") {
          await startEdit(documents.find((doc) => doc.id === id));
          return;
        } else if (button.dataset.role === "delete") {
          if (!(await askConfirm("确认删除这张订单草稿？"))) return;
          await api("DELETE", "/api/documents/" + id);
          if (editingId === id) resetForm();
          showNotice("订单草稿已删除", "ok");
        } else if (button.dataset.role === "convert") {
          await openConvert(documents.find((doc) => doc.id === id));
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
        </div>
        <p class="page-sub">期初 + 累计收入 − 累计支出，截至查询结束日期</p>
        <table class="table" data-role="fund-balances"></table>
      </div>
      <div class="card">
        <div class="card-head">
          <h3>库存余额表</h3>
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
      <div class="card">
        <div class="card-head">
          <h3>销售毛利</h3>
          <button class="btn small" data-role="export-profit">导出 CSV</button>
          <button class="btn small" data-role="excel-profit">导出 Excel</button>
          <button class="btn small" data-role="print-profit">打印</button>
          <button class="btn small" data-role="pdf-profit">导出 PDF</button>
        </div>
        <p class="page-sub">收入取单据金额，成本取审核写入的库存流水；已反审核单据不计入</p>
        <table class="table" data-role="profit"></table>
      </div>
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
    const ledgerTable = mount.querySelector('[data-role="ledger"]');
    const ledgerFlowSelect = mount.querySelector('[data-role="ledger-flow"]');
    const ledgerCategorySelect = mount.querySelector('[data-role="ledger-category"]');
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
    const purchaseSummaryTable = mount.querySelector('[data-role="purchase-summary"]');

    function emptyRow(text, colspan) {
      return `<tbody><tr><td class="empty" colspan="${colspan}">${text}</td></tr></tbody>`;
    }

    function periodQuery() {
      return (
        "?from=" + encodeURIComponent(fromInput.value) + "&to=" + encodeURIComponent(toInput.value)
      );
    }

    // 库存流水明细按出入库方向与业务类型筛选，导出与打印共用。
    function ledgerQuery() {
      let query = ledgerFlowSelect.value
        ? periodQuery() + "&flow=" + ledgerFlowSelect.value
        : periodQuery();
      if (ledgerCategorySelect.value) {
        query += "&biz_category=" + encodeURIComponent(ledgerCategorySelect.value);
      }
      return query;
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
        accounts
          .map(
            (row) =>
              `<option value="${row.id}">${escapeHtml(row.code)} ${escapeHtml(row.name)}${
                row.status ? "" : "（停用）"
              }</option>`
          )
          .join("") + '<option value="0">未指定账户（历史）</option>';
      if (previous && accounts.some((row) => String(row.id) === previous)) {
        fundAccountSelect.value = previous;
      }
      fillSelect(purchaseSupplierSelect, suppliers, "全部供应商", (row) => row.code + " " + row.name);
      fillSelect(purchaseItemSelect, items, "全部商品", (row) => row.code + " " + row.name);
      fillSelect(purchaseWarehouseSelect, warehouses, "全部仓库", (row) => row.code + " " + row.name);
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
      ] = await Promise.all([
        api("GET", "/api/reports/stock-balance"),
        api("GET", "/api/reports/stock-ledger" + ledgerQuery() + "&limit=500"),
        api("GET", "/api/reports/goods-flow" + period),
        api("GET", "/api/reports/inventory-summary" + period),
        api("GET", "/api/reports/sales-profit" + period),
        api("GET", "/api/reports/ar-ap-aging?direction=" + directionSelect.value),
        api("GET", "/api/reports/fund-ledger" + fundQuery),
        api("GET", "/api/reports/fund-balances?as_of=" + encodeURIComponent(toInput.value)),
        api("GET", "/api/reports/purchase-detail" + purchaseQ + "&limit=500"),
        api(
          "GET",
          "/api/reports/purchase-summary" + purchaseQ + "&by=" + purchaseBySelect.value
        ),
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
             .join("")}</tbody>`;

      ledgerTable.innerHTML = !ledger.length
        ? emptyRow("该期间暂无流水", 8)
        : `<thead><tr><th>时间</th><th>类型</th><th>业务类型</th><th>单据号</th><th>商品</th><th>仓库</th><th>数量</th><th>金额（元）</th></tr></thead>
           <tbody>${ledger
             .map(
               (row) => `<tr>
                 <td>${escapeHtml(row.biz_time)}</td>
                 <td>${escapeHtml(bizTypeText(row.biz_type))}</td>
                 <td>${escapeHtml(bizCategoryText(row.biz_category) || "—")}</td>
                 <td>${escapeHtml(row.doc_no)}</td>
                 <td>${escapeHtml(row.item_code + " " + row.item_name)}</td>
                 <td>${escapeHtml(row.warehouse_name)}</td>
                 <td class="${qtyIsNegative(row.qty) ? "negative" : ""}">${qtyText(row.qty)}</td>
                 <td>${money(row.amount_cents)}</td>
               </tr>`
             )
             .join("")}</tbody>`;

      flowsTable.innerHTML = !flows.length
        ? emptyRow("该期间无收发", 9)
        : `<thead><tr><th>商品编码</th><th>商品名称</th><th>仓库</th><th>期初</th><th>收入</th>
             <th>发出</th><th>期末</th><th>收入金额（元）</th><th>发出金额（元）</th></tr></thead>
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
               </tr>`
             )
             .join("")}</tbody>`;

      summaryTable.innerHTML = !summary.length
        ? emptyRow("该期间无进销存发生", 8)
        : `<thead><tr><th>商品编码</th><th>商品名称</th><th>单位</th><th>期初数量</th>
             <th>入库数量</th><th>出库数量</th><th>期末数量</th><th>期末金额（元）</th></tr></thead>
           <tbody>${summary
             .map(
               (row) => `<tr>
                 <td>${escapeHtml(row.item_code)}</td>
                 <td>${escapeHtml(row.item_name)}</td>
                 <td>${escapeHtml(row.unit)}</td>
                 <td>${qtyText(row.opening_qty)}</td>
                 <td>${qtyText(row.in_qty)}</td>
                 <td>${qtyText(row.out_qty)}</td>
                 <td class="${qtyIsNegative(row.closing_qty) ? "negative" : ""}">${qtyText(row.closing_qty)}</td>
                 <td>${money(row.closing_amount_cents)}</td>
               </tr>`
             )
             .join("")}</tbody>`;

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
             .join("")}</tbody>`;

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
             .join("")}</tbody>`;

      purchaseDetailTable.innerHTML = !purchaseDetail.length
        ? emptyRow("该期间无采购", 9)
        : `<thead><tr><th>日期</th><th>单号</th><th>类型</th><th>供应商</th><th>商品</th>
             <th>规格</th><th>数量</th><th>单价（元）</th><th>金额（元）</th></tr></thead>
           <tbody>${purchaseDetail
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
             .join("")}</tbody>`;

      const summaryByItem = purchaseBySelect.value === "item";
      purchaseSummaryTable.innerHTML = !purchaseSummary.length
        ? emptyRow("该期间无采购", summaryByItem ? 7 : 4)
        : summaryByItem
          ? `<thead><tr><th>商品编码</th><th>商品名称</th><th>规格</th><th>单位</th>
               <th>数量</th><th>金额（元）</th><th>均价（元）</th></tr></thead>
             <tbody>${purchaseSummary
               .map(
                 (row) => `<tr>
                   <td>${escapeHtml(row.item_code)}</td>
                   <td>${escapeHtml(row.item_name)}</td>
                   <td>${escapeHtml(row.spec)}</td>
                   <td>${escapeHtml(row.unit)}</td>
                   <td class="${qtyIsNegative(row.qty) ? "negative" : ""}">${qtyText(row.qty)}</td>
                   <td>${money(row.amount_cents)}</td>
                   <td>${money(row.avg_price_cents)}</td>
                 </tr>`
               )
               .join("")}</tbody>`
          : `<thead><tr><th>供应商</th><th>数量</th><th>金额（元）</th><th>均价（元）</th></tr></thead>
             <tbody>${purchaseSummary
               .map(
                 (row) => `<tr>
                   <td>${escapeHtml(row.supplier)}</td>
                   <td class="${qtyIsNegative(row.qty) ? "negative" : ""}">${qtyText(row.qty)}</td>
                   <td>${money(row.amount_cents)}</td>
                   <td>${money(row.avg_price_cents)}</td>
                 </tr>`
               )
               .join("")}</tbody>`;
    }

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

    wireExport("export-balance", () => "/api/reports/stock-balance?format=csv");
    wireExport("export-ledger", () => "/api/reports/stock-ledger" + ledgerQuery() + "&format=csv");
    wireExport("export-flows", () => "/api/reports/goods-flow" + periodQuery() + "&format=csv");
    wireExport(
      "export-summary",
      () => "/api/reports/inventory-summary" + periodQuery() + "&format=csv"
    );
    wireExport("export-profit", () => "/api/reports/sales-profit" + periodQuery() + "&format=csv");
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

    wireExcel("excel-balance", () => "/api/reports/stock-balance?format=xlsx");
    wireExcel("excel-ledger", () => "/api/reports/stock-ledger" + ledgerQuery() + "&format=xlsx");
    wireExcel("excel-flows", () => "/api/reports/goods-flow" + periodQuery() + "&format=xlsx");
    wireExcel(
      "excel-summary",
      () => "/api/reports/inventory-summary" + periodQuery() + "&format=xlsx"
    );
    wireExcel("excel-profit", () => "/api/reports/sales-profit" + periodQuery() + "&format=xlsx");
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

    function wirePrint(role, pathFn) {
      mount.querySelector(`[data-role="${role}"]`).addEventListener("click", () => printTarget(pathFn()));
      const pdf = mount.querySelector(`[data-role="${role.replace(/^print-/, "pdf-")}"]`);
      if (pdf) pdf.addEventListener("click", () => exportPdfTarget(pathFn(), null));
    }

    wirePrint("print-balance", () => "/api/print/render?target_type=REPORT&target_key=stock-balance");
    wirePrint(
      "print-ledger",
      () =>
        "/api/print/render?target_type=REPORT&target_key=stock-ledger" +
        ledgerQuery() +
        "&limit=100000"
    );
    wirePrint(
      "print-flows",
      () => "/api/print/render?target_type=REPORT&target_key=goods-flow" + periodQuery()
    );
    wirePrint(
      "print-summary",
      () => "/api/print/render?target_type=REPORT&target_key=inventory-summary" + periodQuery()
    );
    wirePrint(
      "print-profit",
      () => "/api/print/render?target_type=REPORT&target_key=sales-profit" + periodQuery()
    );
    wirePrint(
      "print-purchase-detail",
      () =>
        "/api/print/render?target_type=REPORT&target_key=purchase-detail" +
        purchaseQuery() +
        "&limit=100000"
    );
    wirePrint(
      "print-purchase-summary",
      () =>
        "/api/print/render?target_type=REPORT&target_key=purchase-summary-" +
        purchaseBySelect.value +
        purchaseQuery()
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
        "/api/print/render?target_type=REPORT&target_key=fund-ledger" +
        periodQuery() +
        "&account_id=" +
        encodeURIComponent(fundAccountSelect.value || "0")
    );

    mount.querySelector('[data-role="refresh"]').addEventListener("click", reload);
    directionSelect.addEventListener("change", reload);
    fundAccountSelect.addEventListener("change", reload);
    ledgerFlowSelect.addEventListener("change", reload);
    ledgerCategorySelect.addEventListener("change", reload);
    purchaseBySelect.addEventListener("change", reload);
    purchaseSupplierSelect.addEventListener("change", reload);
    purchaseItemSelect.addEventListener("change", reload);
    purchaseWarehouseSelect.addEventListener("change", reload);
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
          <th>本期减少（元）</th><th>期末余额（元）</th></tr></thead>
        <tbody><tr>
          <td>${money(result.opening_cents)}</td>
          <td>${money(result.increase_cents)}</td>
          <td>${money(result.decrease_cents)}</td>
          <td>${money(result.closing_cents)}</td>
        </tr></tbody>`;
      linesTable.innerHTML = !result.lines.length
        ? `<tbody><tr><td class="empty" colspan="7">该期间没有发生额</td></tr></tbody>`
        : `<thead><tr><th>日期</th><th>类型</th><th>单号</th><th>摘要</th>
             <th>增加（元）</th><th>减少（元）</th><th>余额（元）</th></tr></thead>
           <tbody>${result.lines
             .map(
               (line) => `<tr>
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
                <option value="CUSTOM">自定义</option>
              </select>
            </label>
            <label class="field"><span>方向</span>
              <select data-role="orientation">
                <option value="PORTRAIT">纵向</option>
                <option value="LANDSCAPE">横向</option>
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

    function updateCustomSize() {
      const custom = pageSizeSelect.value === "CUSTOM";
      for (const field of customSizeFields) field.hidden = !custom;
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
      pageSizeSelect.value = template ? template.page_size : "A4";
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
      if (template) {
        editingColumns = template.columns.map((column) => ({ ...column }));
      } else {
        const parts = splitTarget(editorTarget.value);
        editingColumns = builtinColumns(parts.type, parts.key);
      }
      renderColumns();
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
      const body = {
        name: nameInput.value.trim(),
        code: codeInput.value.trim(),
        target_type: parts.type,
        target_key: parts.key,
        page_size: pageSizeSelect.value,
        orientation: orientationSelect.value,
        paper_width_mm: Number(paperWidthInput.value || 0),
        paper_height_mm: Number(paperHeightInput.value || 0),
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
                 <td>${escapeHtml(bizTypeText(row.biz_type))}</td>
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
    ["item", "商品"],
    ["unit", "计量单位"],
    ["warehouse", "仓库"],
    ["customer", "客户"],
    ["supplier", "供应商"],
    ["settlement", "核销单"],
    ["print_template", "打印模板"],
    ["user", "用户"],
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
  };

  function auditEntityLabel(code) {
    const found = AUDIT_ENTITIES.find(([value]) => value === code);
    return found ? found[1] : code;
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
        </div>
      </div>`;

    const form = mount.querySelector('[data-role="filters"]');
    const table = mount.querySelector('[data-role="table"]');
    const info = mount.querySelector('[data-role="page-info"]');
    const prevButton = mount.querySelector('[data-role="prev"]');
    const nextButton = mount.querySelector('[data-role="next"]');

    const field = (name) => form.querySelector('[data-field="' + name + '"]');

    async function reload() {
      const params = new URLSearchParams();
      for (const name of ["from", "to", "entity", "entity_id", "actor"]) {
        const value = field(name).value.trim();
        if (value) params.set(name, value);
      }
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
        table.innerHTML = `<thead><tr><th>时间</th><th>操作人</th><th>对象</th><th>对象 id</th><th>动作</th><th>摘要</th></tr></thead><tbody>${result.items
          .map(
            (row) => `<tr>
              <td>${escapeHtml(row.at)}</td>
              <td>${escapeHtml(row.actor)}</td>
              <td>${escapeHtml(auditEntityLabel(row.entity))}${
              row.entity_no ? " " + escapeHtml(row.entity_no) : ""
            }</td>
              <td>${row.entity_id || "—"}</td>
              <td>${escapeHtml(AUDIT_ACTIONS[row.action] || row.action)}</td>
              <td>${escapeHtml(row.detail || "—")}</td>
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
      for (const name of ["from", "to", "entity_id", "actor"]) {
        field(name).value = "";
      }
      field("entity").value = "";
      page = 1;
      reload();
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
    { key: "stock", title: "库存", group: "stock", render: stockPage },
    { key: "print-templates", title: "打印模板", group: "system", render: printTemplatePage },
    { key: "reports", title: "报表", group: "", render: reportPage },
    { key: "users", title: "用户", group: "system", permission: "user.read", render: userPage },
    {
      key: "audit",
      title: "审计日志",
      group: "system",
      permission: "audit.read",
      render: auditPage,
    },
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
    location.hash = "#/" + encodeURIComponent(page.key);
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
    main.querySelectorAll(":scope > *").forEach((node) => {
      if (node !== notice) node.remove();
    });
    showNotice("");
    const container = document.createElement("section");
    container.className = "page";
    main.appendChild(container);
    Promise.resolve(page.render(container, { crudPage, money, escapeHtml, can })).catch(
      (error) => {
        showNotice(error.message, "error");
      }
    );
  }

  function renderLogin(message) {
    nav.innerHTML = "";
    document.getElementById("mode-label").textContent = "请登录";
    document.getElementById("health").textContent = "";
    notice.hidden = true;
    main.innerHTML = `
      <div class="login-card">
        <h2>登录 小兔进销存</h2>
        <p class="hint">${escapeHtml(message || "请输入用户名与口令")}</p>
        <form data-role="login-form">
          <label>用户名<input name="username" autocomplete="username" required /></label>
          <label>口令<input name="password" type="password" autocomplete="current-password" required /></label>
          <button class="btn" type="submit">登录</button>
        </form>
      </div>`;
    const form = main.querySelector("form");
    form.addEventListener("submit", async (event) => {
      event.preventDefault();
      try {
        const result = await api("POST", "/api/login", {
          username: form.username.value.trim(),
          password: form.password.value,
        });
        state.csrf = result.csrf || "";
        await bootstrap();
      } catch (error) {
        showNotice(error.message, "error");
      }
    });
  }

  async function bootstrap() {
    try {
      const hash = location.hash || "";
      const match = /(?:^#|&)t=([0-9a-f]+)/.exec(hash);
      if (match) {
        const session = await api("POST", "/api/session", { token: match[1] });
        state.csrf = session.csrf || "";
        history.replaceState(null, "", location.pathname + location.search);
      }
      const me = await api("GET", "/api/me");
      state.csrf = me.csrf || state.csrf;
      state.permissions = me.permissions || [];

      const health = await api("GET", "/api/health");
      document.getElementById("mode-label").textContent =
        health.mode === "local" ? "单机版" : "网络版";
      document.getElementById("health").textContent =
        "v" + health.version + " · schema v" + health.schema_version;

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
      ].filter((page) => !page.permission || can(page.permission));

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
  // 关闭窗口或刷新时的浏览器原生提示。只对 --server 的普通浏览器有效；
  // 单机版 WKWebView 不执行 beforeunload，关窗不会弹确认，需在文档中注明。
  window.addEventListener("beforeunload", (event) => {
    if (!hasUnsaved()) return;
    event.preventDefault();
    event.returnValue = "";
  });
  bootstrap();
})();
