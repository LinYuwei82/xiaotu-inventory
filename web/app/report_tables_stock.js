/* app/report_tables_stock.js：库存类报表表格的纯渲染函数（输入数据，输出 HTML）。
 * 包含库存余额、库存流水、收发明细、进销存汇总、销售毛利与其他出入库。
 */
import { bizCategoryText, bizTypeText, emptyRow, escapeHtml, money, qtyIsNegative, qtyText } from "./core.js";

// 数量跨单位不可加总，页面合计只汇总金额，与 CSV / Excel 一致。
export function balancesTableHtml(rows) {
  const balanceAmount = rows.reduce((sum, row) => sum + row.amount_cents, 0);
  return !rows.length
    ? emptyRow("暂无库存", 5)
    : `<thead><tr><th>商品编码</th><th>商品名称</th><th>仓库</th><th>数量</th><th>金额（元）</th></tr></thead>
           <tbody>${rows
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
           <tfoot><tr><td>合计 ${rows.length} 行</td><td colspan="3"></td>
             <td>${money(balanceAmount)}</td></tr></tfoot>`;
}

export function ledgerTableHtml(data) {
  const rows = data.rows || [];
  return !rows.length
    ? emptyRow("该期间暂无流水", 8)
    : `<thead><tr><th>时间</th><th>类型</th><th>业务类型</th><th>单据号</th><th>商品</th><th>仓库</th><th>数量</th><th>金额（元）</th></tr></thead>
           <tbody>${rows
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
             <td>${qtyText(data.totals ? data.totals.qty : 0)}</td>
             <td>${money(data.totals ? data.totals.amount_cents : 0)}</td></tr></tfoot>`;
}

// 数量跨单位不可加总，合计只汇总收入 / 发出 / 其他调整金额。
export function goodsFlowTableHtml(rows) {
  const flowInAmount = rows.reduce((sum, row) => sum + row.in_amount_cents, 0);
  const flowOutAmount = rows.reduce((sum, row) => sum + row.out_amount_cents, 0);
  const flowAdjustAmount = rows.reduce((sum, row) => sum + (row.adjust_amount_cents || 0), 0);
  return !rows.length
    ? emptyRow("该期间无收发", 10)
    : `<thead><tr><th>商品编码</th><th>商品名称</th><th>仓库</th><th>期初</th><th>收入</th>
             <th>发出</th><th>期末</th><th>收入金额（元）</th><th>发出金额（元）</th>
             <th>其他调整（元）</th></tr></thead>
           <tbody>${rows
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
}

// 与 CSV / Excel 列保持一致：四个数量列 + 五个金额列；数量跨单位不可加总。
export function inventorySummaryTableHtml(rows) {
  const sumOpeningAmount = rows.reduce((sum, row) => sum + row.opening_amount_cents, 0);
  const sumInAmount = rows.reduce((sum, row) => sum + row.in_amount_cents, 0);
  const sumOutAmount = rows.reduce((sum, row) => sum + row.out_amount_cents, 0);
  const sumAdjustAmount = rows.reduce((sum, row) => sum + (row.adjust_amount_cents || 0), 0);
  const sumClosingAmount = rows.reduce((sum, row) => sum + row.closing_amount_cents, 0);
  return !rows.length
    ? emptyRow("该期间无进销存发生", 12)
    : `<thead><tr><th>商品编码</th><th>商品名称</th><th>单位</th><th>期初数量</th>
             <th>期初金额（元）</th><th>入库数量</th><th>入库金额（元）</th>
             <th>出库数量</th><th>出库金额（元）</th><th>其他调整（元）</th><th>期末数量</th>
             <th>期末金额（元）</th></tr></thead>
           <tbody>${rows
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
}

// 合计行：收入、成本、毛利汇总后重算毛利率；数量跨单位不加总。
export function profitTableHtml(rows) {
  const profitRevenue = rows.reduce((sum, row) => sum + row.revenue_cents, 0);
  const profitCost = rows.reduce((sum, row) => sum + row.cost_cents, 0);
  const profitValue = rows.reduce((sum, row) => sum + row.profit_cents, 0);
  return !rows.length
    ? emptyRow("该期间无销售", 7)
    : `<thead><tr><th>商品编码</th><th>商品名称</th><th>数量</th><th>销售收入（元）</th>
             <th>销售成本（元）</th><th>毛利（元）</th><th>毛利率</th></tr></thead>
           <tbody>${rows
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

export function otherFlowTableHtml(rows) {
  return !rows.length
    ? emptyRow("该期间无其他出入库", 5)
    : `<thead><tr><th>业务类型</th><th>入库数量</th><th>入库金额（元）</th>
             <th>出库数量</th><th>出库金额（元）</th></tr></thead>
           <tbody>${rows
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
