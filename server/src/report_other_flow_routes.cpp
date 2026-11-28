#include <httplib.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <string_view>

#include "xiaotu/error.h"
#include "xiaotu/http_guard.h"
#include "xiaotu/http_json.h"
#include "xiaotu/models.h"
#include "xiaotu/qty.h"
#include "xiaotu/report_service.h"
#include "xiaotu/services.h"

#include "report_common.h"

namespace xiaotu {
namespace {

using namespace report_common;

nlohmann::json other_flow_to_json(const OtherFlowSummaryRow& row) {
  return nlohmann::json{
      {"category", row.category},
      {"in_qty", qty_text(row.in_qty)},
      {"in_amount_cents", row.in_amount_cents},
      {"out_qty", qty_text(row.out_qty)},
      {"out_amount_cents", row.out_amount_cents},
  };
}

std::string other_flow_scope(const ReportFilter& filter) {
  return period_label(filter) + "；" +
         dimension_label(filter.warehouse_id, "仓库") + "；业务类型 " +
         (filter.biz_category.empty() ? std::string("全部")
                                      : biz_category_label(filter.biz_category)) +
         "；仅统计已审核的其他出入库，出库数量与金额取绝对值";
}

}  // namespace

void register_report_other_flow_routes(httplib::Server& server, Services& services) {
  // 其他出入库按业务类型汇总：入库与出库分列，固定顺序赠品/期初/领用/报损/其他。
  server.Get("/api/reports/other-flow-summary",
             guard("stock.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     ReportFilter filter = parse_filter(req);
                     if (req.has_param("biz_category")) {
                       filter.biz_category = req.get_param_value("biz_category");
                     }
                     const auto rows = services.reports.other_flow_summary(filter);
                     const std::string scope = other_flow_scope(filter);
                     if (csv_requested(req)) {
                       std::string csv;
                       csv_preamble(csv, "其他出入库汇总", scope);
                       csv_row(csv, {"业务类型", "入库数量", "入库金额（元）", "出库数量",
                                     "出库金额（元）"});
                       for (const auto& row : rows) {
                         csv_row(csv, {biz_category_label(row.category), qty_text(row.in_qty),
                                       yuan(row.in_amount_cents), qty_text(row.out_qty),
                                       yuan(row.out_amount_cents)});
                       }
                       send_csv(res, "other-flow-summary-" + date_stamp() + ".csv", csv);
                       return;
                     }
                     if (xlsx_requested(req)) {
                       XlsxSheet sheet("其他出入库汇总");
                       xlsx_preamble(sheet, "其他出入库汇总", scope);
                       sheet.add_header_row({"业务类型", "入库数量", "入库金额（元）", "出库数量",
                                             "出库金额（元）"});
                       for (const auto& row : rows) {
                         sheet.add_row({XlsxCell::text(biz_category_label(row.category)),
                                        XlsxCell::text(qty_text(row.in_qty)),
                                        XlsxCell::money(row.in_amount_cents),
                                        XlsxCell::text(qty_text(row.out_qty)),
                                        XlsxCell::money(row.out_amount_cents)});
                       }
                       send_xlsx(res, "other-flow-summary-" + date_stamp() + ".xlsx",
                                 sheet.build());
                       return;
                     }
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : rows) {
                       list.push_back(other_flow_to_json(row));
                     }
                     json_ok(res, list);
                   }));
}

}  // namespace xiaotu
