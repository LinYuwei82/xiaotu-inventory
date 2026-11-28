#include <httplib.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "xiaotu/edition.h"
#include "xiaotu/http_guard.h"
#include "xiaotu/http_json.h"
#include "xiaotu/qty.h"
#include "xiaotu/report_service.h"
#include "xiaotu/services.h"

#include "report_common.h"

namespace xiaotu {
namespace {

using namespace report_common;

nlohmann::json profit_to_json(const SalesProfitRow& row) {
  return nlohmann::json{
      {"item_id", row.item_id},
      {"item_code", row.item_code},
      {"item_name", row.item_name},
      {"unit", row.unit},
      {"qty", qty_text(row.qty)},
      {"revenue_cents", row.revenue_cents},
      {"cost_cents", row.cost_cents},
      {"profit_cents", row.profit_cents},
  };
}

}  // namespace

void register_report_sales_profit_routes(httplib::Server& server, Services& services) {
  server.Get("/api/reports/sales-profit",
             guard("finance.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     EditionPolicy::instance().require("report.profit");
                     const ReportFilter filter = parse_filter(req);
                     const auto rows = services.reports.sales_profit(filter);
                     const auto settings = services.settings.load();
                     // 数量跨单位不可加总；合计行汇总收入、成本、毛利并重算毛利率。
                     std::int64_t profit_revenue = 0;
                     std::int64_t profit_cost = 0;
                     std::int64_t profit_total = 0;
                     for (const auto& row : rows) {
                       profit_revenue += row.revenue_cents;
                       profit_cost += row.cost_cents;
                       profit_total += row.profit_cents;
                     }
                     if (csv_requested(req)) {
                       std::string csv;
                       csv_preamble(csv, "销售毛利",
                                    period_label(filter) + "；" +
                                        dimension_label(filter.item_id, "商品") +
                                        "；客户 " +
                                        (filter.customer_id == 0
                                             ? std::string("全部")
                                             : "ID " + std::to_string(filter.customer_id)) +
                                        "；收入取单据金额，成本取审核写入的库存流水，"
                                        "已反审核单据不计入；成本口径 " +
                                        cost_basis_label(settings));
                       csv_row(csv, {"商品编码", "商品名称", "单位", "销售数量",
                                     "销售收入（元）", "销售成本（元）", "毛利（元）", "毛利率"});
                       for (const auto& row : rows) {
                         csv_row(csv, {row.item_code, row.item_name, row.unit,
                                       qty_text(row.qty), yuan(row.revenue_cents),
                                       yuan(row.cost_cents), yuan(row.profit_cents),
                                       margin_text(row.revenue_cents, row.profit_cents)});
                       }
                       csv_row(csv, {"合计", "", "", "", yuan(profit_revenue),
                                     yuan(profit_cost), yuan(profit_total),
                                     margin_text(profit_revenue, profit_total)});
                       send_csv(res, "sales-profit-" + date_stamp() + ".csv", csv);
                       return;
                     }
                     if (xlsx_requested(req)) {
                       XlsxSheet sheet("销售毛利");
                       xlsx_preamble(sheet, "销售毛利",
                                     period_label(filter) + "；" +
                                         dimension_label(filter.item_id, "商品") + "；客户 " +
                                         (filter.customer_id == 0
                                              ? std::string("全部")
                                              : "ID " + std::to_string(filter.customer_id)) +
                                         "；收入取单据金额，成本取审核写入的库存流水，"
                                         "已反审核单据不计入；成本口径 " +
                                         cost_basis_label(settings));
                       sheet.add_header_row({"商品编码", "商品名称", "单位", "销售数量",
                                             "销售收入（元）", "销售成本（元）", "毛利（元）",
                                             "毛利率"});
                       for (const auto& row : rows) {
                         sheet.add_row({XlsxCell::text(row.item_code),
                                        XlsxCell::text(row.item_name),
                                        XlsxCell::text(row.unit),
                                        XlsxCell::text(qty_text(row.qty)),
                                        XlsxCell::money(row.revenue_cents),
                                        XlsxCell::money(row.cost_cents),
                                        XlsxCell::money(row.profit_cents),
                                        XlsxCell::text(
                                            margin_text(row.revenue_cents, row.profit_cents))});
                       }
                       sheet.add_row({XlsxCell::text("合计"), XlsxCell::text(""),
                                      XlsxCell::text(""), XlsxCell::text(""),
                                      XlsxCell::money(profit_revenue),
                                      XlsxCell::money(profit_cost),
                                      XlsxCell::money(profit_total),
                                      XlsxCell::text(margin_text(profit_revenue, profit_total))});
                       send_xlsx(res, "sales-profit-" + date_stamp() + ".xlsx", sheet.build());
                       return;
                     }
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : rows) {
                       auto item = profit_to_json(row);
                       // 每行标注当前成本口径与切换日，避免拿切换前后的毛利直接比较。
                       item["cost_basis"] = settings.cost_basis;
                       item["cost_basis_changed_at"] = settings.cost_basis_changed_at;
                       list.push_back(item);
                     }
                     json_ok(res, list);
                   }));


}

}  // namespace xiaotu
