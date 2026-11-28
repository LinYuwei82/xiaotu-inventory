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

namespace xiaotu {
namespace {

using namespace report_common;

// 采购报表口径说明：入库与退货同表，退货按负数冲减，只统计已审核单据。
std::string purchase_scope(const ReportFilter& filter) {
  return period_label(filter) + "；" + dimension_label(filter.supplier_id, "供应商") + "；" +
         dimension_label(filter.item_id, "商品") + "；" +
         dimension_label(filter.warehouse_id, "仓库") +
         "；包含采购入库与采购退货，退货数量与金额按负数冲减，仅统计已审核单据";
}

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

nlohmann::json purchase_supplier_to_json(const PurchaseSupplierSummaryRow& row) {
  return nlohmann::json{
      {"partner_id", row.partner_id},
      {"supplier", row.supplier},
      {"in_qty", qty_text(row.in_qty)},
      {"in_amount_cents", row.in_amount_cents},
      {"return_amount_cents", row.return_amount_cents},
      {"qty", qty_text(row.qty)},
      {"amount_cents", row.amount_cents},
      {"has_inbound", row.has_inbound},
      {"avg_price_cents", row.has_inbound ? nlohmann::json(row.avg_price_cents)
                                          : nlohmann::json(nullptr)},
  };
}

nlohmann::json purchase_item_to_json(const PurchaseItemSummaryRow& row) {
  return nlohmann::json{
      {"item_id", row.item_id},
      {"item_code", row.item_code},
      {"item_name", row.item_name},
      {"spec", row.spec},
      {"unit", row.unit},
      {"in_qty", qty_text(row.in_qty)},
      {"in_amount_cents", row.in_amount_cents},
      {"return_amount_cents", row.return_amount_cents},
      {"qty", qty_text(row.qty)},
      {"amount_cents", row.amount_cents},
      {"has_inbound", row.has_inbound},
      {"avg_price_cents", row.has_inbound ? nlohmann::json(row.avg_price_cents)
                                          : nlohmann::json(nullptr)},
  };
}

nlohmann::json purchase_totals_to_json(const PurchaseSummaryTotals& totals) {
  return nlohmann::json{
      {"in_qty", qty_text(totals.in_qty)},
      {"in_amount_cents", totals.in_amount_cents},
      {"return_amount_cents", totals.return_amount_cents},
      {"qty", qty_text(totals.qty)},
      {"amount_cents", totals.amount_cents},
      {"has_inbound", totals.has_inbound},
      {"avg_price_cents", totals.has_inbound ? nlohmann::json(totals.avg_price_cents)
                                             : nlohmann::json(nullptr)},
  };
}

}  // namespace

void register_report_purchase_routes(httplib::Server& server, Services& services) {
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

  // 采购汇总：by=supplier（默认）按供应商名称升序，by=item 按商品编码升序。
  server.Get("/api/reports/purchase-summary",
             guard("finance.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const ReportFilter filter = parse_filter(req);
                     const std::string by =
                         req.has_param("by") ? req.get_param_value("by") : std::string("supplier");
                     const bool by_item = by == "item";
                     const auto totals = services.reports.purchase_summary_totals(filter);
                     const std::string avg_text =
                         totals.has_inbound ? yuan(totals.avg_price_cents) : "—";
                     const std::string scope = purchase_scope(filter) + "；维度 " +
                                               (by_item ? "按商品" : "按供应商") +
                                               "；均价 = 入库金额 ÷ 入库数量（只计入库，退货不参与）";

                     if (by_item) {
                       const auto rows = services.reports.purchase_summary_by_item(filter);
                       if (csv_requested(req)) {
                         std::string csv;
                         csv_preamble(csv, "采购汇总（按商品）", scope);
                         csv_row(csv, {"商品编码", "商品名称", "规格", "单位", "入库数量",
                                       "入库金额（元）", "退货额（元）", "净数量", "净金额（元）",
                                       "入库均价（元）"});
                         for (const auto& row : rows) {
                           csv_row(csv, {row.item_code, row.item_name, row.spec, row.unit,
                                         qty_text(row.in_qty), yuan(row.in_amount_cents),
                                         yuan(row.return_amount_cents), qty_text(row.qty),
                                         yuan(row.amount_cents),
                                         row.has_inbound ? yuan(row.avg_price_cents) : "—"});
                         }
                         csv_row(csv, {"合计", "", "", "", qty_text(totals.in_qty),
                                       yuan(totals.in_amount_cents),
                                       yuan(totals.return_amount_cents), qty_text(totals.qty),
                                       yuan(totals.amount_cents), avg_text});
                         send_csv(res, "purchase-summary-item-" + date_stamp() + ".csv", csv);
                         return;
                       }
                       if (xlsx_requested(req)) {
                         XlsxSheet sheet("采购汇总（按商品）");
                         xlsx_preamble(sheet, "采购汇总（按商品）", scope);
                         sheet.add_header_row({"商品编码", "商品名称", "规格", "单位", "入库数量",
                                               "入库金额（元）", "退货额（元）", "净数量",
                                               "净金额（元）", "入库均价（元）"});
                         for (const auto& row : rows) {
                           sheet.add_row({XlsxCell::text(row.item_code),
                                          XlsxCell::text(row.item_name),
                                          XlsxCell::text(row.spec),
                                          XlsxCell::text(row.unit),
                                          XlsxCell::text(qty_text(row.in_qty)),
                                          XlsxCell::money(row.in_amount_cents),
                                          XlsxCell::money(row.return_amount_cents),
                                          XlsxCell::text(qty_text(row.qty)),
                                          XlsxCell::money(row.amount_cents),
                                          row.has_inbound ? XlsxCell::money(row.avg_price_cents)
                                                          : XlsxCell::text("—")});
                         }
                         sheet.add_row({XlsxCell::text("合计"),
                                        XlsxCell::text(""),
                                        XlsxCell::text(""),
                                        XlsxCell::text(""),
                                        XlsxCell::text(qty_text(totals.in_qty)),
                                        XlsxCell::money(totals.in_amount_cents),
                                        XlsxCell::money(totals.return_amount_cents),
                                        XlsxCell::text(qty_text(totals.qty)),
                                        XlsxCell::money(totals.amount_cents),
                                        totals.has_inbound
                                            ? XlsxCell::money(totals.avg_price_cents)
                                            : XlsxCell::text("—")});
                         send_xlsx(res, "purchase-summary-item-" + date_stamp() + ".xlsx",
                                   sheet.build());
                         return;
                       }
                       nlohmann::json list = nlohmann::json::array();
                       for (const auto& row : rows) {
                         list.push_back(purchase_item_to_json(row));
                       }
                       json_ok(res, nlohmann::json{{"rows", list},
                                                   {"totals", purchase_totals_to_json(totals)}});
                       return;
                     }

                     const auto rows = services.reports.purchase_summary_by_supplier(filter);
                     if (csv_requested(req)) {
                       std::string csv;
                       csv_preamble(csv, "采购汇总（按供应商）", scope);
                       csv_row(csv, {"供应商", "入库数量", "入库金额（元）", "退货额（元）",
                                     "净数量", "净金额（元）", "入库均价（元）"});
                       for (const auto& row : rows) {
                         csv_row(csv, {row.supplier, qty_text(row.in_qty),
                                       yuan(row.in_amount_cents),
                                       yuan(row.return_amount_cents), qty_text(row.qty),
                                       yuan(row.amount_cents),
                                       row.has_inbound ? yuan(row.avg_price_cents) : "—"});
                       }
                       csv_row(csv, {"合计", qty_text(totals.in_qty),
                                     yuan(totals.in_amount_cents),
                                     yuan(totals.return_amount_cents), qty_text(totals.qty),
                                     yuan(totals.amount_cents), avg_text});
                       send_csv(res, "purchase-summary-supplier-" + date_stamp() + ".csv", csv);
                       return;
                     }
                     if (xlsx_requested(req)) {
                       XlsxSheet sheet("采购汇总（按供应商）");
                       xlsx_preamble(sheet, "采购汇总（按供应商）", scope);
                       sheet.add_header_row({"供应商", "入库数量", "入库金额（元）", "退货额（元）",
                                             "净数量", "净金额（元）", "入库均价（元）"});
                       for (const auto& row : rows) {
                         sheet.add_row({XlsxCell::text(row.supplier),
                                        XlsxCell::text(qty_text(row.in_qty)),
                                        XlsxCell::money(row.in_amount_cents),
                                        XlsxCell::money(row.return_amount_cents),
                                        XlsxCell::text(qty_text(row.qty)),
                                        XlsxCell::money(row.amount_cents),
                                        row.has_inbound ? XlsxCell::money(row.avg_price_cents)
                                                        : XlsxCell::text("—")});
                       }
                       sheet.add_row({XlsxCell::text("合计"),
                                      XlsxCell::text(qty_text(totals.in_qty)),
                                      XlsxCell::money(totals.in_amount_cents),
                                      XlsxCell::money(totals.return_amount_cents),
                                      XlsxCell::text(qty_text(totals.qty)),
                                      XlsxCell::money(totals.amount_cents),
                                      totals.has_inbound
                                          ? XlsxCell::money(totals.avg_price_cents)
                                          : XlsxCell::text("—")});
                       send_xlsx(res, "purchase-summary-supplier-" + date_stamp() + ".xlsx",
                                 sheet.build());
                       return;
                     }
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : rows) {
                       list.push_back(purchase_supplier_to_json(row));
                     }
                     json_ok(res, nlohmann::json{{"rows", list},
                                                 {"totals", purchase_totals_to_json(totals)}});
                   }));
}

}  // namespace xiaotu
