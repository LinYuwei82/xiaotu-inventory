#include <httplib.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

#include "xiaotu/error.h"
#include "xiaotu/http_guard.h"
#include "xiaotu/http_json.h"
#include "xiaotu/qty.h"
#include "xiaotu/report_service.h"
#include "xiaotu/services.h"

#include "report_common.h"

namespace xiaotu {

using namespace report_common;

void register_report_fund_routes(httplib::Server& server, Services& services) {
  // 资金流水（现金日记账）：包含期初行与逐行余额，按账户 + 日期筛选。
  server.Get("/api/reports/fund-ledger",
             guard("finance.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     // account_id：数字为指定账户，0 为历史「未指定账户」，all 为全部账户。
                     std::int64_t account_id = 0;
                     if (req.has_param("account_id")) {
                       const std::string value = req.get_param_value("account_id");
                       account_id = value == "all" ? -1 : parse_id(value);
                     }
                     const std::string from =
                         req.has_param("from") ? req.get_param_value("from") : "";
                     const std::string to =
                         req.has_param("to") ? req.get_param_value("to") : "";
                     const std::string format =
                         req.has_param("format") ? req.get_param_value("format") : "json";
                     const auto rows = services.settlements.ledger(account_id, from, to);
                     const auto direction_text = [](const FundLedgerRow& row) -> std::string {
                       if (row.opening) return "期初";
                       // 撤销行金额为负，列内只显示绝对值，靠类型区分「撤销收款/付款」。
                       const bool reversed = row.note.rfind("撤销", 0) == 0;
                       if (row.direction == 1) return reversed ? "撤销收款" : "收款";
                       return reversed ? "撤销付款" : "付款";
                     };

                     if (format == "csv") {
                       std::string csv = "\xEF\xBB\xBF";
                       csv_row(csv, {"日期", "单号", "类型", "往来单位", "结算方式", "经办人",
                                     "摘要", "收入（元）", "支出（元）", "余额（元）"});
                       for (const auto& row : rows) {
                         csv_row(csv, {row.settled_on, row.settle_no, direction_text(row),
                                       row.partner_name, method_label(row.method),
                                       row.operator_name, row.note,
                                       row.opening ? std::string{}
                                                   : yuan(row.income_cents),
                                       row.opening ? std::string{}
                                                   : yuan(row.expense_cents),
                                       yuan(row.balance_cents)});
                       }
                       send_csv(res, "fund-ledger-" + date_stamp() + ".csv", csv);
                       return;
                     }

                     if (format == "xlsx") {
                       XlsxSheet sheet("资金流水");
                       sheet.add_header_row({"日期", "单号", "类型", "往来单位", "结算方式", "经办人",
                                             "摘要", "收入（元）", "支出（元）", "余额（元）"});
                       for (const auto& row : rows) {
                         sheet.add_row(
                             {XlsxCell::text(row.settled_on),
                              XlsxCell::text(row.settle_no),
                              XlsxCell::text(direction_text(row)),
                              XlsxCell::text(row.partner_name),
                              XlsxCell::text(method_label(row.method)),
                              XlsxCell::text(row.operator_name),
                              XlsxCell::text(row.note),
                              row.opening ? XlsxCell::text("") : XlsxCell::money(row.income_cents),
                              row.opening ? XlsxCell::text("") : XlsxCell::money(row.expense_cents),
                              XlsxCell::money(row.balance_cents)});
                       }
                       send_xlsx(res, "fund-ledger-" + date_stamp() + ".xlsx", sheet.build());
                       return;
                     }

                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : rows) {
                       list.push_back({
                           {"opening", row.opening},
                           {"settled_on", row.settled_on},
                           {"settle_no", row.settle_no},
                           {"direction", row.opening ? 0 : row.direction},
                           {"direction_text", direction_text(row)},
                           {"partner_name", row.partner_name},
                           {"method", row.method},
                           {"method_text", method_label(row.method)},
                           {"operator", row.operator_name},
                           {"note", row.note},
                           {"income_cents", row.income_cents},
                           {"expense_cents", row.expense_cents},
                           {"balance_cents", row.balance_cents},
                       });
                     }
                     json_ok(res, list);
                   }));

  // 账户余额一览：期初 + 累计收入 − 累计支出。
  server.Get("/api/reports/fund-balances",
             guard("finance.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const std::string as_of =
                         req.has_param("as_of") ? req.get_param_value("as_of") : "";
                     const auto rows = services.settlements.account_balances(as_of);
                     const std::string scope =
                         "截至 " + (as_of.empty() ? std::string("今天") : as_of) +
                         "；期初 + 累计收入 − 累计支出";
                     if (csv_requested(req)) {
                       std::string csv;
                       csv_preamble(csv, "账户余额一览", scope);
                       csv_row(csv, {"账户编码", "账户名称", "类型", "期初（元）",
                                     "累计收入（元）", "累计支出（元）", "余额（元）"});
                       for (const auto& row : rows) {
                         csv_row(csv, {row.code, row.name, method_label(row.kind),
                                       yuan(row.opening_cents), yuan(row.inflow_cents),
                                       yuan(row.outflow_cents), yuan(row.balance_cents)});
                       }
                       send_csv(res, "fund-balances-" + date_stamp() + ".csv", csv);
                       return;
                     }
                     if (xlsx_requested(req)) {
                       XlsxSheet sheet("账户余额一览");
                       xlsx_preamble(sheet, "账户余额一览", scope);
                       sheet.add_header_row({"账户编码", "账户名称", "类型", "期初（元）",
                                             "累计收入（元）", "累计支出（元）", "余额（元）"});
                       for (const auto& row : rows) {
                         sheet.add_row({XlsxCell::text(row.code), XlsxCell::text(row.name),
                                        XlsxCell::text(method_label(row.kind)),
                                        XlsxCell::money(row.opening_cents),
                                        XlsxCell::money(row.inflow_cents),
                                        XlsxCell::money(row.outflow_cents),
                                        XlsxCell::money(row.balance_cents)});
                       }
                       send_xlsx(res, "fund-balances-" + date_stamp() + ".xlsx", sheet.build());
                       return;
                     }
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : rows) {
                       list.push_back({
                           {"id", row.id},
                           {"code", row.code},
                           {"name", row.name},
                           {"kind", row.kind},
                           {"kind_text", method_label(row.kind)},
                           {"opening_cents", row.opening_cents},
                           {"inflow_cents", row.inflow_cents},
                           {"outflow_cents", row.outflow_cents},
                           {"balance_cents", row.balance_cents},
                       });
                     }
                     json_ok(res, list);
                   }));

}

}  // namespace xiaotu
