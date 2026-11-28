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

nlohmann::json sales_summary_to_json(const SalesSummaryRow& row, std::int64_t total_net,
                                     bool by_salesperson) {
  nlohmann::json json{{"group_id", row.group_id},
                      {"name", row.name},
                      {"orders", row.orders},
                      {"qty", qty_text(row.qty)},
                      {"sales_cents", row.sales_cents},
                      {"return_cents", row.return_cents},
                      {"net_cents", row.net_cents},
                      {"cost_cents", row.cost_cents},
                      {"profit_cents", row.profit_cents},
                      {"margin", margin_text(row.net_cents, row.profit_cents)},
                      {"ratio", margin_text(total_net, row.net_cents)}};
  if (by_salesperson) json["customers"] = row.customers;
  return json;
}

}  // namespace

void register_report_sales_summary_routes(httplib::Server& server, Services& services) {
  server.Get("/api/reports/sales-summary",
             guard("finance.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const ReportFilter filter = parse_filter(req);
                     const bool by_salesperson =
                         req.has_param("dimension") &&
                         req.get_param_value("dimension") == "salesperson";
                     const auto rows = services.reports.sales_summary(
                         filter, by_salesperson ? "salesperson" : "customer");
                     std::int64_t total_orders = 0;
                     std::int64_t total_qty = 0;
                     std::int64_t total_sales = 0;
                     std::int64_t total_return = 0;
                     std::int64_t total_net = 0;
                     std::int64_t total_cost = 0;
                     std::int64_t total_profit = 0;
                     for (const auto& row : rows) {
                       total_orders += row.orders;
                       total_qty += row.qty;
                       total_sales += row.sales_cents;
                       total_return += row.return_cents;
                       total_net += row.net_cents;
                       total_cost += row.cost_cents;
                       total_profit += row.profit_cents;
                     }
                     const std::string title =
                         by_salesperson ? "销售报表（按业务员）" : "销售报表（按客户）";
                     const std::string subtitle =
                         period_label(filter) + "；只计已审核的销售出库与销售退货；" +
                         "退货按负数冲减；毛利按出库时的移动加权成本";
                     if (csv_requested(req)) {
                       std::string csv;
                       csv_preamble(csv, title, subtitle);
                       if (by_salesperson) {
                         csv_row(csv, {"业务员", "单数", "客户数", "销售数量", "销售额（元）",
                                       "退货额（元）", "净销售额（元）", "成本（元）",
                                       "毛利（元）", "毛利率", "销售额占比"});
                       } else {
                         csv_row(csv, {"客户", "单数", "销售数量", "销售额（元）",
                                       "退货额（元）", "净销售额（元）", "成本（元）",
                                       "毛利（元）", "毛利率", "销售额占比"});
                       }
                       for (const auto& row : rows) {
                         const std::string ratio = margin_text(total_net, row.net_cents);
                         if (by_salesperson) {
                           csv_row(csv, {row.name, std::to_string(row.orders),
                                         std::to_string(row.customers), qty_text(row.qty),
                                         yuan(row.sales_cents), yuan(row.return_cents),
                                         yuan(row.net_cents), yuan(row.cost_cents),
                                         yuan(row.profit_cents),
                                         margin_text(row.net_cents, row.profit_cents), ratio});
                         } else {
                           csv_row(csv, {row.name, std::to_string(row.orders),
                                         qty_text(row.qty), yuan(row.sales_cents),
                                         yuan(row.return_cents), yuan(row.net_cents),
                                         yuan(row.cost_cents), yuan(row.profit_cents),
                                         margin_text(row.net_cents, row.profit_cents), ratio});
                         }
                       }
                       if (by_salesperson) {
                         csv_row(csv, {"合计", std::to_string(total_orders), "",
                                       qty_text(total_qty), yuan(total_sales),
                                       yuan(total_return), yuan(total_net), yuan(total_cost),
                                       yuan(total_profit),
                                       margin_text(total_net, total_profit), "100.00%"});
                       } else {
                         csv_row(csv, {"合计", std::to_string(total_orders),
                                       qty_text(total_qty), yuan(total_sales),
                                       yuan(total_return), yuan(total_net), yuan(total_cost),
                                       yuan(total_profit),
                                       margin_text(total_net, total_profit), "100.00%"});
                       }
                       send_csv(res, std::string(by_salesperson ? "sales-salesperson-"
                                                                : "sales-customer-") +
                                         date_stamp() + ".csv",
                                csv);
                       return;
                     }
                     if (xlsx_requested(req)) {
                       XlsxSheet sheet(title);
                       xlsx_preamble(sheet, title, subtitle);
                       if (by_salesperson) {
                         sheet.add_header_row({"业务员", "单数", "客户数", "销售数量",
                                               "销售额（元）", "退货额（元）", "净销售额（元）",
                                               "成本（元）", "毛利（元）", "毛利率",
                                               "销售额占比"});
                       } else {
                         sheet.add_header_row({"客户", "单数", "销售数量", "销售额（元）",
                                               "退货额（元）", "净销售额（元）", "成本（元）",
                                               "毛利（元）", "毛利率", "销售额占比"});
                       }
                       for (const auto& row : rows) {
                         const std::string ratio = margin_text(total_net, row.net_cents);
                         std::vector<XlsxCell> cells;
                         cells.push_back(XlsxCell::text(row.name));
                         cells.push_back(XlsxCell::integer(row.orders));
                         if (by_salesperson) {
                           cells.push_back(XlsxCell::integer(row.customers));
                         }
                         cells.push_back(XlsxCell::text(qty_text(row.qty)));
                         cells.push_back(XlsxCell::money(row.sales_cents));
                         cells.push_back(XlsxCell::money(row.return_cents));
                         cells.push_back(XlsxCell::money(row.net_cents));
                         cells.push_back(XlsxCell::money(row.cost_cents));
                         cells.push_back(XlsxCell::money(row.profit_cents));
                         cells.push_back(XlsxCell::text(
                             margin_text(row.net_cents, row.profit_cents)));
                         cells.push_back(XlsxCell::text(ratio));
                         sheet.add_row(cells);
                       }
                       std::vector<XlsxCell> total_cells;
                       total_cells.push_back(XlsxCell::text("合计"));
                       total_cells.push_back(XlsxCell::integer(total_orders));
                       if (by_salesperson) total_cells.push_back(XlsxCell::text(""));
                       total_cells.push_back(XlsxCell::text(qty_text(total_qty)));
                       total_cells.push_back(XlsxCell::money(total_sales));
                       total_cells.push_back(XlsxCell::money(total_return));
                       total_cells.push_back(XlsxCell::money(total_net));
                       total_cells.push_back(XlsxCell::money(total_cost));
                       total_cells.push_back(XlsxCell::money(total_profit));
                       total_cells.push_back(
                           XlsxCell::text(margin_text(total_net, total_profit)));
                       total_cells.push_back(XlsxCell::text("100.00%"));
                       sheet.add_row(total_cells);
                       send_xlsx(res, std::string(by_salesperson ? "sales-salesperson-"
                                                                 : "sales-customer-") +
                                         date_stamp() + ".xlsx",
                                sheet.build());
                       return;
                     }
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : rows) {
                       list.push_back(sales_summary_to_json(row, total_net, by_salesperson));
                     }
                     nlohmann::json totals{{"orders", total_orders},
                                           {"qty", qty_text(total_qty)},
                                           {"sales_cents", total_sales},
                                           {"return_cents", total_return},
                                           {"net_cents", total_net},
                                           {"cost_cents", total_cost},
                                           {"profit_cents", total_profit},
                                           {"margin", margin_text(total_net, total_profit)}};
                     json_ok(res, nlohmann::json{{"rows", list}, {"totals", totals}});
                   }));
}

}  // namespace xiaotu
