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
namespace {

using namespace report_common;

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

void register_report_purchase_summary_routes(httplib::Server& server, Services& services) {
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
