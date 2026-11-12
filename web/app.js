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
  };

  const main = document.getElementById("main");
  const nav = document.getElementById("nav");
  const notice = document.getElementById("notice");

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

  function showNotice(message, kind) {
    notice.textContent = message;
    notice.className = "notice" + (kind ? " " + kind : "");
    notice.hidden = !message;
  }

  async function api(method, path, body) {
    const options = { method, credentials: "include", headers: {} };
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

  function printWithTemplate(data) {
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
    const cleanup = () => {
      document.body.classList.remove("print-mode");
      view.remove();
      style.remove();
    };
    // 浏览器打印结束（含取消）后清理；单机版桥接不触发时，下次打印会先清掉旧视图。
    window.addEventListener("afterprint", cleanup, { once: true });
    printPage();
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

  // ---------- 通用 CRUD 页面（供 codegen 生成的页面调用） ----------

  async function crudPage(mount, def) {
    let rows = [];
    let editingId = 0;
    const optionCache = {};

    mount.innerHTML = `
      <div class="page-head">
        <div>
          <h2>${escapeHtml(def.title)}</h2>
          <p class="page-sub">${escapeHtml(def.api)}</p>
        </div>
        <button class="btn primary" data-role="new">新增</button>
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
        if (!confirm("确认删除这条记录？")) return;
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
    let warehouses = [];
    let items = [];
    let documents = [];
    let editingId = 0;

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
            <label class="field"><span>${config.twoWarehouse ? "调出仓库" : "仓库"}</span>
              <select data-role="warehouse" required></select>
            </label>
            <label class="field" ${config.twoWarehouse ? "" : "hidden"}><span>调入仓库</span>
              <select data-role="target-warehouse" required></select>
            </label>
            <label class="field"><span>单据日期</span>
              <input type="date" data-role="date" required />
            </label>
            <label class="field"><span>${escapeHtml(config.partnerLabel)}</span>
              <input type="text" data-role="partner" placeholder="${escapeHtml(
                config.partnerPlaceholder
              )}" ${config.partnerApi ? "hidden" : ""} />
              <select data-role="partner-select" ${config.partnerApi ? "" : "hidden"}></select>
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
    const documentsTable = mount.querySelector('[data-role="documents"]');

    dateInput.value = new Date().toISOString().slice(0, 10);

    function itemOptions(selected) {
      return items
        .map(
          (item) =>
            `<option value="${item.id}" ${String(item.id) === String(selected) ? "selected" : ""}>${escapeHtml(
              item.code + " " + item.name
            )}</option>`
        )
        .join("");
    }

    function addLine(line) {
      const row = document.createElement("tr");
      row.innerHTML = `
        <td><select data-role="item">${itemOptions(line ? line.item_id : undefined)}</select></td>
        <td><input type="number" min="1" step="1" value="1" data-role="qty" /></td>
        <td><input type="number" min="0" step="0.01" value="0.00" data-role="price" /></td>
        <td><input type="text" class="readonly" value="—" data-role="amount" disabled /></td>
        <td class="actions"><button class="link danger" type="button" data-role="remove">移除</button></td>`;
      linesTable.querySelector("tbody").appendChild(row);
      row.querySelector('[data-role="remove"]').addEventListener("click", () => row.remove());
      const recalc = () => {
        const qty = Number(row.querySelector('[data-role="qty"]').value || 0);
        const price = Number(row.querySelector('[data-role="price"]').value || 0);
        row.querySelector('[data-role="amount"]').value = (qty * price).toFixed(2);
      };
      row.querySelector('[data-role="qty"]').addEventListener("input", recalc);
      row.querySelector('[data-role="price"]').addEventListener("input", recalc);
      if (line) {
        row.querySelector('[data-role="qty"]').value = line.qty;
        row.querySelector('[data-role="price"]').value = (line.price_cents / 100).toFixed(2);
        recalc();
      }
    }

    function resetLines() {
      linesTable.innerHTML = `<thead><tr><th>商品</th><th>数量</th><th>单价（元）</th><th>金额（元）</th><th class="actions">操作</th></tr></thead><tbody></tbody>`;
      addLine();
    }

    function statusLabel(status) {
      return { APPROVED: "已审核", DRAFT: "草稿", VOIDED: "已作废" }[status] || status;
    }

    function actionsFor(doc) {
      const print = '<button class="link" data-role="print">打印</button>';
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
        )}</th><th>数量</th><th>金额（元）</th><th>状态</th><th class="actions">操作</th></tr></thead>
        <tbody>${documents
          .map(
            (doc) => `<tr data-id="${doc.id}">
              <td>${escapeHtml(doc.doc_no)}</td>
              <td>${escapeHtml(doc.doc_date)}</td>
              <td>${escapeHtml(doc.warehouse_name)}</td>
              <td>${escapeHtml(doc.customer || "—")}</td>
              <td>${doc.total_qty}</td>
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
      mount.querySelector('[data-role="cancel-edit"]').hidden = true;
      form.querySelector('[data-role="partner"]').value = "";
      if (partnerSelect) partnerSelect.value = "";
      dateInput.value = new Date().toISOString().slice(0, 10);
      resetLines();
    }

    function collectLines() {
      const lines = [];
      for (const row of linesTable.querySelectorAll("tbody tr")) {
        const itemId = Number(row.querySelector('[data-role="item"]').value);
        const qty = parseInt(row.querySelector('[data-role="qty"]').value, 10);
        const price = Math.round(Number(row.querySelector('[data-role="price"]').value || 0) * 100);
        if (!itemId || !qty || qty <= 0) {
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
        warehouse_id: Number(warehouseSelect.value),
        target_warehouse_id: config.twoWarehouse ? Number(targetWarehouseSelect.value) : 0,
        doc_date: dateInput.value,
        partner_id: usePartnerSelect ? Number(partnerSelect.value || 0) : 0,
        partner: usePartnerSelect
          ? partnerOption && partnerOption.value
            ? partnerOption.dataset.name || ""
            : ""
          : partnerInput.value.trim(),
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
        linesTable.innerHTML = `<thead><tr><th>商品</th><th>数量</th><th>单价（元）</th><th>金额（元）</th><th class="actions">操作</th></tr></thead><tbody></tbody>`;
        for (const line of detail.lines) addLine(line);
        mount.querySelector('[data-role="cancel-edit"]').hidden = false;
        form.scrollIntoView({ behavior: "smooth", block: "start" });
        showNotice("正在编辑草稿 " + detail.doc_no, "ok");
      } catch (error) {
        showNotice(error.message, "error");
      }
    }

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
        if (button.dataset.role === "reverse") {
          if (!confirm("反审核会生成反向库存流水，确认继续？")) return;
          await api("POST", "/api/documents/" + id + "/reverse");
          showNotice("已反审核", "ok");
        } else if (button.dataset.role === "approve") {
          if (!confirm("审核后才会写库存流水，确认继续？")) return;
          await api("POST", "/api/documents/" + id + "/approve");
          showNotice("已审核", "ok");
        } else if (button.dataset.role === "delete") {
          if (!confirm("确认删除这张草稿？")) return;
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

  // ---------- 销售订单 ----------

  async function orderPage(mount) {
    const docType = "SAL_ORDER";
    let warehouses = [];
    let items = [];
    let customers = [];
    let documents = [];
    let editingId = 0;
    let convertingId = 0;

    mount.innerHTML = `
      <div class="page-head">
        <div>
          <h2>销售订单</h2>
          <p class="page-sub">只做需求确认与转出库来源，不占库存、不写流水、不产生往来</p>
        </div>
        <button class="btn" data-role="refresh">刷新</button>
      </div>
      <div class="card">
        <form class="doc-form" data-role="form">
          <div class="doc-head">
            <label class="field"><span>预计出库仓</span>
              <select data-role="warehouse" required></select>
            </label>
            <label class="field"><span>订单日期</span>
              <input type="date" data-role="date" required />
            </label>
            <label class="field"><span>客户</span>
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
          <h3>转出库</h3>
          <span class="page-sub" data-role="convert-title"></span>
        </div>
        <div class="doc-head">
          <label class="field"><span>出库仓库</span>
            <select data-role="convert-warehouse"></select>
          </label>
          <label class="field"><span>出库日期</span>
            <input type="date" data-role="convert-date" />
          </label>
        </div>
        <table class="table" data-role="convert-lines"></table>
        <div class="form-actions">
          <button class="btn primary" type="button" data-role="convert-submit">确认转出</button>
          <button class="btn" type="button" data-role="convert-cancel">取消</button>
        </div>
      </div>
      <div class="card">
        <table class="table" data-role="documents"></table>
      </div>`;

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

    dateInput.value = new Date().toISOString().slice(0, 10);
    convertDate.value = dateInput.value;

    function itemLabel(itemId) {
      const item = items.find((row) => row.id === itemId);
      return item ? item.code + " " + item.name : String(itemId);
    }

    function itemOptions(selected) {
      return items
        .map(
          (item) =>
            `<option value="${item.id}" ${String(item.id) === String(selected) ? "selected" : ""}>${escapeHtml(
              item.code + " " + item.name
            )}</option>`
        )
        .join("");
    }

    function addLine(line) {
      const row = document.createElement("tr");
      row.innerHTML = `
        <td><select data-role="item">${itemOptions(line ? line.item_id : undefined)}</select></td>
        <td><input type="number" min="1" step="1" value="1" data-role="qty" /></td>
        <td><input type="number" min="0" step="0.01" value="0.00" data-role="price" /></td>
        <td><input type="text" class="readonly" value="—" data-role="amount" disabled /></td>
        <td class="actions"><button class="link danger" type="button" data-role="remove">移除</button></td>`;
      linesTable.querySelector("tbody").appendChild(row);
      row.querySelector('[data-role="remove"]').addEventListener("click", () => row.remove());
      const recalc = () => {
        const qty = Number(row.querySelector('[data-role="qty"]').value || 0);
        const price = Number(row.querySelector('[data-role="price"]').value || 0);
        row.querySelector('[data-role="amount"]').value = (qty * price).toFixed(2);
      };
      row.querySelector('[data-role="qty"]').addEventListener("input", recalc);
      row.querySelector('[data-role="price"]').addEventListener("input", recalc);
      if (line) {
        row.querySelector('[data-role="qty"]').value = line.qty;
        row.querySelector('[data-role="price"]').value = (line.price_cents / 100).toFixed(2);
        recalc();
      }
    }

    function resetLines() {
      linesTable.innerHTML = `<thead><tr><th>商品</th><th>数量</th><th>单价（元）</th><th>金额（元）</th><th class="actions">操作</th></tr></thead><tbody></tbody>`;
      addLine();
    }

    function orderStatusLabel(doc) {
      if (doc.status === "DRAFT") return "草稿";
      if (doc.order_status === "CONVERTED") return "已转出库";
      if (doc.order_status === "CLOSED") return "已关闭";
      return "已确认";
    }

    function actionsFor(doc) {
      if (doc.status === "DRAFT") {
        return (
          '<button class="link" data-role="approve">确认</button>' +
          '<button class="link" data-role="edit">编辑</button>' +
          '<button class="link danger" data-role="delete">删除</button>'
        );
      }
      if (doc.order_status === "CONFIRMED") {
        return (
          '<button class="link" data-role="convert">转出库</button>' +
          '<button class="link danger" data-role="close">关闭</button>'
        );
      }
      return "—";
    }

    function renderDocuments() {
      if (!documents.length) {
        documentsTable.innerHTML =
          '<tbody><tr><td class="empty">还没有销售订单</td></tr></tbody>';
        return;
      }
      documentsTable.innerHTML = `
        <thead><tr><th>订单号</th><th>日期</th><th>仓库</th><th>客户</th><th>数量</th>
          <th>金额（元）</th><th>状态</th><th class="actions">操作</th></tr></thead>
        <tbody>${documents
          .map(
            (doc) => `<tr data-id="${doc.id}">
              <td>${escapeHtml(doc.doc_no)}</td>
              <td>${escapeHtml(doc.doc_date)}</td>
              <td>${escapeHtml(doc.warehouse_name)}</td>
              <td>${escapeHtml(doc.customer || "—")}</td>
              <td>${doc.total_qty}</td>
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
        api("GET", "/api/customers"),
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
        const qty = parseInt(row.querySelector('[data-role="qty"]').value, 10);
        const price = Math.round(Number(row.querySelector('[data-role="price"]').value || 0) * 100);
        if (!itemId || !qty || qty <= 0) {
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
        convertTitle.textContent = detail.doc_no + " · " + (detail.partner || "未指定客户");
        convertWarehouse.value = String(detail.warehouse_id);
        convertDate.value = new Date().toISOString().slice(0, 10);
        convertLines.innerHTML = `
          <thead><tr><th>商品</th><th>订单数量</th><th>已转</th><th>剩余</th><th>本次转出</th></tr></thead>
          <tbody>${detail.lines
            .map((line) => {
              const remaining = line.qty - line.converted_qty;
              return `<tr data-line-id="${line.id}">
                <td>${escapeHtml(itemLabel(line.item_id))}</td>
                <td>${line.qty}</td>
                <td>${line.converted_qty}</td>
                <td>${remaining}</td>
                <td><input type="number" min="0" max="${remaining}" step="1"
                  value="${remaining}" data-role="convert-qty" ${remaining <= 0 ? "disabled" : ""} /></td>
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
        const qty = parseInt(input.value || 0, 10);
        if (qty > 0) lines.push({ order_line_id: Number(row.dataset.lineId), qty });
      }
      if (!lines.length) {
        showNotice("请填写要转出的数量", "error");
        return;
      }
      try {
        const result = await api("POST", "/api/documents/" + convertingId + "/convert", {
          warehouse_id: Number(convertWarehouse.value || 0),
          doc_date: convertDate.value,
          lines,
        });
        showNotice("已生成销售出库单：" + result.doc_no, "ok");
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

    documentsTable.addEventListener("click", async (event) => {
      const button = event.target.closest("button");
      if (!button) return;
      const id = Number(event.target.closest("tr").dataset.id);
      try {
        if (button.dataset.role === "approve") {
          if (!confirm("确认订单后不可再改单，只能转出库或关闭。继续？")) return;
          await api("POST", "/api/documents/" + id + "/approve");
          showNotice("订单已确认", "ok");
        } else if (button.dataset.role === "edit") {
          await startEdit(documents.find((doc) => doc.id === id));
          return;
        } else if (button.dataset.role === "delete") {
          if (!confirm("确认删除这张订单草稿？")) return;
          await api("DELETE", "/api/documents/" + id);
          if (editingId === id) resetForm();
          showNotice("订单草稿已删除", "ok");
        } else if (button.dataset.role === "convert") {
          await openConvert(documents.find((doc) => doc.id === id));
          return;
        } else if (button.dataset.role === "close") {
          if (!confirm("关闭后订单不能再转出，确认？")) return;
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
    mount.innerHTML = `
      <div class="page-head">
        <div>
          <h2>应收应付</h2>
          <p class="page-sub">单据审核自动生成；退货为红字，反审核未核销时作废</p>
          <p class="page-sub">统计范围：仅统计 v7 迁移之后审核的单据，升级前已审核单据不计入往来与账龄</p>
        </div>
        <div class="toolbar">
          <select data-role="direction">
            <option value="1">应收</option>
            <option value="-1">应付</option>
          </select>
          <button class="btn" data-role="refresh">刷新</button>
        </div>
      </div>
      <div class="card">
        <table class="table" data-role="rows"></table>
      </div>
      <div class="card">
        <h3>核销记录</h3>
        <table class="table" data-role="settlements"></table>
      </div>`;
    const directionSelect = mount.querySelector('[data-role="direction"]');
    const rowsTable = mount.querySelector('[data-role="rows"]');
    const settlementsTable = mount.querySelector('[data-role="settlements"]');
    let entries = [];

    async function reload() {
      const direction = directionSelect.value;
      const [rows, settlements] = await Promise.all([
        api("GET", "/api/ar-ap?direction=" + direction),
        api("GET", "/api/settlements?direction=" + direction),
      ]);
      entries = rows;
      rowsTable.innerHTML = !rows.length
        ? '<tbody><tr><td class="empty">暂无记录</td></tr></tbody>'
        : `<thead><tr><th>单号</th><th>类型</th><th>往来单位</th><th>日期</th>
             <th>金额（元）</th><th>已核销</th><th>未核销</th><th>状态</th><th></th></tr></thead>
           <tbody>${rows
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
                   row.outstanding_cents !== 0 && row.status !== "VOIDED"
                     ? `<button class="btn small" data-settle="${row.id}">核销</button>`
                     : ""
                 }</td>
               </tr>`
             )
             .join("")}</tbody>`;
      settlementsTable.innerHTML = !settlements.length
        ? '<tbody><tr><td class="empty">暂无核销</td></tr></tbody>'
        : `<thead><tr><th>核销单号</th><th>往来单位</th><th>日期</th>
             <th>金额（元）</th><th>备注</th><th></th></tr></thead>
           <tbody>${settlements
             .map(
               (row, index) => `<tr>
                 <td>${escapeHtml(row.settle_no)}</td>
                 <td>${escapeHtml(row.partner_name || "未指定")}</td>
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
          if (!window.confirm(`撤销核销 ${row.settle_no}？`)) return;
          try {
            await api("POST", `/api/settlements/${row.id}/reverse`, {});
            await reload();
          } catch (error) {
            window.alert(error.message);
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
      const input = window.prompt("本次核销金额（元）", defaultAmount);
      if (input === null) return;
      const yuan = Number(input);
      if (!Number.isFinite(yuan) || yuan <= 0) {
        window.alert("请输入大于 0 的金额");
        return;
      }
      const cents = Math.round(yuan * 100) * Math.sign(entry.outstanding_cents);
      try {
        await api("POST", "/api/settlements", {
          direction: Number(directionSelect.value),
          partner_key: entry.partner_key,
          amount_cents: cents,
          allocations: [{ entry_id: entry.id, amount_cents: cents }],
          note: "",
        });
        await reload();
      } catch (error) {
        window.alert(error.message);
      }
    });

    mount.querySelector('[data-role="refresh"]').addEventListener("click", reload);
    directionSelect.addEventListener("change", reload);
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
          <h3>库存余额表</h3>
          <button class="btn small" data-role="export-balance">导出 CSV</button>
          <button class="btn small" data-role="excel-balance">导出 Excel</button>
          <button class="btn small" data-role="print-balance">打印</button>
        </div>
        <table class="table" data-role="balances"></table>
      </div>
      <div class="card">
        <div class="card-head">
          <h3>库存流水明细</h3>
          <button class="btn small" data-role="export-ledger">导出 CSV</button>
          <button class="btn small" data-role="excel-ledger">导出 Excel</button>
          <button class="btn small" data-role="print-ledger">打印</button>
        </div>
        <table class="table" data-role="ledger"></table>
      </div>
      <div class="card">
        <div class="card-head">
          <h3>收发明细</h3>
          <button class="btn small" data-role="export-flows">导出 CSV</button>
          <button class="btn small" data-role="excel-flows">导出 Excel</button>
          <button class="btn small" data-role="print-flows">打印</button>
        </div>
        <table class="table" data-role="flows"></table>
      </div>
      <div class="card">
        <div class="card-head">
          <h3>进销存汇总</h3>
          <button class="btn small" data-role="export-summary">导出 CSV</button>
          <button class="btn small" data-role="excel-summary">导出 Excel</button>
          <button class="btn small" data-role="print-summary">打印</button>
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
        </div>
        <p class="page-sub">收入取单据金额，成本取审核写入的库存流水；已反审核单据不计入</p>
        <table class="table" data-role="profit"></table>
      </div>
      <div class="card">
        <div class="card-head">
          <h3>应收应付余额与账龄</h3>
          <button class="btn small" data-role="export-aging">导出 CSV</button>
          <button class="btn small" data-role="excel-aging">导出 Excel</button>
          <button class="btn small" data-role="print-aging">打印</button>
        </div>
        <p class="page-sub">统计范围：仅 v7 迁移后审核的单据，升级前已审核单据不计入往来与账龄；上方库存类报表不受影响</p>
        <table class="table" data-role="aging"></table>
      </div>`;

    const fromInput = mount.querySelector('[data-role="from"]');
    const toInput = mount.querySelector('[data-role="to"]');
    const directionSelect = mount.querySelector('[data-role="direction"]');
    const balancesTable = mount.querySelector('[data-role="balances"]');
    const ledgerTable = mount.querySelector('[data-role="ledger"]');
    const flowsTable = mount.querySelector('[data-role="flows"]');
    const summaryTable = mount.querySelector('[data-role="summary"]');
    const profitTable = mount.querySelector('[data-role="profit"]');
    const agingTable = mount.querySelector('[data-role="aging"]');

    function emptyRow(text, colspan) {
      return `<tbody><tr><td class="empty" colspan="${colspan}">${text}</td></tr></tbody>`;
    }

    function periodQuery() {
      return (
        "?from=" + encodeURIComponent(fromInput.value) + "&to=" + encodeURIComponent(toInput.value)
      );
    }

    async function reload() {
      const period = periodQuery();
      const [balances, ledger, flows, summary, profit, aging] = await Promise.all([
        api("GET", "/api/reports/stock-balance"),
        api("GET", "/api/reports/stock-ledger" + period + "&limit=500"),
        api("GET", "/api/reports/goods-flow" + period),
        api("GET", "/api/reports/inventory-summary" + period),
        api("GET", "/api/reports/sales-profit" + period),
        api("GET", "/api/reports/ar-ap-aging?direction=" + directionSelect.value),
      ]);

      balancesTable.innerHTML = !balances.length
        ? emptyRow("暂无库存", 5)
        : `<thead><tr><th>商品编码</th><th>商品名称</th><th>仓库</th><th>数量</th><th>金额（元）</th></tr></thead>
           <tbody>${balances
             .map(
               (row) => `<tr>
                 <td>${escapeHtml(row.item_code)}</td>
                 <td>${escapeHtml(row.item_name)}</td>
                 <td>${escapeHtml(row.warehouse_name)}</td>
                 <td class="${row.qty < 0 ? "negative" : ""}">${row.qty}</td>
                 <td>${money(row.amount_cents)}</td>
               </tr>`
             )
             .join("")}</tbody>`;

      ledgerTable.innerHTML = !ledger.length
        ? emptyRow("该期间暂无流水", 7)
        : `<thead><tr><th>时间</th><th>类型</th><th>单据号</th><th>商品</th><th>仓库</th><th>数量</th><th>金额（元）</th></tr></thead>
           <tbody>${ledger
             .map(
               (row) => `<tr>
                 <td>${escapeHtml(row.biz_time)}</td>
                 <td>${escapeHtml(row.biz_type)}</td>
                 <td>${escapeHtml(row.doc_no)}</td>
                 <td>${escapeHtml(row.item_code + " " + row.item_name)}</td>
                 <td>${escapeHtml(row.warehouse_name)}</td>
                 <td class="${row.qty < 0 ? "negative" : ""}">${row.qty}</td>
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
                 <td>${row.opening_qty}</td>
                 <td>${row.in_qty}</td>
                 <td>${row.out_qty}</td>
                 <td class="${row.closing_qty < 0 ? "negative" : ""}">${row.closing_qty}</td>
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
                 <td>${row.opening_qty}</td>
                 <td>${row.in_qty}</td>
                 <td>${row.out_qty}</td>
                 <td class="${row.closing_qty < 0 ? "negative" : ""}">${row.closing_qty}</td>
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
                 <td>${row.qty}</td>
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
    wireExport("export-ledger", () => "/api/reports/stock-ledger" + periodQuery() + "&format=csv");
    wireExport("export-flows", () => "/api/reports/goods-flow" + periodQuery() + "&format=csv");
    wireExport(
      "export-summary",
      () => "/api/reports/inventory-summary" + periodQuery() + "&format=csv"
    );
    wireExport("export-profit", () => "/api/reports/sales-profit" + periodQuery() + "&format=csv");
    wireExport(
      "export-aging",
      () => "/api/reports/ar-ap-aging?direction=" + directionSelect.value + "&format=csv"
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
    wireExcel("excel-ledger", () => "/api/reports/stock-ledger" + periodQuery() + "&format=xlsx");
    wireExcel("excel-flows", () => "/api/reports/goods-flow" + periodQuery() + "&format=xlsx");
    wireExcel(
      "excel-summary",
      () => "/api/reports/inventory-summary" + periodQuery() + "&format=xlsx"
    );
    wireExcel("excel-profit", () => "/api/reports/sales-profit" + periodQuery() + "&format=xlsx");
    wireExcel(
      "excel-aging",
      () => "/api/reports/ar-ap-aging?direction=" + directionSelect.value + "&format=xlsx"
    );

    function wirePrint(role, pathFn) {
      mount.querySelector(`[data-role="${role}"]`).addEventListener("click", () => printTarget(pathFn()));
    }

    wirePrint("print-balance", () => "/api/print/render?target_type=REPORT&target_key=stock-balance");
    wirePrint(
      "print-ledger",
      () =>
        "/api/print/render?target_type=REPORT&target_key=stock-ledger" +
        periodQuery() +
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
      "print-aging",
      () =>
        "/api/print/render?target_type=REPORT&target_key=ar-ap-aging&direction=" +
        directionSelect.value
    );

    mount.querySelector('[data-role="refresh"]').addEventListener("click", reload);
    directionSelect.addEventListener("change", reload);
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
          if (!confirm("确认删除模板「" + template.name + "」？删除后该目标回退到内置默认。")) {
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
        <h3>库存流水</h3>
        <table class="table" data-role="ledger"></table>
      </div>`;

    const balancesTable = mount.querySelector('[data-role="balances"]');
    const ledgerTable = mount.querySelector('[data-role="ledger"]');

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
                 <td class="${row.qty < 0 ? "negative" : ""}">${row.qty}</td>
                 <td>${money(row.amount_cents)}</td>
               </tr>`
             )
             .join("")}</tbody>`;

      ledgerTable.innerHTML = !ledger.length
        ? '<tbody><tr><td class="empty">暂无流水</td></tr></tbody>'
        : `<thead><tr><th>时间</th><th>类型</th><th>单据号</th><th>商品</th><th>仓库</th><th>数量</th><th>金额（元）</th></tr></thead>
           <tbody>${ledger
             .map(
               (row) => `<tr>
                 <td>${escapeHtml(row.biz_time)}</td>
                 <td>${escapeHtml(row.biz_type)}</td>
                 <td>${escapeHtml(row.doc_no)}</td>
                 <td>${escapeHtml(row.item_code + " " + row.item_name)}</td>
                 <td>${escapeHtml(row.warehouse_name)}</td>
                 <td class="${row.qty < 0 ? "negative" : ""}">${row.qty}</td>
                 <td>${money(row.amount_cents)}</td>
               </tr>`
             )
             .join("")}</tbody>`;
    }

    mount.querySelector('[data-role="refresh"]').addEventListener("click", () => reload());
    await reload();
  }

  // ---------- 导航与启动 ----------

  const FIXED_PAGES = [
    {
      key: "purchase",
      title: "采购入库",
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
      key: "sales",
      title: "销售出库",
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
      key: "sales-orders",
      title: "销售订单",
      render: orderPage,
    },
    {
      key: "transfer",
      title: "调拨",
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
      key: "ar-ap",
      title: "应收应付",
      render: financePage,
    },
    { key: "statement", title: "对账单", render: statementPage },
    { key: "stock", title: "库存", render: stockPage },
    { key: "print-templates", title: "打印模板", render: printTemplatePage },
    { key: "reports", title: "报表", render: reportPage },
  ];

  function mountPage(page) {
    state.current = page.key;
    for (const link of nav.querySelectorAll("button")) {
      link.classList.toggle("active", link.dataset.key === page.key);
    }
    main.querySelectorAll(":scope > *").forEach((node) => {
      if (node !== notice) node.remove();
    });
    showNotice("");
    const container = document.createElement("section");
    container.className = "page";
    main.appendChild(container);
    Promise.resolve(page.render(container, { crudPage, money, escapeHtml })).catch((error) => {
      showNotice(error.message, "error");
    });
  }

  async function bootstrap() {
    try {
      const hash = location.hash || "";
      const match = /(?:^#|&)t=([0-9a-f]+)/.exec(hash);
      if (match) {
        await api("POST", "/api/session", { token: match[1] });
        history.replaceState(null, "", location.pathname + location.search);
      }

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
          render: (mount, ui) => window.XiaotuPages[page.key].render(mount, ui),
        })),
        ...FIXED_PAGES,
      ];

      nav.innerHTML = all
        .map(
          (page) =>
            `<button type="button" data-key="${page.key}">${escapeHtml(page.title)}</button>`
        )
        .join("");
      nav.addEventListener("click", (event) => {
        const button = event.target.closest("button");
        if (!button) return;
        const page = all.find((item) => item.key === button.dataset.key);
        if (page) mountPage(page);
      });

      mountPage(all[0]);
    } catch (error) {
      if (error.status === 401) {
        showNotice(
          "未获得访问凭据。请通过启动程序时输出的带 #t= 的链接打开，或在网络版中使用该链接登录。",
          "error"
        );
      } else {
        showNotice("加载失败: " + error.message, "error");
      }
    }
  }

  bootstrap();
})();
