/* app/report_tables_trade.js：资金与往来类报表表格的纯渲染函数（输入数据，输出 HTML）。
 * 包含资金流水、账户余额、采购明细、采购汇总与应收应付账龄。
 */
import { emptyRow, escapeHtml, money, qtyIsNegative, qtyText } from "./core.js";

export function fundLedgerTableHtml(rows) {
  return !rows.length
    ? emptyRow("该账户暂无资金流水", 10)
    : `<thead><tr><th>日期</th><th>类型</th><th>单号</th><th>往来单位</th>
             <th>结算方式</th><th>经办人</th><th>摘要</th><th>收入（元）</th>
             <th>支出（元）</th><th>余额（元）</th></tr></thead>
           <tbody>${rows
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
}

export function fundBalancesTableHtml(rows) {
  return !rows.length
    ? emptyRow("暂无资金账户", 7)
    : `<thead><tr><th>编码</th><th>名称</th><th>类型</th><th>期初（元）</th>
             <th>累计收入（元）</th><th>累计支出（元）</th><th>余额（元）</th></tr></thead>
           <tbody>${rows
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
}

export function purchaseDetailTableHtml(data) {
  const rows = data.rows || [];
  return !rows.length
    ? emptyRow("该期间无采购", 9)
    : `<thead><tr><th>日期</th><th>单号</th><th>类型</th><th>供应商</th><th>商品</th>
             <th>规格</th><th>数量</th><th>单价（元）</th><th>金额（元）</th></tr></thead>
           <tbody>${rows
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
             <td>${qtyText(data.totals ? data.totals.qty : 0)}</td>
             <td></td>
             <td>${money(data.totals ? data.totals.amount_cents : 0)}</td>
           </tr></tfoot>`;
}

export function purchaseSummaryTableHtml(data, byItem) {
  const rows = data.rows || [];
  const totals = data.totals || {};
  // 入库均价只计 PUR_IN；无入库行时显示「—」，不显示 0。
  const inboundAvgText = (row) => (row.has_inbound ? money(row.avg_price_cents) : "—");
  // 净数量按入库减退货如实显示，负数额外标注「净退货」。
  const netQtyCell = (row) =>
    `<td class="${qtyIsNegative(row.qty) ? "negative" : ""}">${qtyText(row.qty)}${
      row.qty < 0 ? "（净退货）" : ""
    }</td>`;
  return !rows.length
    ? emptyRow("该期间无采购", byItem ? 10 : 7)
    : byItem
      ? `<thead><tr><th>商品编码</th><th>商品名称</th><th>规格</th><th>单位</th>
               <th>入库数量</th><th>入库金额（元）</th><th>退货额（元）</th>
               <th>净数量</th><th>净金额（元）</th><th>入库均价（元）</th></tr></thead>
             <tbody>${rows
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
             <td>${qtyText(totals.in_qty || 0)}</td>
             <td>${money(totals.in_amount_cents || 0)}</td>
             <td>${money(totals.return_amount_cents || 0)}</td>
             <td>${qtyText(totals.qty || 0)}</td>
             <td>${money(totals.amount_cents || 0)}</td>
             <td>${inboundAvgText(totals)}</td>
           </tr></tfoot>`
      : `<thead><tr><th>供应商</th><th>入库数量</th><th>入库金额（元）</th><th>退货额（元）</th>
               <th>净数量</th><th>净金额（元）</th><th>入库均价（元）</th></tr></thead>
             <tbody>${rows
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
             <td>${qtyText(totals.in_qty || 0)}</td>
             <td>${money(totals.in_amount_cents || 0)}</td>
             <td>${money(totals.return_amount_cents || 0)}</td>
             <td>${qtyText(totals.qty || 0)}</td>
             <td>${money(totals.amount_cents || 0)}</td>
             <td>${inboundAvgText(totals)}</td>
           </tr></tfoot>`;
}

export function agingTableHtml(rows) {
  const agingTotal = rows.reduce((sum, row) => sum + row.total_cents, 0);
  const agingNotDue = rows.reduce((sum, row) => sum + row.not_due_cents, 0);
  const agingD1 = rows.reduce((sum, row) => sum + row.d1_30_cents, 0);
  const agingD31 = rows.reduce((sum, row) => sum + row.d31_60_cents, 0);
  const agingD61 = rows.reduce((sum, row) => sum + row.d61_90_cents, 0);
  const agingD90 = rows.reduce((sum, row) => sum + row.d90_plus_cents, 0);
  return !rows.length
    ? emptyRow("暂无未核销余额", 7)
    : `<thead><tr><th>往来单位</th><th>合计（元）</th><th>未到期</th><th>1-30 天</th>
             <th>31-60 天</th><th>61-90 天</th><th>90 天以上</th></tr></thead>
           <tbody>${rows
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
}
