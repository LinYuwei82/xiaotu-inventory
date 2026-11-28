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

void register_report_cost_recalc_routes(httplib::Server& server, Services& services) {
  // 成本重算只读核对：按流水重放移动加权平均，与账面结存比对，不写回。
  server.Get("/api/stock/cost-recalc",
             guard("stock.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const std::int64_t item_id =
                         req.has_param("item_id") ? parse_id(req.get_param_value("item_id")) : 0;
                     const std::int64_t warehouse_id =
                         req.has_param("warehouse_id")
                             ? parse_id(req.get_param_value("warehouse_id"))
                             : 0;
                     const CostRecalcResult result =
                         services.cost_recalc.recalculate(item_id, warehouse_id);
                     nlohmann::json differences = nlohmann::json::array();
                     for (const auto& row : result.differences) {
                       differences.push_back(
                           {{"item_id", row.item_id},
                            {"item_code", row.item_code},
                            {"item_name", row.item_name},
                            {"warehouse_id", row.warehouse_id},
                            {"warehouse_name", row.warehouse_name},
                            {"book_qty", qty_text(row.book_qty)},
                            {"book_amount_cents", row.book_amount_cents},
                            {"replay_qty", qty_text(row.replay_qty)},
                            {"replay_amount_cents", row.replay_amount_cents},
                            {"qty_diff", qty_text(row.qty_diff)},
                            {"amount_diff_cents", row.amount_diff_cents}});
                     }
                     json_ok(res, nlohmann::json{
                                      {"ledger_rows", result.ledger_rows},
                                      {"item_count", result.item_count},
                                      {"difference_count", result.difference_count},
                                      {"qty_difference_count", result.qty_difference_count},
                                      {"differences", differences},
                                  });
                   }));
}

}  // namespace xiaotu
