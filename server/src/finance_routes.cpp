#include <httplib.h>
#include <nlohmann/json.hpp>

#include <string>

#include "xiaotu/http_guard.h"
#include "xiaotu/http_json.h"
#include "xiaotu/services.h"

namespace xiaotu {

void register_finance_routes(httplib::Server& server, Services& services) {
  server.Get("/api/ar-ap",
             guard("finance.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     int direction = 0;
                     if (req.has_param("direction")) {
                       const std::string value = req.get_param_value("direction");
                       direction = value == "-1" || value == "AP" ? -1 : 1;
                     }
                     const std::string partner_key =
                         req.has_param("partner_key") ? req.get_param_value("partner_key") : "";
                     const std::string status =
                         req.has_param("status") ? req.get_param_value("status") : "";
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : services.finance.entries(direction, partner_key, status)) {
                       list.push_back(nlohmann::json{
                           {"id", row.id},
                           {"doc_id", row.doc_id},
                           {"doc_no", row.doc_no},
                           {"doc_type", row.doc_type},
                           {"direction", row.direction},
                           {"partner_id", row.partner_id},
                           {"partner_name", row.partner_name},
                           {"partner_key", row.partner_key},
                           {"biz_date", row.biz_date},
                           {"due_date", row.due_date},
                           {"amount_cents", row.amount_cents},
                           {"settled_cents", row.settled_cents},
                           {"outstanding_cents", row.outstanding_cents},
                           {"status", row.status},
                       });
                     }
                     json_ok(res, list);
                   }));
}

}  // namespace xiaotu
