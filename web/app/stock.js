/* app/stock.js：库存查询页与角色常量。
 * 由原 web/app.js 按职责拆出，供本模块与入口共同加载。
 */
import { can, showNotice } from "./core_state.js";
import { bizTypeText, escapeHtml, money, qtyIsNegative, qtyText } from "./core_format.js";
import { api } from "./core_api.js";

  export async function stockPage(mount) {
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
  export const USER_ROLES = [
    { value: "admin", label: "管理员" },
    { value: "operator", label: "操作员" },
    { value: "viewer", label: "只读" },
  ];
