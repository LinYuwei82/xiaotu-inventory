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

nlohmann::json summary_to_json(const InventorySummaryRow& row) {
  return nlohmann::json{
      {"item_id", row.item_id},
      {"item_code", row.item_code},
      {"item_name", row.item_name},
      {"unit", row.unit},
      {"opening_qty", qty_text(row.opening_qty)},
      {"in_qty", qty_text(row.in_qty)},
      {"out_qty", qty_text(row.out_qty)},
      {"closing_qty", qty_text(row.closing_qty)},
      {"opening_amount_cents", row.opening_amount_cents},
      {"in_amount_cents", row.in_amount_cents},
      {"out_amount_cents", row.out_amount_cents},
      {"adjust_amount_cents", row.adjust_amount_cents},
      {"closing_amount_cents", row.closing_amount_cents},
  };
}

}  // namespace

void register_report_inventory_summary_routes(httplib::Server& server, Services& services) {
  server.Get("/api/reports/inventory-summary",
             guard("stock.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const ReportFilter filter = parse_filter(req);
                     const auto rows = services.reports.inventory_summary(filter);
                     // 数量跨单位不可加总，合计行只汇总五个金额列。
                     std::int64_t sum_opening = 0;
                     std::int64_t sum_in = 0;
                     std::int64_t sum_out = 0;
                     std::int64_t sum_adjust = 0;
                     std::int64_t sum_closing = 0;
                     for (const auto& row : rows) {
                       sum_opening += row.opening_amount_cents;
                       sum_in += row.in_amount_cents;
                       sum_out += row.out_amount_cents;
                       sum_adjust += row.adjust_amount_cents;
                       sum_closing += row.closing_amount_cents;
                     }
                     if (csv_requested(req)) {
                       std::string csv;
                       csv_preamble(csv, "进销存汇总",
                                    period_label(filter) + "；" +
                                        dimension_label(filter.item_id, "商品") + "；" +
                                        dimension_label(filter.warehouse_id, "仓库") +
                                        "；按商品跨仓库汇总；数量跨单位不可加总，合计只汇总金额；"
                                        "数量为 0、只动金额的流水单列「其他调整」");
                       csv_row(csv, {"商品编码", "商品名称", "单位", "期初数量",
                                     "期初金额（元）", "入库数量", "入库金额（元）",
                                     "出库数量", "出库金额（元）", "其他调整（元）",
                                     "期末数量", "期末金额（元）"});
                       for (const auto& row : rows) {
                         csv_row(csv, {row.item_code, row.item_name, row.unit,
                                       qty_text(row.opening_qty),
                                       yuan(row.opening_amount_cents),
                                       qty_text(row.in_qty), yuan(row.in_amount_cents),
                                       qty_text(row.out_qty), yuan(row.out_amount_cents),
                                       yuan(row.adjust_amount_cents),
                                       qty_text(row.closing_qty),
                                       yuan(row.closing_amount_cents)});
                       }
                       csv_row(csv, {"合计", "", "", "", yuan(sum_opening), "",
                                     yuan(sum_in), "", yuan(sum_out), yuan(sum_adjust), "",
                                     yuan(sum_closing)});
                       send_csv(res, "inventory-summary-" + date_stamp() + ".csv", csv);
                       return;
                     }
                     if (xlsx_requested(req)) {
                       XlsxSheet sheet("进销存汇总");
                       xlsx_preamble(sheet, "进销存汇总",
                                     period_label(filter) + "；" +
                                         dimension_label(filter.item_id, "商品") + "；" +
                                         dimension_label(filter.warehouse_id, "仓库") +
                                         "；按商品跨仓库汇总；数量跨单位不可加总，合计只汇总金额；"
                                         "数量为 0、只动金额的流水单列「其他调整」");
                       sheet.add_header_row({"商品编码", "商品名称", "单位", "期初数量",
                                             "期初金额（元）", "入库数量", "入库金额（元）",
                                             "出库数量", "出库金额（元）", "其他调整（元）",
                                             "期末数量", "期末金额（元）"});
                       for (const auto& row : rows) {
                         sheet.add_row({XlsxCell::text(row.item_code),
                                        XlsxCell::text(row.item_name),
                                        XlsxCell::text(row.unit),
                                        XlsxCell::text(qty_text(row.opening_qty)),
                                        XlsxCell::money(row.opening_amount_cents),
                                        XlsxCell::text(qty_text(row.in_qty)),
                                        XlsxCell::money(row.in_amount_cents),
                                        XlsxCell::text(qty_text(row.out_qty)),
                                        XlsxCell::money(row.out_amount_cents),
                                        XlsxCell::money(row.adjust_amount_cents),
                                        XlsxCell::text(qty_text(row.closing_qty)),
                                        XlsxCell::money(row.closing_amount_cents)});
                       }
                       sheet.add_row({XlsxCell::text("合计"), XlsxCell::text(""),
                                      XlsxCell::text(""), XlsxCell::text(""),
                                      XlsxCell::money(sum_opening), XlsxCell::text(""),
                                      XlsxCell::money(sum_in), XlsxCell::text(""),
                                      XlsxCell::money(sum_out),
                                      XlsxCell::money(sum_adjust), XlsxCell::text(""),
                                      XlsxCell::money(sum_closing)});
                       send_xlsx(res, "inventory-summary-" + date_stamp() + ".xlsx",
                                 sheet.build());
                       return;
                     }
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : rows) {
                       list.push_back(summary_to_json(row));
                     }
                     json_ok(res, list);
                   }));
}

}  // namespace xiaotu
