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

nlohmann::json balance_to_json(const StockBalanceRow& row) {
  return nlohmann::json{
      {"item_id", row.item_id},
      {"item_code", row.item_code},
      {"item_name", row.item_name},
      {"unit", row.unit},
      {"warehouse_code", row.warehouse_code},
      {"warehouse_name", row.warehouse_name},
      {"qty", qty_text(row.qty)},
      {"amount_cents", row.amount_cents},
  };
}

}  // namespace

void register_report_stock_balance_routes(httplib::Server& server, Services& services) {
  server.Get("/api/reports/stock-balance",
             guard("stock.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const std::int64_t warehouse_id =
                         req.has_param("warehouse_id")
                             ? parse_id(req.get_param_value("warehouse_id"))
                             : 0;
                     const std::int64_t item_id =
                         req.has_param("item_id") ? parse_id(req.get_param_value("item_id")) : 0;
                     const bool hide_zero = req.has_param("hide_zero") &&
                                            req.get_param_value("hide_zero") != "0";
                     const auto rows = services.stock.balances(warehouse_id, item_id, hide_zero);
                     // 数量跨单位不可加总，合计行只汇总金额并标注行数。
                     std::int64_t balance_amount = 0;
                     for (const auto& row : rows) balance_amount += row.amount_cents;
                     const std::string balance_total_label =
                         "合计 " + std::to_string(rows.size()) + " 行";
                     const std::string scope =
                         dimension_label(warehouse_id, "仓库") + "；" +
                         dimension_label(item_id, "商品") +
                         (hide_zero ? "；隐藏零库存" : "；包含零库存") +
                         "；数量跨单位不可加总，合计只汇总金额";
                     if (csv_requested(req)) {
                       std::string csv;
                       csv_preamble(csv, "库存余额表", scope);
                       csv_row(csv, {"商品编码", "商品名称", "单位", "仓库编码", "仓库名称",
                                     "数量", "金额（元）"});
                       for (const auto& row : rows) {
                         csv_row(csv, {row.item_code, row.item_name, row.unit,
                                       row.warehouse_code, row.warehouse_name,
                                       qty_text(row.qty), yuan(row.amount_cents)});
                       }
                       csv_row(csv, {balance_total_label, "", "", "", "", "",
                                     yuan(balance_amount)});
                       send_csv(res, "stock-balance-" + date_stamp() + ".csv", csv);
                       return;
                     }
                     if (xlsx_requested(req)) {
                       XlsxSheet sheet("库存余额表");
                       xlsx_preamble(sheet, "库存余额表", scope);
                       sheet.add_header_row({"商品编码", "商品名称", "单位", "仓库编码",
                                             "仓库名称", "数量", "金额（元）"});
                       for (const auto& row : rows) {
                         sheet.add_row({XlsxCell::text(row.item_code),
                                        XlsxCell::text(row.item_name),
                                        XlsxCell::text(row.unit),
                                        XlsxCell::text(row.warehouse_code),
                                        XlsxCell::text(row.warehouse_name),
                                        XlsxCell::text(qty_text(row.qty)),
                                        XlsxCell::money(row.amount_cents)});
                       }
                       sheet.add_row({XlsxCell::text(balance_total_label),
                                      XlsxCell::text(""), XlsxCell::text(""),
                                      XlsxCell::text(""), XlsxCell::text(""),
                                      XlsxCell::text(""),
                                      XlsxCell::money(balance_amount)});
                       send_xlsx(res, "stock-balance-" + date_stamp() + ".xlsx", sheet.build());
                       return;
                     }
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : rows) {
                       list.push_back(balance_to_json(row));
                     }
                     json_ok(res, list);
                   }));


}

}  // namespace xiaotu
