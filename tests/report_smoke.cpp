#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "xiaotu/db.h"
#include "xiaotu/document.h"
#include "xiaotu/gen/customer_service.h"
#include "xiaotu/gen/employee_service.h"
#include "xiaotu/gen/item_service.h"
#include "xiaotu/migrations.h"
#include "xiaotu/qty.h"
#include "xiaotu/report_service.h"
#include "xiaotu/xlsx.h"
#include "xiaotu/xlsx_reader.h"

namespace {
int failures = 0;
void check(bool condition, const std::string& label) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", label.c_str());
    ++failures;
  }
}
}  // namespace

int main() {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / ("xiaotu-report-" + std::to_string(std::rand()));
  std::filesystem::create_directories(dir);
  {
    xiaotu::Db db(dir / "app.db");
    xiaotu::migrate(db);
    xiaotu::gen::ItemService items(db);
    const auto item = items.create(xiaotu::gen::ItemRow{.code = "R001", .name = "报表商品"});
    xiaotu::gen::CustomerService customers(db);
    const auto customer = customers.create(
        xiaotu::gen::CustomerRow{.code = "RC", .name = "报表客户", .credit_limit_cents = 0});
    xiaotu::gen::EmployeeService employees(db);
    const auto person =
        employees.create(xiaotu::gen::EmployeeRow{.code = "RE", .name = "业务员甲"});
    const std::string today = xiaotu::today_text();

    xiaotu::DocumentEngine engine(db);
    xiaotu::DocInput opening;
    opening.doc_type = "OTH_IN";
    opening.warehouse_id = 1;
    opening.doc_date = today;
    opening.partner = "期初";
    opening.lines.push_back(
        xiaotu::DocLineInput{.item_id = item.id, .qty = xiaotu::qty_from_int(10), .price_cents = 1000});
    engine.save_and_approve(opening);

    auto sale = [&](const char* type, std::int64_t qty) {
      xiaotu::DocInput input;
      input.doc_type = type;
      input.warehouse_id = 1;
      input.doc_date = today;
      input.partner_id = customer.id;
      input.partner = customer.name;
      input.salesperson_id = person.id;
      input.lines.push_back(
          xiaotu::DocLineInput{.item_id = item.id, .qty = qty, .price_cents = 2000});
      return engine.save_and_approve(input);
    };
    sale("SAL_OUT", xiaotu::qty_from_int(4));
    sale("SAL_RET", xiaotu::qty_from_int(1));

    xiaotu::ReportService reports(db);
    xiaotu::ReportFilter filter;
    filter.from = today;
    filter.to = today;

    const auto by_customer = reports.sales_summary(filter, "customer");
    check(by_customer.size() == 1, "客户维度应只有 1 组");
    if (!by_customer.empty()) {
      const auto& row = by_customer.front();
      check(row.name == "报表客户" && row.orders == 2, "客户分组名称与单数应正确");
      check(row.qty == xiaotu::qty_from_int(3), "净数量应为 3");
      check(row.sales_cents == 8000 && row.return_cents == 2000 && row.net_cents == 6000,
            "销售额 80.00 / 退货 20.00 / 净额 60.00");
      check(row.cost_cents == 3000 && row.profit_cents == 3000,
            "净成本 30.00、毛利 30.00（实际 cost=" + std::to_string(row.cost_cents) +
                " profit=" + std::to_string(row.profit_cents) + "）");
    }

    const auto by_salesperson = reports.sales_summary(filter, "salesperson");
    check(by_salesperson.size() == 1 && by_salesperson.front().name == "业务员甲" &&
              by_salesperson.front().customers == 1,
          "业务员维度应带客户数 1");
    check(by_salesperson.front().profit_cents == 3000, "业务员维度毛利应一致");

    const auto detail = reports.sales_detail(filter, "customer");
    check(detail.size() == 2, "明细应有出库与退货两行");
    const xiaotu::SalesDetailRow* out_row = nullptr;
    const xiaotu::SalesDetailRow* return_row = nullptr;
    for (const auto& row : detail) {
      if (row.qty > 0) {
        out_row = &row;
      } else if (row.qty < 0) {
        return_row = &row;
      }
    }
    check(out_row != nullptr && out_row->price_cents == 2000 &&
              out_row->amount_cents == 8000 && out_row->cost_cents == 4000,
          "出库行单价为正、金额与成本正确");
    check(return_row != nullptr && return_row->qty == -xiaotu::qty_from_int(1) &&
              return_row->price_cents == 2000 && return_row->amount_cents == -2000 &&
              return_row->cost_cents == -1000,
          "退货行数量与金额为负、单价仍为正、成本按移动加权冲回");
    const auto totals = reports.sales_detail_totals(filter);
    check(totals.rows == 2 && totals.qty == xiaotu::qty_from_int(3) && totals.amount_cents == 6000 &&
              totals.cost_cents == 3000 && totals.profit_cents == 3000,
          "明细合计应按全量口径");

    const auto dashboard = reports.dashboard(today);
    check(dashboard.today_sales_cents == 8000 && dashboard.today_return_cents == 2000 &&
              dashboard.today_profit_cents == 3000,
          "看板今日销售额与毛利");
    check(dashboard.stock_value_cents == 7000,
          "库存总额应为 7 个 × 10.00 = 70.00（实际 " +
              std::to_string(dashboard.stock_value_cents) + "）");
    check(!dashboard.top_debtors.empty() && dashboard.top_debtors.front().name == "报表客户" &&
              dashboard.top_debtors.front().outstanding_cents == 6000,
          "欠款 Top N 应按未核销应收");
    check(dashboard.month_start == today.substr(0, 7) + "-01", "本月起点应为自然月 1 号");

    // 真分页：库存流水按 offset 取第二页。
    xiaotu::ReportFilter page;
    page.from = today;
    page.to = today;
    page.limit = 1;
    page.offset = 1;
    const auto ledger_page = reports.stock_ledger(page);
    const auto ledger_totals = reports.stock_ledger_totals(filter);
    check(ledger_totals.rows == 3, "库存流水共 3 条");
    check(ledger_page.size() == 1, "分页每页 1 行应只返回 1 行");

    // 流式取数：与一次性取数结果一致；回调返回 false 立即停止。
    std::vector<xiaotu::LedgerDetailRow> streamed_ledger;
    const bool ledger_completed = reports.stream_stock_ledger(
        filter, [&](const xiaotu::LedgerDetailRow& row) {
          streamed_ledger.push_back(row);
          return true;
        });
    check(ledger_completed && streamed_ledger.size() == 3,
          "流式库存流水应返回全部 3 条（实际 " + std::to_string(streamed_ledger.size()) + "）");
    std::size_t stopped_after = 0;
    reports.stream_stock_ledger(filter, [&](const xiaotu::LedgerDetailRow&) {
      ++stopped_after;
      return false;
    });
    check(stopped_after == 1, "流式回调返回 false 应立即停止");

    std::vector<xiaotu::SalesDetailRow> streamed_detail;
    const bool detail_completed = reports.stream_sales_detail(
        filter, [&](const xiaotu::SalesDetailRow& row) {
          streamed_detail.push_back(row);
          return true;
        });
    check(detail_completed && streamed_detail.size() == detail.size(),
          "流式销售明细行数应与一次性取数一致");

    // 采购明细：退货按负数冲减；合计按全量算，不受分页 offset 影响。
    auto purchase = [&](const char* type, std::int64_t qty) {
      xiaotu::DocInput input;
      input.doc_type = type;
      input.warehouse_id = 1;
      input.doc_date = today;
      input.partner = "报表供应商";
      input.lines.push_back(xiaotu::DocLineInput{
          .item_id = item.id, .qty = qty, .price_cents = 500});
      return engine.save_and_approve(input);
    };
    purchase("PUR_IN", xiaotu::qty_from_int(10));
    purchase("PUR_RET", xiaotu::qty_from_int(2));

    xiaotu::ReportFilter purchase_filter;
    purchase_filter.from = today;
    purchase_filter.to = today;
    const auto purchase_rows = reports.purchase_detail(purchase_filter);
    check(purchase_rows.size() == 2, "采购明细应有入库与退货两行");
    const auto purchase_totals = reports.purchase_detail_totals(purchase_filter);
    check(purchase_totals.rows == 2 && purchase_totals.qty == xiaotu::qty_from_int(8) &&
              purchase_totals.amount_cents == 4000,
          "采购合计应为净数量 8、净金额 40.00");

    std::vector<xiaotu::PurchaseDetailRow> streamed_purchase;
    const bool purchase_completed = reports.stream_purchase_detail(
        purchase_filter, [&](const xiaotu::PurchaseDetailRow& row) {
          streamed_purchase.push_back(row);
          return true;
        });
    check(purchase_completed && streamed_purchase.size() == purchase_rows.size(),
          "流式采购明细行数应与一次性取数一致");

    xiaotu::ReportFilter purchase_page;
    purchase_page.from = today;
    purchase_page.to = today;
    purchase_page.limit = 1;
    purchase_page.offset = 1;
    check(reports.purchase_detail(purchase_page).size() == 1, "采购明细分页应只返回 1 行");
    check(reports.purchase_detail_totals(purchase_page).rows == 2,
          "采购合计按全量计算，不受分页 offset 影响");

    // 流式 xlsx：分批写入 3000 行，生成的文件应能被读取器完整读出。
    xiaotu::XlsxStreamWriter writer("流式导出");
    check(writer.ok(), "流式 xlsx 写入器应能创建临时文件");
    writer.add_header_row({"序号", "金额（元）"});
    for (int i = 0; i < 3000; ++i) {
      writer.add_row({xiaotu::XlsxCell::text("行" + std::to_string(i)),
                      xiaotu::XlsxCell::money(12345)});
    }
    const std::string xlsx_path = writer.finish();
    check(!xlsx_path.empty(), "流式 xlsx 应组装出文件");
    if (!xlsx_path.empty()) {
      std::ifstream stream(xlsx_path, std::ios::binary);
      const std::string bytes{std::istreambuf_iterator<char>(stream),
                              std::istreambuf_iterator<char>()};
      stream.close();
      const auto table = xiaotu::read_first_sheet(bytes);
      check(table.header == std::vector<std::string>{"序号", "金额（元）"},
            "流式 xlsx 表头应完整");
      check(table.rows.size() == 3000, "流式 xlsx 应保留全部 3000 行");
      if (table.rows.size() == 3000) {
        const auto& last = table.rows.back();
        check(last.cells.size() == 2 && last.cells[0] == "行2999" && last.cells[1] == "123.45",
              "流式 xlsx 末行内容应正确");
      }
      std::error_code remove_error;
      std::filesystem::remove(xlsx_path, remove_error);
    }
  }
  std::error_code error;
  std::filesystem::remove_all(dir, error);
  if (failures == 0) std::printf("report_smoke passed\n");
  return failures == 0 ? 0 : 1;
}
