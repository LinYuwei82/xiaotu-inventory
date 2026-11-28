/* app/report_actions.js：报表页导出 CSV / Excel 与打印 / PDF 按钮的事件绑定。
 * 路径拼装全部委托给传入的查询函数，保持与页面筛选一致。
 */
import { downloadCsv, downloadXlsx, showNotice, withQuery } from "./core.js";
import { exportPdfTarget, printTarget } from "./print.js";

export function wireReportDownloads(mount, q, profitAllowed) {
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

  wireExport("export-balance", () => "/api/reports/stock-balance" + q.balance() + "&format=csv");
  wireExport("export-ledger", () => "/api/reports/stock-ledger" + q.ledger() + "&format=csv");
  wireExport("export-flows", () => "/api/reports/goods-flow" + q.period() + "&format=csv");
  wireExport("export-summary", () => "/api/reports/inventory-summary" + q.period() + "&format=csv");
  if (profitAllowed) {
    wireExport("export-profit", () => "/api/reports/sales-profit" + q.period() + "&format=csv");
  }
  wireExport(
    "export-purchase-detail",
    () => "/api/reports/purchase-detail" + q.purchase() + "&format=csv"
  );
  wireExport(
    "export-purchase-summary",
    () => "/api/reports/purchase-summary" + q.purchase() + "&by=" + q.purchaseBy() + "&format=csv"
  );
  wireExport(
    "export-other-flow",
    () => "/api/reports/other-flow-summary" + q.otherFlow() + "&format=csv"
  );
  wireExport("export-aging", () => "/api/reports/ar-ap-aging?direction=" + q.direction() + "&format=csv");
  wireExport(
    "export-fund",
    () =>
      "/api/reports/fund-ledger" +
      q.period() +
      "&account_id=" +
      encodeURIComponent(q.fundAccount() || "0") +
      "&format=csv"
  );
  wireExport(
    "export-fund-balances",
    () => "/api/reports/fund-balances?as_of=" + encodeURIComponent(q.to()) + "&format=csv"
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

  wireExcel("excel-balance", () => "/api/reports/stock-balance" + q.balance() + "&format=xlsx");
  wireExcel("excel-ledger", () => "/api/reports/stock-ledger" + q.ledger() + "&format=xlsx");
  wireExcel("excel-flows", () => "/api/reports/goods-flow" + q.period() + "&format=xlsx");
  wireExcel("excel-summary", () => "/api/reports/inventory-summary" + q.period() + "&format=xlsx");
  if (profitAllowed) {
    wireExcel("excel-profit", () => "/api/reports/sales-profit" + q.period() + "&format=xlsx");
  }
  wireExcel(
    "excel-purchase-detail",
    () => "/api/reports/purchase-detail" + q.purchase() + "&format=xlsx"
  );
  wireExcel(
    "excel-purchase-summary",
    () => "/api/reports/purchase-summary" + q.purchase() + "&by=" + q.purchaseBy() + "&format=xlsx"
  );
  wireExcel(
    "excel-other-flow",
    () => "/api/reports/other-flow-summary" + q.otherFlow() + "&format=xlsx"
  );
  wireExcel(
    "excel-aging",
    () => "/api/reports/ar-ap-aging?direction=" + q.direction() + "&format=xlsx"
  );
  wireExcel(
    "excel-fund",
    () =>
      "/api/reports/fund-ledger" +
      q.period() +
      "&account_id=" +
      encodeURIComponent(q.fundAccount() || "0") +
      "&format=xlsx"
  );
  wireExcel(
    "excel-fund-balances",
    () => "/api/reports/fund-balances?as_of=" + encodeURIComponent(q.to()) + "&format=xlsx"
  );
}

export function wireReportPrints(mount, q, profitAllowed) {
  function wirePrint(role, pathFn) {
    mount.querySelector(`[data-role="${role}"]`).addEventListener("click", () => printTarget(pathFn()));
    const pdf = mount.querySelector(`[data-role="${role.replace(/^print-/, "pdf-")}"]`);
    if (pdf) pdf.addEventListener("click", () => exportPdfTarget(pathFn(), null));
  }

  wirePrint(
    "print-balance",
    () => withQuery("/api/print/render?target_type=REPORT&target_key=stock-balance", q.balance())
  );
  wirePrint(
    "print-ledger",
    () =>
      withQuery(
        "/api/print/render?target_type=REPORT&target_key=stock-ledger",
        q.ledger() + "&limit=100000"
      )
  );
  wirePrint(
    "print-flows",
    () => withQuery("/api/print/render?target_type=REPORT&target_key=goods-flow", q.period())
  );
  wirePrint(
    "print-summary",
    () =>
      withQuery("/api/print/render?target_type=REPORT&target_key=inventory-summary", q.period())
  );
  if (profitAllowed) {
    wirePrint(
      "print-profit",
      () => withQuery("/api/print/render?target_type=REPORT&target_key=sales-profit", q.period())
    );
  }
  wirePrint(
    "print-purchase-detail",
    () =>
      withQuery(
        "/api/print/render?target_type=REPORT&target_key=purchase-detail",
        q.purchase() + "&limit=100000"
      )
  );
  wirePrint(
    "print-purchase-summary",
    () =>
      withQuery(
        "/api/print/render?target_type=REPORT&target_key=purchase-summary-" + q.purchaseBy(),
        q.purchase()
      )
  );
  wirePrint(
    "print-other-flow",
    () =>
      withQuery(
        "/api/print/render?target_type=REPORT&target_key=other-flow-summary",
        q.otherFlow()
      )
  );
  wirePrint(
    "print-aging",
    () => "/api/print/render?target_type=REPORT&target_key=ar-ap-aging&direction=" + q.direction()
  );
  wirePrint(
    "print-fund",
    () =>
      withQuery(
        "/api/print/render?target_type=REPORT&target_key=fund-ledger",
        q.period() + "&account_id=" + encodeURIComponent(q.fundAccount() || "0")
      )
  );
}
