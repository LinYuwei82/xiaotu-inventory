/* app/report.js：经营报表页（筛选、查询编排与事件绑定）。
 * 页面骨架与表格渲染在 report_tables*.js，导出 / 打印绑定在 report_actions.js。
 */
import { editionAllows } from "./core_state.js";
import { escapeHtml, itemOptionText } from "./core_format.js";
import { api } from "./core_api.js";
import { wireReportDownloads, wireReportPrints } from "./report_actions.js";
import { reportPageHtml } from "./report_tables.js";
import {
  balancesTableHtml,
  goodsFlowTableHtml,
  inventorySummaryTableHtml,
  ledgerTableHtml,
  otherFlowTableHtml,
  profitTableHtml,
} from "./report_tables_stock.js";
import {
  agingTableHtml,
  fundBalancesTableHtml,
  fundLedgerTableHtml,
  purchaseDetailTableHtml,
  purchaseSummaryTableHtml,
} from "./report_tables_trade.js";

export async function reportPage(mount) {
  const today = new Date().toISOString().slice(0, 10);
  const profitAllowed = editionAllows("report.profit");
  mount.innerHTML = reportPageHtml(today, profitAllowed);

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
    const fundQuery = period + "&account_id=" + encodeURIComponent(fundAccountSelect.value || "0");
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

    fundLedgerTable.innerHTML = fundLedgerTableHtml(fundLedger);
    fundBalancesTable.innerHTML = fundBalancesTableHtml(fundBalances);
    balancesTable.innerHTML = balancesTableHtml(balances);

    const ledgerRows = ledger.rows || [];
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
    ledgerTable.innerHTML = ledgerTableHtml(ledger);
    flowsTable.innerHTML = goodsFlowTableHtml(flows);
    summaryTable.innerHTML = inventorySummaryTableHtml(summary);
    // 社区版不渲染毛利卡片，profitTable 为空时整段跳过。
    if (profitTable) profitTable.innerHTML = profitTableHtml(profit);
    agingTable.innerHTML = agingTableHtml(aging);

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
    purchaseDetailTable.innerHTML = purchaseDetailTableHtml(purchaseDetail);
    purchaseSummaryTable.innerHTML = purchaseSummaryTableHtml(
      purchaseSummary,
      purchaseBySelect.value === "item"
    );
    otherFlowTable.innerHTML = otherFlowTableHtml(otherFlow);
  }

  // 导出 / 打印按钮共用同一套查询函数，保证与页面筛选一致。
  const q = {
    period: periodQuery,
    balance: balanceQuery,
    ledger: ledgerQuery,
    purchase: purchaseQuery,
    otherFlow: otherFlowQuery,
    purchaseBy: () => purchaseBySelect.value,
    direction: () => directionSelect.value,
    fundAccount: () => fundAccountSelect.value,
    to: () => toInput.value,
  };
  wireReportDownloads(mount, q, profitAllowed);
  wireReportPrints(mount, q, profitAllowed);

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
