#include <httplib.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

#include "xiaotu/http_guard.h"
#include "xiaotu/http_json.h"
#include "xiaotu/qty.h"
#include "xiaotu/report_service.h"
#include "xiaotu/services.h"

#include "report_common.h"

#include "report_purchase_scope.h"

namespace xiaotu {

using namespace report_common;

// 采购报表口径说明：入库与退货同表，退货按负数冲减，只统计已审核单据。
std::string purchase_scope(const ReportFilter& filter) {
  return period_label(filter) + "；" + dimension_label(filter.supplier_id, "供应商") + "；" +
         dimension_label(filter.item_id, "商品") + "；" +
         dimension_label(filter.warehouse_id, "仓库") +
         "；包含采购入库与采购退货，退货数量与金额按负数冲减，仅统计已审核单据";
}

namespace {

nlohmann::json purchase_detail_to_json(const PurchaseDetailRow& row) {
  return nlohmann::json{
      {"date", row.date},
      {"doc_no", row.doc_no},
      {"kind", row.kind},
      {"supplier", row.supplier},
      {"item_id", row.item_id},
      {"item_code", row.item_code},
      {"item_name", row.item_name},
      {"spec", row.spec},
      {"unit", row.unit},
      {"warehouse_name", row.warehouse_name},
      {"qty", qty_text(row.qty)},
      {"price_cents", row.price_cents},
      {"amount_cents", row.amount_cents},
  };
}

}  // namespace

void register_report_purchase_detail_routes(httplib::Server& server, Services& services) {
  server.Get("/api/reports/purchase-detail",
             guard("finance.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     ReportFilter filter = parse_filter(req);
                     const auto totals = services.reports.purchase_detail_totals(filter);
                     const std::string base_scope =
                         purchase_scope(filter) + "；共 " + std::to_string(totals.rows) + " 行";
                     if (csv_requested(req) || xlsx_requested(req)) {
                       // 导出不分页、不截断：边查边写，内存占用与总行数无关。
                       ReportFilter export_filter = filter;
                       export_filter.limit = 0;
                       export_filter.offset = 0;
                       const std::string scope = base_scope + "；全部导出";
                       if (csv_requested(req)) {
                         send_csv_stream(
                             res, "purchase-detail-" + date_stamp() + ".csv",
                             [&services, export_filter, scope, totals](httplib::DataSink& sink,
                                                                      CsvStream& out) {
                               std::string preamble;
                               csv_preamble(preamble, "采购明细", scope);
                               if (!out.raw(sink, preamble)) return false;
                               if (!out.row(sink, {"日期", "单号", "类型", "供应商",
                                                   "商品编码", "商品名称", "规格", "单位",
                                                   "仓库", "数量", "单价（元）",
                                                   "金额（元）"})) {
                                 return false;
                               }
                               const bool streamed = services.reports.stream_purchase_detail(
                                   export_filter, [&](const PurchaseDetailRow& row) {
                                     return out.row(
                                         sink, {row.date, row.doc_no, row.kind, row.supplier,
                                                row.item_code, row.item_name, row.spec,
                                                row.unit, row.warehouse_name,
                                                qty_text(row.qty), yuan(row.price_cents),
                                                yuan(row.amount_cents)});
                                   });
                               if (!streamed) return false;
                               return out.row(sink, {"合计", "", "", "", "", "", "", "", "",
                                                     qty_text(totals.qty), "",
                                                     yuan(totals.amount_cents)});
                             });
                         return;
                       }
                       XlsxStreamWriter sheet("采购明细");
                       xlsx_preamble(sheet, "采购明细", scope);
                       sheet.add_header_row({"日期", "单号", "类型", "供应商", "商品编码",
                                             "商品名称", "规格", "单位", "仓库", "数量",
                                             "单价（元）", "金额（元）"});
                       const bool streamed = services.reports.stream_purchase_detail(
                           export_filter, [&](const PurchaseDetailRow& row) {
                             sheet.add_row({XlsxCell::text(row.date),
                                            XlsxCell::text(row.doc_no),
                                            XlsxCell::text(row.kind),
                                            XlsxCell::text(row.supplier),
                                            XlsxCell::text(row.item_code),
                                            XlsxCell::text(row.item_name),
                                            XlsxCell::text(row.spec),
                                            XlsxCell::text(row.unit),
                                            XlsxCell::text(row.warehouse_name),
                                            XlsxCell::text(qty_text(row.qty)),
                                            XlsxCell::money(row.price_cents),
                                            XlsxCell::money(row.amount_cents)});
                             return sheet.ok();
                           });
                       if (streamed) {
                         sheet.add_row({XlsxCell::text("合计"),
                                        XlsxCell::text(""), XlsxCell::text(""),
                                        XlsxCell::text(""), XlsxCell::text(""),
                                        XlsxCell::text(""), XlsxCell::text(""),
                                        XlsxCell::text(""), XlsxCell::text(""),
                                        XlsxCell::text(qty_text(totals.qty)),
                                        XlsxCell::text(""),
                                        XlsxCell::money(totals.amount_cents)});
                       }
                       const std::string path = sheet.finish();
                       if (path.empty()) {
                         json_error(res, 500, "EXPORT_FAILED", "导出文件生成失败");
                         return;
                       }
                       send_xlsx_file(res, "purchase-detail-" + date_stamp() + ".xlsx", path);
                       return;
                     }
                     const auto rows = services.reports.purchase_detail(filter);
                     const std::int64_t last_row =
                         static_cast<std::int64_t>(filter.offset + rows.size());
                     const bool truncated = totals.rows > last_row;
                     const std::string scope =
                         base_scope +
                         (rows.empty() ? std::string()
                                       : "，当前显示第 " +
                                             std::to_string(filter.offset + 1) + "–" +
                                             std::to_string(last_row) + " 行") +
                         (truncated ? "；可翻页或导出查看全部" : std::string());
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : rows) {
                       list.push_back(purchase_detail_to_json(row));
                     }
                     json_ok(res, nlohmann::json{{"total", totals.rows},
                                                 {"limit", filter.limit},
                                                 {"offset", filter.offset},
                                                 {"truncated", truncated},
                                                 {"rows", list},
                                                 {"totals",
                                                  {{"qty", qty_text(totals.qty)},
                                                   {"amount_cents", totals.amount_cents}}}});
                   }));


}

}  // namespace xiaotu
