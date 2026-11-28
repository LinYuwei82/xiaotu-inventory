#include <httplib.h>
#include <nlohmann/json.hpp>

#include <string>

#include "local_auth_common.h"
#include "xiaotu/audit.h"
#include "xiaotu/context.h"
#include "xiaotu/http_guard.h"
#include "xiaotu/http_json.h"
#include "xiaotu/http_server.h"
#include "xiaotu/http_util.h"
#include "xiaotu/services.h"

namespace xiaotu {

void HttpServer::register_local_auth_routes() {
  // 单机版首次设置本机口令：仅在尚未设置口令时可用，写内置 admin 的 password_hash。
  // 不走「首次设置强制改密」：门禁页本身已经要求输入口令，再弹改密页没有意义。
  server_->Post("/api/local/password",
                [this](const httplib::Request& req, httplib::Response& res) {
                  if (info_.mode != "local") {
                    json_error(res, 403, "FORBIDDEN", "仅单机版支持本机口令");
                    return;
                  }
                  const auto body = json_body(req);
                  const std::string password = body.value("password", std::string{});
                  const UserRow admin = services_.users.find_by_username("admin");
                  if (!admin.active) {
                    json_error(res, 403, "USER_DISABLED", "内置管理员已停用");
                    return;
                  }
                  if (services_.users.has_password(admin.id)) {
                    json_error(res, 409, "LOCAL_PASSWORD_ALREADY_SET",
                               "本机口令已设置；如需重置，请在数据目录上执行 --set-password");
                    return;
                  }
                  services_.users.set_password(admin.id, password, /*force_change_on_first_set=*/false);
                  save_local_password_choice(/*enabled=*/true);
                  json_ok(res, nlohmann::json{{"ok", true}});
                });

  // 跳过首次设置提示：记下「已询问」，之后保持一次性 token 自动登录的现状。
  server_->Post("/api/local/password/skip",
                [this](const httplib::Request&, httplib::Response& res) {
                  if (info_.mode != "local") {
                    json_error(res, 403, "FORBIDDEN", "仅单机版支持本机口令");
                    return;
                  }
                  save_local_password_choice(/*enabled=*/false);
                  json_ok(res, nlohmann::json{{"ok", true}});
                });

  // 用 macOS 钥匙串里的长期凭据换正常会话：凭据是服务端签发的随机 token，
  // 原始口令不进入钥匙串。到期、时钟回拨与作废都在服务端判断。
  server_->Post("/api/local/remember", [this](const httplib::Request& req,
                                              httplib::Response& res) {
    if (info_.mode != "local") {
      json_error(res, 403, "FORBIDDEN", "仅单机版支持本机免输");
      return;
    }
    if (!local_password_required()) {
      json_error(res, 401, "REMEMBER_TOKEN_INVALID", "本机口令门禁未启用");
      return;
    }
    const std::string token = json_body(req).value("token", std::string{});
    std::string failure;
    if (!validate_local_remember_token(services_.db, token, &failure)) {
      json_error(res, 401, "REMEMBER_TOKEN_INVALID", failure);
      return;
    }
    const UserRow admin = services_.users.find_by_username("admin");
    if (!admin.active) {
      json_error(res, 403, "USER_DISABLED", "内置管理员已停用");
      return;
    }
    const SessionPolicy policy = services_.settings.session_policy();
    const CreatedSession session = sessions_.create(admin.id, policy.idle_minutes);
    res.set_header("Set-Cookie",
                   "sid=" + session.id + cookie_attributes(req, policy.idle_minutes * 60));
    services_.db.write([&](Conn& conn) {
      audit_write_as(conn, admin.username, "login", "auth", admin.id, admin.username,
                     "本机免输");
    });
    nlohmann::json result;
    result["ok"] = true;
    result["user"] = admin.username;
    result["role"] = admin.role;
    result["csrf"] = session.csrf;
    result["must_change_password"] = admin.must_change_password;
    json_ok(res, result);
  });

  // 本机免输设置：默认 30 天免输；切到「每次都要输」时立即作废服务端凭据。
  server_->Get("/api/local/remember/setting",
               guard("setting.write",
                     [this](const httplib::Request&, httplib::Response& res) {
                       if (info_.mode != "local") {
                         json_error(res, 403, "FORBIDDEN", "仅单机版支持本机免输");
                         return;
                       }
                       json_ok(res, nlohmann::json{
                                        {"enabled", local_remember_enabled(services_.db)},
                                        {"has_token", has_local_remember_token(services_.db)}});
                     }));

  server_->Put("/api/local/remember/setting",
               guard("setting.write",
                     [this](const httplib::Request& req, httplib::Response& res) {
                       if (info_.mode != "local") {
                         json_error(res, 403, "FORBIDDEN", "仅单机版支持本机免输");
                         return;
                       }
                       const bool enabled = json_body(req).value("enabled", true);
                       services_.db.write([&](Conn& conn) {
                         write_setting_value(conn, kLocalRememberSettingKey,
                                             enabled ? "1" : "0");
                       });
                       const UserRow admin = services_.users.find_by_username("admin");
                       const std::string actor = current_context().user;
                       if (!enabled) {
                         clear_local_remember_tokens(services_.db, admin.id, actor,
                                                     "切换为每次都要输");
                         json_ok(res, nlohmann::json{{"ok", true}, {"enabled", false}});
                         return;
                       }
                       const IssuedRememberToken issued =
                           issue_local_remember_token(services_.db, admin.id, actor);
                       json_ok(res, nlohmann::json{{"ok", true},
                                                   {"enabled", true},
                                                   {"remember_token", issued.token},
                                                   {"expires_at", issued.expires_at}});
                     }));

  server_->Post("/api/local/remember/clear",
                guard("setting.write",
                      [this](const httplib::Request&, httplib::Response& res) {
                        if (info_.mode != "local") {
                          json_error(res, 403, "FORBIDDEN", "仅单机版支持本机免输");
                          return;
                        }
                        const UserRow admin = services_.users.find_by_username("admin");
                        clear_local_remember_tokens(services_.db, admin.id,
                                                    current_context().user, "用户清除本机记忆");
                        json_ok(res, nlohmann::json{{"ok", true}});
                      }));
}

}  // namespace xiaotu
