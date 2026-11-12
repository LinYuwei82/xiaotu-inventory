#include <httplib.h>
#include <nlohmann/json.hpp>

#include <exception>
#include <string>

#include "xiaotu/audit.h"
#include "xiaotu/http_guard.h"
#include "xiaotu/http_json.h"
#include "xiaotu/services.h"

namespace xiaotu {
namespace {

// 参数缺失或格式不对时按「不限」处理，不让一个手写的查询串把页面变成 500。
std::int64_t param_int(const httplib::Request& req, const char* name,
                       std::int64_t fallback) {
  if (!req.has_param(name)) {
    return fallback;
  }
  try {
    return std::stoll(req.get_param_value(name));
  } catch (const std::exception&) {
    return fallback;
  }
}

}  // namespace

void register_audit_routes(httplib::Server& server, Services& services) {
  server.Get("/api/audit-logs",
             guard("audit.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     AuditFilter filter;
                     filter.from = req.has_param("from") ? req.get_param_value("from") : "";
                     filter.to = req.has_param("to") ? req.get_param_value("to") : "";
                     filter.entity =
                         req.has_param("entity") ? req.get_param_value("entity") : "";
                     filter.entity_id = param_int(req, "entity_id", 0);
                     filter.actor = req.has_param("actor") ? req.get_param_value("actor") : "";
                     filter.page = static_cast<int>(param_int(req, "page", 1));
                     filter.page_size = static_cast<int>(param_int(req, "page_size", 50));

                     const AuditPage page = audit_query(services.db, filter);
                     nlohmann::json items = nlohmann::json::array();
                     for (const auto& row : page.rows) {
                       items.push_back(nlohmann::json{
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
                     nlohmann::json body;
                     body["items"] = items;
                     body["total"] = page.total;
                     body["page"] = page.page;
                     body["page_size"] = page.page_size;
                     json_ok(res, body);
                   }));
}

}  // namespace xiaotu
