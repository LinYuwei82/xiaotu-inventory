/* app/report_tables.js：报表页骨架（筛选区、各报表卡片与导出 / 打印按钮）。
 * 各表格渲染函数按类别拆在 report_tables_stock.js 与 report_tables_trade.js。
 */
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
