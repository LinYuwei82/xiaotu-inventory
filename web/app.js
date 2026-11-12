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
      if (doc.status === "DRAFT") {
        return (
          '<button class="link" data-role="approve">审核</button>' +
          '<button class="link" data-role="edit">编辑</button>' +
          '<button class="link danger" data-role="delete">删除</button>'
        );
      }
      if (doc.status === "APPROVED") {
        return '<button class="link danger" data-role="reverse">反审核</button>';
      }
      return "—";
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
      <div class="print-only print-head" data-role="print-head"></div>
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
          <button class="btn" data-role="print">打印</button>
        </div>
      </div>
      <div class="card">
        <div class="card-head">
          <h3>库存余额表</h3>
          <button class="btn small" data-role="export-balance">导出 CSV</button>
        </div>
        <table class="table" data-role="balances"></table>
      </div>
      <div class="card">
        <div class="card-head">
          <h3>库存流水明细</h3>
          <button class="btn small" data-role="export-ledger">导出 CSV</button>
        </div>
        <table class="table" data-role="ledger"></table>
      </div>
      <div class="card">
        <div class="card-head">
          <h3>收发明细</h3>
          <button class="btn small" data-role="export-flows">导出 CSV</button>
        </div>
        <table class="table" data-role="flows"></table>
      </div>
      <div class="card">
        <div class="card-head">
          <h3>进销存汇总</h3>
          <button class="btn small" data-role="export-summary">导出 CSV</button>
        </div>
        <p class="page-sub">按商品跨仓库汇总：期初 + 入库 − 出库 = 期末，数量与金额双口径</p>
        <table class="table" data-role="summary"></table>
      </div>
      <div class="card">
        <div class="card-head">
          <h3>应收应付余额与账龄</h3>
          <button class="btn small" data-role="export-aging">导出 CSV</button>
        </div>
        <p class="page-sub">统计范围：仅 v7 迁移后审核的单据，升级前已审核单据不计入往来与账龄；上方库存类报表不受影响</p>
        <table class="table" data-role="aging"></table>
      </div>`;

    const fromInput = mount.querySelector('[data-role="from"]');
    const toInput = mount.querySelector('[data-role="to"]');
    const directionSelect = mount.querySelector('[data-role="direction"]');
    const printHead = mount.querySelector('[data-role="print-head"]');
    const balancesTable = mount.querySelector('[data-role="balances"]');
    const ledgerTable = mount.querySelector('[data-role="ledger"]');
    const flowsTable = mount.querySelector('[data-role="flows"]');
    const summaryTable = mount.querySelector('[data-role="summary"]');
    const agingTable = mount.querySelector('[data-role="aging"]');

    function emptyRow(text, colspan) {
      return `<tbody><tr><td class="empty" colspan="${colspan}">${text}</td></tr></tbody>`;
    }

    function periodQuery() {
      return (
        "?from=" + encodeURIComponent(fromInput.value) + "&to=" + encodeURIComponent(toInput.value)
      );
    }

    function renderPrintHead() {
      const scope =
        `期间 ${fromInput.value || "不限"} 至 ${toInput.value || "不限"}` +
        `；账龄方向 ${directionSelect.value === "-1" ? "应付" : "应收"}`;
      printHead.innerHTML = `
        <div><strong>单位名称：</strong>小兔进销存</div>
        <div><strong>报表：</strong>库存余额表 / 库存流水明细 / 收发明细 / 进销存汇总 / 应收应付余额与账龄</div>
        <div><strong>筛选区间：</strong>${escapeHtml(scope)}</div>
        <div><strong>打印时间：</strong>${escapeHtml(new Date().toLocaleString("zh-CN", { hour12: false }))}</div>`;
    }

    async function reload() {
      const period = periodQuery();
      const [balances, ledger, flows, summary, aging] = await Promise.all([
        api("GET", "/api/reports/stock-balance"),
        api("GET", "/api/reports/stock-ledger" + period + "&limit=500"),
        api("GET", "/api/reports/goods-flow" + period),
        api("GET", "/api/reports/inventory-summary" + period),
        api("GET", "/api/reports/ar-ap-aging?direction=" + directionSelect.value),
      ]);
      renderPrintHead();

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
    wireExport(
      "export-aging",
      () => "/api/reports/ar-ap-aging?direction=" + directionSelect.value + "&format=csv"
    );

    mount.querySelector('[data-role="print"]').addEventListener("click", () => {
      renderPrintHead();
      printPage();
    });
    mount.querySelector('[data-role="refresh"]').addEventListener("click", reload);
    directionSelect.addEventListener("change", reload);
    await reload();
  }

  // ---------- 对账单 ----------

  async function statementPage(mount) {
    const today = new Date().toISOString().slice(0, 10);
    mount.innerHTML = `
      <div class="print-only print-head" data-role="print-head"></div>
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
    const printHead = mount.querySelector('[data-role="print-head"]');
    const title = mount.querySelector('[data-role="title"]');
    const summaryTable = mount.querySelector('[data-role="summary"]');
    const linesTable = mount.querySelector('[data-role="lines"]');
    let current = null;

    function periodLabel() {
      return (
        (fromInput.value || "起始不限") + " 至 " + (toInput.value || "截止不限")
      );
    }

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
      current = result;
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

    function renderPrintHead() {
      printHead.innerHTML = `
        <div><strong>单位名称：</strong>小兔进销存</div>
        <div><strong>报表：</strong>${escapeHtml(
          current ? title.textContent : "往来对账单"
        )}</div>
        <div><strong>筛选区间：</strong>${escapeHtml(periodLabel())}</div>
        <div><strong>打印时间：</strong>${escapeHtml(
          new Date().toLocaleString("zh-CN", { hour12: false })
        )}</div>`;
    }

    async function reload() {
      if (!partnerSelect.value) {
        showNotice("请先在客户或供应商档案中新增往来单位", "error");
        summaryTable.innerHTML = "";
        linesTable.innerHTML = "";
        current = null;
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
    mount.querySelector('[data-role="print"]').addEventListener("click", () => {
      renderPrintHead();
      printPage();
    });

    await loadPartners();
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
