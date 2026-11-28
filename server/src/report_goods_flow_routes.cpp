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

nlohmann::json flow_to_json(const GoodsFlowRow& row) {
  return nlohmann::json{
      {"item_id", row.item_id},
      {"item_code", row.item_code},
      {"item_name", row.item_name},
      {"unit", row.unit},
      {"warehouse_code", row.warehouse_code},
      {"warehouse_name", row.warehouse_name},
      {"opening_qty", qty_text(row.opening_qty)},
      {"in_qty", qty_text(row.in_qty)},
      {"out_qty", qty_text(row.out_qty)},
      {"closing_qty", qty_text(row.closing_qty)},
      {"in_amount_cents", row.in_amount_cents},
      {"out_amount_cents", row.out_amount_cents},
      {"adjust_amount_cents", row.adjust_amount_cents},
  };
}

}  // namespace

void register_report_goods_flow_routes(httplib::Server& server, Services& services) {
  server.Get("/api/reports/goods-flow",
             guard("stock.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const ReportFilter filter = parse_filter(req);
                     const auto rows = services.reports.goods_flow(filter);
                     // 数量跨单位不可加总，合计行只汇总收入 / 发出 / 其他调整金额。
                     std::int64_t flow_in_amount = 0;
                     std::int64_t flow_out_amount = 0;
                     std::int64_t flow_adjust_amount = 0;
                     for (const auto& row : rows) {
                       flow_in_amount += row.in_amount_cents;
                       flow_out_amount += row.out_amount_cents;
                       flow_adjust_amount += row.adjust_amount_cents;
                     }
                     if (csv_requested(req)) {
                       std::string csv;
                       csv_preamble(csv, "收发明细",
                                    period_label(filter) + "；" +
                                        dimension_label(filter.warehouse_id, "仓库") +
                                        "；数量跨单位不可加总，合计只汇总金额；"
                                        "数量为 0、只动金额的流水单列「其他调整」");
                       csv_row(csv, {"商品编码", "商品名称", "单位", "仓库编码", "仓库名称",
                                     "期初数量", "收入数量", "发出数量", "期末数量",
                                     "收入金额（元）", "发出金额（元）", "其他调整（元）"});
                       for (const auto& row : rows) {
                         csv_row(csv, {row.item_code, row.item_name, row.unit,
                                       row.warehouse_code, row.warehouse_name,
                                       qty_text(row.opening_qty),
                                       qty_text(row.in_qty), qty_text(row.out_qty),
                                       qty_text(row.closing_qty),
                                       yuan(row.in_amount_cents), yuan(row.out_amount_cents),
                                       yuan(row.adjust_amount_cents)});
                       }
                       csv_row(csv, {"合计", "", "", "", "", "", "", "", "",
                                     yuan(flow_in_amount), yuan(flow_out_amount),
                                     yuan(flow_adjust_amount)});
                       send_csv(res, "goods-flow-" + date_stamp() + ".csv", csv);
                       return;
                     }
                     if (xlsx_requested(req)) {
                       XlsxSheet sheet("收发明细");
                       xlsx_preamble(sheet, "收发明细",
                                     period_label(filter) + "；" +
                                         dimension_label(filter.warehouse_id, "仓库") +
                                         "；数量跨单位不可加总，合计只汇总金额；"
                                         "数量为 0、只动金额的流水单列「其他调整」");
                       sheet.add_header_row({"商品编码", "商品名称", "单位", "仓库编码",
                                             "仓库名称", "期初数量", "收入数量", "发出数量",
                                             "期末数量", "收入金额（元）", "发出金额（元）",
                                             "其他调整（元）"});
                       for (const auto& row : rows) {
                         sheet.add_row({XlsxCell::text(row.item_code),
                                        XlsxCell::text(row.item_name),
                                        XlsxCell::text(row.unit),
                                        XlsxCell::text(row.warehouse_code),
                                        XlsxCell::text(row.warehouse_name),
                                        XlsxCell::text(qty_text(row.opening_qty)),
                                        XlsxCell::text(qty_text(row.in_qty)),
                                        XlsxCell::text(qty_text(row.out_qty)),
                                        XlsxCell::text(qty_text(row.closing_qty)),
                                        XlsxCell::money(row.in_amount_cents),
                                        XlsxCell::money(row.out_amount_cents),
                                        XlsxCell::money(row.adjust_amount_cents)});
                       }
                       sheet.add_row({XlsxCell::text("合计"),
                                      XlsxCell::text(""), XlsxCell::text(""),
                                      XlsxCell::text(""), XlsxCell::text(""),
                                      XlsxCell::text(""), XlsxCell::text(""),
                                      XlsxCell::text(""), XlsxCell::text(""),
                                      XlsxCell::money(flow_in_amount),
                                      XlsxCell::money(flow_out_amount),
                                      XlsxCell::money(flow_adjust_amount)});
                       send_xlsx(res, "goods-flow-" + date_stamp() + ".xlsx", sheet.build());
                       return;
                     }
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : rows) {
                       list.push_back(flow_to_json(row));
                     }
                     json_ok(res, list);
                   }));


}

}  // namespace xiaotu
