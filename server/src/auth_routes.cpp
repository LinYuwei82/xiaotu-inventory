#include <httplib.h>
#include <nlohmann/json.hpp>

#include <string>

#include "local_auth_common.h"
#include "xiaotu/audit.h"
#include "xiaotu/context.h"
#include "xiaotu/error.h"
#include "xiaotu/http_json.h"
#include "xiaotu/http_server.h"
#include "xiaotu/http_util.h"
#include "xiaotu/permissions.h"
#include "xiaotu/services.h"

namespace xiaotu {

void HttpServer::register_auth_routes() {
  server_->Get("/api/me", [this](const httplib::Request&, httplib::Response& res) {
    const auto& context = current_context();
    nlohmann::json body;
    body["user"] = context.user;
    body["role"] = context.role;
    body["csrf"] = context.csrf;
    // 前端据此隐藏无权限的入口；真正的拦截仍在路由的 guard 里。
    body["permissions"] = role_permissions(context.role);
    body["must_change_password"] = context.must_change_password;
    json_ok(res, body);
  });

  // 一次性 token 换会话：仅单机版使用；网络版必须口令登录。
  server_->Post("/api/session", [this](const httplib::Request& req, httplib::Response& res) {
    if (info_.mode != "local") {
      json_error(res, 403, "FORBIDDEN", "网络版请使用用户名与口令登录");
      return;
    }
    // 启用本机口令门禁后，启动链接里的一次性 token 不再能绕开门禁。
    if (local_password_required()) {
      json_error(res, 403, "LOCAL_PASSWORD_REQUIRED", "已设置本机口令，请先输入口令");
      return;
    }
    const auto body = json_body(req);
    const std::string token = body.value("token", std::string{});
    if (!consume_bootstrap_token(token)) {
      json_error(res, 401, "UNAUTHORIZED", "访问凭据无效或已过期");
      return;
    }
    const UserRow admin = services_.users.find_by_username("admin");
    if (!admin.active) {
      json_error(res, 403, "USER_DISABLED", "内置管理员已停用");
      return;
    }
    const SessionPolicy policy = services_.settings.session_policy();
    const CreatedSession session = sessions_.create(admin.id, policy.idle_minutes);
    services_.db.write([&](Conn& conn) {
      audit_write_as(conn, admin.username, "login", "auth", admin.id, admin.username,
                     "单机令牌");
    });
    res.set_header("Set-Cookie",
                   "sid=" + session.id + cookie_attributes(req, policy.idle_minutes * 60));
    nlohmann::json result;
    result["ok"] = true;
    result["user"] = admin.username;
    result["role"] = admin.role;
    result["csrf"] = session.csrf;
    result["must_change_password"] = admin.must_change_password;
    json_ok(res, result);
  });

  // 口令登录：失败计数落库（按账号），达到 5 次锁定 15 分钟；成功即清零。
  // 不存在的用户名仍走内存限流，避免枚举时被无限试探。
  server_->Post("/api/login", [this](const httplib::Request& req, httplib::Response& res) {
    const auto body = json_body(req);
    const std::string username = body.value("username", std::string{});
    const std::string password = body.value("password", std::string{});
    const std::string key = req.remote_addr + "|" + username;
    bool known = true;
    try {
      services_.users.find_by_username(username);
    } catch (const AppError&) {
      known = false;
    }
    if (!known && login_limiter_.blocked(key)) {
      res.set_header("Retry-After", "300");
      json_error(res, 429, "TOO_MANY_ATTEMPTS", "登录失败次数过多，请 5 分钟后再试");
      return;
    }
    UserRow user;
    try {
      user = services_.users.authenticate(username, password, req.remote_addr);
    } catch (const AppError& error) {
      if (error.code() == "INVALID_CREDENTIALS") {
        if (!known) {
          login_limiter_.record_failure(key);
        }
        json_error(res, 401, "INVALID_CREDENTIALS", "用户名或口令错误");
        return;
      }
      throw;
    }
    if (known) {
      login_limiter_.reset(key);
    }
    // 登录成功后顺手清掉过期会话，不需要额外的定时任务。
    sessions_.purge_expired();
    const SessionPolicy policy = services_.settings.session_policy();
    const CreatedSession session = sessions_.create(user.id, policy.idle_minutes);
    res.set_header("Set-Cookie",
                   "sid=" + session.id + cookie_attributes(req, policy.idle_minutes * 60));
    nlohmann::json result;
    result["ok"] = true;
    result["user"] = user.username;
    result["role"] = user.role;
    result["csrf"] = session.csrf;
    result["must_change_password"] = user.must_change_password;
    // 单机版口令登录成功后按设置签发长期凭据，由前端写入 macOS 钥匙串。
    if (info_.mode == "local" && body.value("remember", false) &&
        local_password_required()) {
      const IssuedRememberToken issued =
          issue_local_remember_token(services_.db, user.id, user.username);
      result["remember_token"] = issued.token;
      result["remember_expires_at"] = issued.expires_at;
    }
    json_ok(res, result);
  });

  // 自助改密：校验原口令；成功后清失败计数、锁定与强制改密标记，并换发新会话。
  server_->Post("/api/password/change", [this](const httplib::Request& req,
                                               httplib::Response& res) {
    const auto body = json_body(req);
    const std::string old_password = body.value("old_password", std::string{});
    const std::string new_password = body.value("new_password", std::string{});
    services_.users.change_password(current_context().user_id, old_password, new_password);
    const SessionPolicy policy = services_.settings.session_policy();
    const CreatedSession session = sessions_.create(current_context().user_id, policy.idle_minutes);
    res.set_header("Set-Cookie",
                   "sid=" + session.id + cookie_attributes(req, policy.idle_minutes * 60));
    nlohmann::json result;
    result["ok"] = true;
    result["csrf"] = session.csrf;
    result["must_change_password"] = false;
    json_ok(res, result);
  });

  server_->Post("/api/session/logout", [this](const httplib::Request& req,
                                              httplib::Response& res) {
    sessions_.erase(cookie_value(req, "sid"));
    res.set_header("Set-Cookie", "sid=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
    nlohmann::json body;
    body["ok"] = true;
    json_ok(res, body);
  });
}

}  // namespace xiaotu
