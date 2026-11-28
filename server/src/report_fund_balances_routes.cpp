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

void register_report_fund_balances_routes(httplib::Server& server, Services& services) {
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
