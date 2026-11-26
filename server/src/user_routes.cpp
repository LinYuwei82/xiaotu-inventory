#include <httplib.h>
#include <nlohmann/json.hpp>

#include <string>

#include "xiaotu/edition.h"
#include "xiaotu/http_guard.h"
#include "xiaotu/http_json.h"
#include "xiaotu/services.h"

namespace xiaotu {
namespace {

nlohmann::json to_json(const UserRow& row) {
  return nlohmann::json{
      {"id", row.id},
      {"username", row.username},
      {"display_name", row.display_name},
      {"role", row.role},
      {"active", row.active},
      {"created_at", row.created_at},
      {"updated_at", row.updated_at},
  };
}

UserInput from_json(const nlohmann::json& body) {
  UserInput input;
  input.username = body.value("username", std::string{});
  input.display_name = body.value("display_name", std::string{});
  input.role = body.value("role", std::string{"viewer"});
  input.active = body.value("active", true);
  input.password = body.value("password", std::string{});
  return input;
}

}  // namespace

void register_user_routes(httplib::Server& server, Services& services) {
  server.Get("/api/users",
             guard("user.read",
                   [&services](const httplib::Request&, httplib::Response& res) {
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : services.users.list()) {
                       list.push_back(to_json(row));
                     }
                     json_ok(res, list);
                   }));

  server.Post("/api/users",
              guard("user.write",
                    [&services](const httplib::Request& req, httplib::Response& res) {
                      const EditionPolicy& policy = EditionPolicy::instance();
                      policy.require("system.user");
                      const auto body = json_body(req);
                      policy.check_quota("max_users", services.users.count_active(), 1);
                      json_ok(res, to_json(services.users.create(from_json(body))));
                    }));

  server.Put(R"(/api/users/(\d+))",
             guard("user.write",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const std::int64_t id = parse_id(req.matches[1].str());
                     const UserInput input = from_json(json_body(req));
                     // 停用账号重新启用同样占用额度。
                     if (input.active && !services.users.get(id).active) {
                       EditionPolicy::instance().check_quota("max_users",
                                                             services.users.count_active(), 1);
                     }
                     json_ok(res, to_json(services.users.update(id, input)));
                   }));

  server.Delete(R"(/api/users/(\d+))",
                guard("user.write",
                      [&services](const httplib::Request& req, httplib::Response& res) {
                        services.users.remove(parse_id(req.matches[1].str()));
                        nlohmann::json ok;
                        ok["ok"] = true;
                        json_ok(res, ok);
                      }));
}

}  // namespace xiaotu
