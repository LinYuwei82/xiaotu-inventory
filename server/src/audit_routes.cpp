#include <httplib.h>
#include <nlohmann/json.hpp>

#include "xiaotu/audit.h"
#include "xiaotu/http_guard.h"
#include "xiaotu/http_json.h"
#include "xiaotu/services.h"

namespace xiaotu {

void register_audit_routes(httplib::Server& server, Services& services) {
  server.Get("/api/audit-logs",
             guard("audit.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const int limit =
                         req.has_param("limit") ? std::stoi(req.get_param_value("limit")) : 100;
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : audit_recent(services.db, limit)) {
                       list.push_back(nlohmann::json{
                           {"id", row.id},
                           {"at", row.at},
                           {"actor", row.actor},
                           {"action", row.action},
                           {"entity", row.entity},
                           {"entity_id", row.entity_id},
                           {"entity_no", row.entity_no},
                           {"detail", row.detail},
                       });
                     }
                     json_ok(res, list);
                   }));
}

}  // namespace xiaotu
