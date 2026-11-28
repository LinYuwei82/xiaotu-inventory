/* app/salesreport.js：销售报表、仪表盘、往来对账单。
 * 由原 web/app.js 按职责拆出，供本模块与入口共同加载。
 */
import { api, downloadCsv, downloadXlsx, emptyRow, escapeHtml, itemOptionText, money, showNotice, withQuery } from "./core.js";
import { exportPdfTarget, printTarget } from "./print.js";

  export async function salesReportPage(mount) {
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
  export async function dashboardPage(mount) {
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
  export async function statementPage(mount) {
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
