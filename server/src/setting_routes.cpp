#include <httplib.h>
#include <nlohmann/json.hpp>

#include "xiaotu/http_guard.h"
#include "xiaotu/http_json.h"
#include "xiaotu/services.h"

namespace xiaotu {
namespace {

nlohmann::json to_json(const SystemSettings& settings) {
  return nlohmann::json{
      {"company", nlohmann::json{{"name", settings.company.name},
                                 {"phone", settings.company.phone},
                                 {"address", settings.company.address}}},
      {"allow_negative_stock", settings.allow_negative_stock},
      {"session_idle_minutes", settings.session_idle_minutes},
      {"session_absolute_hours", settings.session_absolute_hours},
  };
}

// 只覆盖请求里出现的字段，避免旧客户端只提交公司抬头时把开关重置。
void merge_json(const nlohmann::json& body, SystemSettings& settings) {
  if (body.contains("company")) {
    const auto company = body.value("company", nlohmann::json::object());
    settings.company.name = company.value("name", settings.company.name);
    settings.company.phone = company.value("phone", settings.company.phone);
    settings.company.address = company.value("address", settings.company.address);
  }
  if (body.contains("allow_negative_stock")) {
    settings.allow_negative_stock = body.value("allow_negative_stock", false);
  }
  if (body.contains("session_idle_minutes")) {
    settings.session_idle_minutes = body.value("session_idle_minutes", 30);
  }
  if (body.contains("session_absolute_hours")) {
    settings.session_absolute_hours = body.value("session_absolute_hours", 12);
  }
}

}  // namespace

void register_setting_routes(httplib::Server& server, Services& services) {
  server.Get("/api/settings",
             guard("setting.read",
                   [&services](const httplib::Request&, httplib::Response& res) {
                     json_ok(res, to_json(services.settings.load()));
                   }));

  server.Put("/api/settings",
             guard("setting.write",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     SystemSettings input = services.settings.load();
                     merge_json(json_body(req), input);
                     json_ok(res, to_json(services.settings.update(input)));
                   }));
}

}  // namespace xiaotu
