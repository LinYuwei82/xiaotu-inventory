#include <httplib.h>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

#include "xiaotu/http_guard.h"
#include "xiaotu/http_json.h"
#include "xiaotu/models.h"
#include "xiaotu/report_service.h"
#include "xiaotu/services.h"

namespace xiaotu {
namespace {

ReportFilter parse_filter(const httplib::Request& req) {
  ReportFilter filter;
  if (req.has_param("from")) filter.from = req.get_param_value("from");
  if (req.has_param("to")) filter.to = req.get_param_value("to");
  if (req.has_param("item_id")) filter.item_id = std::stoll(req.get_param_value("item_id"));
  if (req.has_param("warehouse_id")) {
    filter.warehouse_id = std::stoll(req.get_param_value("warehouse_id"));
  }
  if (req.has_param("limit")) filter.limit = std::stoi(req.get_param_value("limit"));
  return filter;
}

nlohmann::json balance_to_json(const StockBalanceRow& row) {
  return nlohmann::json{
      {"item_id", row.item_id},
      {"item_code", row.item_code},
      {"item_name", row.item_name},
      {"unit", row.unit},
      {"warehouse_code", row.warehouse_code},
      {"warehouse_name", row.warehouse_name},
      {"qty", row.qty},
      {"amount_cents", row.amount_cents},
  };
}

nlohmann::json ledger_to_json(const LedgerDetailRow& row) {
  return nlohmann::json{
      {"id", row.id},
      {"biz_time", row.biz_time},
      {"biz_type", row.biz_type},
      {"doc_no", row.doc_no},
      {"item_code", row.item_code},
      {"item_name", row.item_name},
      {"warehouse_name", row.warehouse_name},
      {"qty", row.qty},
      {"amount_cents", row.amount_cents},
  };
}

nlohmann::json flow_to_json(const GoodsFlowRow& row) {
  return nlohmann::json{
      {"item_id", row.item_id},
      {"item_code", row.item_code},
      {"item_name", row.item_name},
      {"unit", row.unit},
      {"warehouse_code", row.warehouse_code},
      {"warehouse_name", row.warehouse_name},
      {"opening_qty", row.opening_qty},
      {"in_qty", row.in_qty},
      {"out_qty", row.out_qty},
      {"closing_qty", row.closing_qty},
      {"in_amount_cents", row.in_amount_cents},
      {"out_amount_cents", row.out_amount_cents},
  };
}

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

void register_report_routes(httplib::Server& server, Services& services) {
  server.Get("/api/reports/stock-balance",
             guard("stock.read",
                   [&services](const httplib::Request&, httplib::Response& res) {
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : services.stock.balances()) {
                       list.push_back(balance_to_json(row));
                     }
                     json_ok(res, list);
                   }));

  server.Get("/api/reports/stock-ledger",
             guard("stock.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : services.reports.stock_ledger(parse_filter(req))) {
                       list.push_back(ledger_to_json(row));
                     }
                     json_ok(res, list);
                   }));

  server.Get("/api/reports/goods-flow",
             guard("stock.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : services.reports.goods_flow(parse_filter(req))) {
                       list.push_back(flow_to_json(row));
                     }
                     json_ok(res, list);
                   }));

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
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : services.reports.ar_ap_aging(direction, as_of)) {
                       list.push_back(aging_to_json(row));
                     }
                     json_ok(res, list);
                   }));
}

}  // namespace xiaotu
