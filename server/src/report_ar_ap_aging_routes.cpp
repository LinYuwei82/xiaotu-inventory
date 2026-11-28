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
namespace {

using namespace report_common;

nlohmann::json aging_to_json(const AgingRow& row) {
  return nlohmann::json{
      {"partner_key", row.partner_key},
      {"partner_name", row.partner_name},
      {"direction", row.direction},
      {"total_cents", row.total_cents},
      {"not_due_cents", row.not_due_cents},
      {"d1_30_cents", row.d1_30_cents},
      {"d31_60_cents", row.d31_60_cents},
      {"d61_90_cents", row.d61_90_cents},
      {"d90_plus_cents", row.d90_plus_cents},
  };
}

}  // namespace

void register_report_aging_routes(httplib::Server& server, Services& services) {
  server.Get("/api/reports/ar-ap-aging",
             guard("finance.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     int direction = 0;
                     if (req.has_param("direction")) {
                       const std::string value = req.get_param_value("direction");
                       direction = value == "-1" || value == "AP" ? -1 : 1;
                     }
                     const std::string as_of = req.has_param("as_of")
                                                   ? req.get_param_value("as_of")
                                                   : today_text();
                     const auto rows = services.reports.ar_ap_aging(direction, as_of);
                     // 合计行按当前截止日的全量分组求和。
                     std::int64_t aging_total = 0;
                     std::int64_t aging_not_due = 0;
                     std::int64_t aging_d1_30 = 0;
                     std::int64_t aging_d31_60 = 0;
                     std::int64_t aging_d61_90 = 0;
                     std::int64_t aging_d90_plus = 0;
                     for (const auto& row : rows) {
                       aging_total += row.total_cents;
                       aging_not_due += row.not_due_cents;
                       aging_d1_30 += row.d1_30_cents;
                       aging_d31_60 += row.d31_60_cents;
                       aging_d61_90 += row.d61_90_cents;
                       aging_d90_plus += row.d90_plus_cents;
                     }
                     res.set_header(kArApScopeHeader, kArApScope);
                     if (csv_requested(req)) {
                       std::string csv;
                       csv_preamble(csv, "应收应付余额与账龄",
                                    std::string("方向 ") + (direction == -1 ? "应付" : "应收") +
                                        "；截止日 " + as_of +
                                        "；仅统计 v7 迁移后审核的单据"
                                        "；按截止日时点还原：业务日期晚于截止日不计入，"
                                        "核销按核销日期还原");
                       csv_row(csv, {"往来单位", "方向", "合计（元）", "未到期（元）", "1-30 天（元）",
                                     "31-60 天（元）", "61-90 天（元）", "90 天以上（元）"});
                       for (const auto& row : rows) {
                         csv_row(csv, {row.partner_name.empty() ? row.partner_key
                                                               : row.partner_name,
                                       row.direction == -1 ? "应付" : "应收",
                                       yuan(row.total_cents), yuan(row.not_due_cents),
                                       yuan(row.d1_30_cents), yuan(row.d31_60_cents),
                                       yuan(row.d61_90_cents), yuan(row.d90_plus_cents)});
                       }
                       csv_row(csv, {"合计", "", yuan(aging_total), yuan(aging_not_due),
                                     yuan(aging_d1_30), yuan(aging_d31_60),
                                     yuan(aging_d61_90), yuan(aging_d90_plus)});
                       send_csv(res, "ar-ap-aging-" + date_stamp() + ".csv", csv);
                       return;
                     }
                     if (xlsx_requested(req)) {
                       XlsxSheet sheet("应收应付余额与账龄");
                       xlsx_preamble(sheet, "应收应付余额与账龄",
                                     std::string("方向 ") + (direction == -1 ? "应付" : "应收") +
                                         "；截止日 " + as_of +
                                         "；仅统计 v7 迁移后审核的单据"
                                         "；按截止日时点还原：业务日期晚于截止日不计入，"
                                         "核销按核销日期还原");
                       sheet.add_header_row({"往来单位", "方向", "合计（元）", "未到期（元）",
                                             "1-30 天（元）", "31-60 天（元）", "61-90 天（元）",
                                             "90 天以上（元）"});
                       for (const auto& row : rows) {
                         sheet.add_row(
                             {XlsxCell::text(row.partner_name.empty() ? row.partner_key
                                                                      : row.partner_name),
                              XlsxCell::text(row.direction == -1 ? "应付" : "应收"),
                              XlsxCell::money(row.total_cents),
                              XlsxCell::money(row.not_due_cents),
                              XlsxCell::money(row.d1_30_cents),
                              XlsxCell::money(row.d31_60_cents),
                              XlsxCell::money(row.d61_90_cents),
                              XlsxCell::money(row.d90_plus_cents)});
                       }
                       sheet.add_row({XlsxCell::text("合计"), XlsxCell::text(""),
                                      XlsxCell::money(aging_total),
                                      XlsxCell::money(aging_not_due),
                                      XlsxCell::money(aging_d1_30),
                                      XlsxCell::money(aging_d31_60),
                                      XlsxCell::money(aging_d61_90),
                                      XlsxCell::money(aging_d90_plus)});
                       send_xlsx(res, "ar-ap-aging-" + date_stamp() + ".xlsx", sheet.build());
                       return;
                     }
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : rows) {
                       list.push_back(aging_to_json(row));
                     }
                     json_ok(res, list);
                   }));
}

}  // namespace xiaotu
