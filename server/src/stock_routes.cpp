#include <httplib.h>
#include <nlohmann/json.hpp>

#include <string>

#include "xiaotu/http_guard.h"
#include "xiaotu/http_json.h"
#include "xiaotu/models.h"
#include "xiaotu/services.h"

namespace xiaotu {
namespace {

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

nlohmann::json ledger_to_json(const StockLedgerRow& row) {
  return nlohmann::json{
      {"id", row.id},
      {"biz_type", row.biz_type},
      {"biz_time", row.biz_time},
      {"doc_no", row.doc_no},
      {"item_code", row.item_code},
      {"item_name", row.item_name},
      {"warehouse_name", row.warehouse_name},
      {"qty", row.qty},
      {"amount_cents", row.amount_cents},
  };
}

}  // namespace

void register_stock_routes(httplib::Server& server, Services& services) {
  server.Get("/api/stock/balances",
             guard("stock.read",
                   [&services](const httplib::Request&, httplib::Response& res) {
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : services.stock.balances()) {
                       list.push_back(balance_to_json(row));
                     }
                     json_ok(res, list);
                   }));

  server.Get("/api/stock/ledger",
             guard("stock.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     std::int64_t item_id = 0;
                     if (req.has_param("item_id")) {
                       item_id = std::stoll(req.get_param_value("item_id"));
                     }
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : services.stock.ledger(item_id)) {
                       list.push_back(ledger_to_json(row));
                     }
                     json_ok(res, list);
                   }));
}

}  // namespace xiaotu
