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

void register_report_dashboard_routes(httplib::Server& server, Services& services) {
  server.Get("/api/reports/dashboard",
             guard("finance.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     EditionPolicy::instance().require("report.dashboard");
                     const std::string today = req.has_param("today")
                                                   ? req.get_param_value("today")
                                                   : today_text();
                     const auto result = services.reports.dashboard(today);
                     nlohmann::json debtors = nlohmann::json::array();
                     for (const auto& debtor : result.top_debtors) {
                       debtors.push_back(nlohmann::json{{"partner_id", debtor.partner_id},
                                                        {"name", debtor.name},
                                                        {"outstanding_cents",
                                                         debtor.outstanding_cents}});
                     }
                     json_ok(res, nlohmann::json{
                                     {"as_of", result.today},
                                     {"month_start", result.month_start},
                                     {"today",
                                      {{"sales_cents", result.today_sales_cents},
                                       {"return_cents", result.today_return_cents},
                                       {"net_cents", result.today_sales_cents -
                                                         result.today_return_cents},
                                       {"cost_cents", result.today_cost_cents},
                                       {"profit_cents", result.today_profit_cents}}},
                                     {"month",
                                      {{"sales_cents", result.month_sales_cents},
                                       {"return_cents", result.month_return_cents},
                                       {"net_cents", result.month_sales_cents -
                                                         result.month_return_cents},
                                       {"cost_cents", result.month_cost_cents},
                                       {"profit_cents", result.month_profit_cents}}},
                                     {"stock_value_cents", result.stock_value_cents},
                                     {"top_debtors", debtors}});
                   }));
}

}  // namespace xiaotu
