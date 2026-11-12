#include <httplib.h>
#include <nlohmann/json.hpp>

#include "xiaotu/http_guard.h"
#include "xiaotu/http_json.h"
#include "xiaotu/services.h"

namespace xiaotu {
namespace {

nlohmann::json to_json(const CompanySettings& company) {
  return nlohmann::json{
      {"name", company.name},
      {"phone", company.phone},
      {"address", company.address},
  };
}

CompanySettings from_json(const nlohmann::json& body) {
  const auto company = body.value("company", nlohmann::json::object());
  CompanySettings input;
  input.name = company.value("name", std::string{});
  input.phone = company.value("phone", std::string{});
  input.address = company.value("address", std::string{});
  return input;
}

}  // namespace

void register_setting_routes(httplib::Server& server, Services& services) {
  server.Get("/api/settings",
             guard("setting.read",
                   [&services](const httplib::Request&, httplib::Response& res) {
                     json_ok(res, nlohmann::json{{"company", to_json(services.settings.company())}});
                   }));

  server.Put("/api/settings",
             guard("setting.write",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const CompanySettings updated =
                         services.settings.update_company(from_json(json_body(req)));
                     json_ok(res, nlohmann::json{{"company", to_json(updated)}});
                   }));
}

}  // namespace xiaotu
