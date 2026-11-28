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

nlohmann::json sales_detail_to_json(const SalesDetailRow& row) {
  return nlohmann::json{{"date", row.date},
                        {"doc_no", row.doc_no},
                        {"customer", row.customer},
                        {"salesperson", row.salesperson},
                        {"item_code", row.item_code},
                        {"item_name", row.item_name},
                        {"spec", row.spec},
                        {"unit", row.unit},
                        {"qty", qty_text(row.qty)},
                        {"price_cents", row.price_cents},
                        {"amount_cents", row.amount_cents},
                        {"cost_cents", row.cost_cents},
                        {"profit_cents", row.amount_cents - row.cost_cents}};
}

}  // namespace

void register_report_sales_detail_routes(httplib::Server& server, Services& services) {
  server.Get("/api/reports/sales-detail",
             guard("finance.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     ReportFilter filter = parse_filter(req);
                     const auto totals = services.reports.sales_detail_totals(filter);
                     const std::string title = "销售明细";
                     const std::string scope =
                         period_label(filter) + "；只计已审核的销售出库与销售退货；共 " +
                         std::to_string(totals.rows) + " 行";
                     if (csv_requested(req) || xlsx_requested(req)) {
                       // 导出不分页、不截断：边查边写，内存占用与总行数无关。
                       ReportFilter export_filter = filter;
                       export_filter.limit = 0;
                       export_filter.offset = 0;
                       if (csv_requested(req)) {
                         send_csv_stream(
                             res, "sales-detail-" + date_stamp() + ".csv",
                             [&services, export_filter, title, scope,
                              totals](httplib::DataSink& sink, CsvStream& out) {
                               std::string preamble;
                               csv_preamble(preamble, title, scope + "；全部导出");
                               if (!out.raw(sink, preamble)) return false;
                               if (!out.row(sink, {"日期", "单号", "客户", "业务员", "商品编码",
                                                   "商品名称", "规格", "单位", "数量",
                                                   "单价（元）", "金额（元）", "成本（元）",
                                                   "毛利（元）"})) {
                                 return false;
                               }
                               const bool streamed = services.reports.stream_sales_detail(
                                   export_filter, [&](const SalesDetailRow& row) {
                                     return out.row(
                                         sink, {row.date, row.doc_no, row.customer,
                                                row.salesperson, row.item_code, row.item_name,
                                                row.spec, row.unit, qty_text(row.qty),
                                                yuan(row.price_cents), yuan(row.amount_cents),
                                                yuan(row.cost_cents),
                                                yuan(row.amount_cents - row.cost_cents)});
                                   });
                               if (!streamed) return false;
                               return out.row(sink, {"合计", "", "", "", "", "", "", "",
                                                     qty_text(totals.qty), "",
                                                     yuan(totals.amount_cents),
                                                     yuan(totals.cost_cents),
                                                     yuan(totals.profit_cents)});
                             });
                         return;
                       }
                       XlsxStreamWriter sheet(title);
                       xlsx_preamble(sheet, title, scope + "；全部导出");
                       sheet.add_header_row({"日期", "单号", "客户", "业务员", "商品编码",
                                             "商品名称", "规格", "单位", "数量", "单价（元）",
                                             "金额（元）", "成本（元）", "毛利（元）"});
                       const bool streamed = services.reports.stream_sales_detail(
                           export_filter, [&](const SalesDetailRow& row) {
                             sheet.add_row({XlsxCell::text(row.date),
                                            XlsxCell::text(row.doc_no),
                                            XlsxCell::text(row.customer),
                                            XlsxCell::text(row.salesperson),
                                            XlsxCell::text(row.item_code),
                                            XlsxCell::text(row.item_name),
                                            XlsxCell::text(row.spec), XlsxCell::text(row.unit),
                                            XlsxCell::text(qty_text(row.qty)),
                                            XlsxCell::money(row.price_cents),
                                            XlsxCell::money(row.amount_cents),
                                            XlsxCell::money(row.cost_cents),
                                            XlsxCell::money(row.amount_cents - row.cost_cents)});
                             return sheet.ok();
                           });
                       if (streamed) {
                         sheet.add_row({XlsxCell::text("合计"), XlsxCell::text(""),
                                        XlsxCell::text(""), XlsxCell::text(""),
                                        XlsxCell::text(""), XlsxCell::text(""),
                                        XlsxCell::text(""), XlsxCell::text(""),
                                        XlsxCell::text(qty_text(totals.qty)),
                                        XlsxCell::text(""),
                                        XlsxCell::money(totals.amount_cents),
                                        XlsxCell::money(totals.cost_cents),
                                        XlsxCell::money(totals.profit_cents)});
                       }
                       const std::string path = sheet.finish();
                       if (path.empty()) {
                         json_error(res, 500, "EXPORT_FAILED", "导出文件生成失败");
                         return;
                       }
                       send_xlsx_file(res, "sales-detail-" + date_stamp() + ".xlsx", path);
                       return;
                     }
                     const auto rows = services.reports.sales_detail(filter, "customer");
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : rows) list.push_back(sales_detail_to_json(row));
                     json_ok(res, nlohmann::json{
                                     {"rows", list},
                                     {"total", totals.rows},
                                     {"limit", filter.limit},
                                     {"offset", filter.offset},
                                     {"totals",
                                      {{"qty", qty_text(totals.qty)},
                                       {"amount_cents", totals.amount_cents},
                                       {"cost_cents", totals.cost_cents},
                                       {"profit_cents", totals.profit_cents}}}});
                   }));


}

}  // namespace xiaotu
