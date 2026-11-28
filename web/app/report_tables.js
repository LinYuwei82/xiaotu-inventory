/* app/report_tables.js：报表页骨架与各报表表格的纯渲染函数（输入数据，输出 HTML）。
 */
import { bizCategoryText, bizTypeText, emptyRow, escapeHtml, money, qtyIsNegative, qtyText } from "./core.js";

// 页面骨架：筛选区、各报表卡片与导出 / 打印按钮。
export function reportPageHtml(today, profitAllowed) {
  return `
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
}

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
