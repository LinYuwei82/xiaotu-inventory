#include "xiaotu/auth_middleware.h"

#include <cstddef>
#include <string>

#include "xiaotu/context.h"
#include "xiaotu/gen/edition_caps.h"
#include "xiaotu/http_json.h"
#include "xiaotu/http_util.h"
#include "xiaotu/ids.h"
#include "xiaotu/services.h"
#include "xiaotu/session_store.h"

namespace xiaotu {

void install_auth_middleware(httplib::Server& server, AuthMiddlewareContext context) {
  server.set_pre_routing_handler([context](const httplib::Request& req,
                                           httplib::Response& res) {
    const std::string sid = cookie_value(req, "sid");
    bool renewed = false;
    const SessionPolicy policy = context.services.settings.session_policy();
    const auto session = context.sessions.get(sid, policy.idle_minutes,
                                              policy.absolute_hours, &renewed);
    if (renewed) {
      // 服务端滑动续期后同步延长浏览器 Cookie，否则 Cookie 会比会话先过期。
      res.set_header("Set-Cookie",
                     "sid=" + sid + cookie_attributes(req, policy.idle_minutes * 60));
    }
    current_context() = RequestContext{session ? session->user : std::string{},
                                       session ? session->role : std::string{},
                                       random_hex(8),
                                       session ? session->csrf : std::string{},
                                       session ? session->user_id : 0,
                                       session ? session->must_change_password : false,
                                       req.remote_addr,
                                       std::string{}};

    // 单机版只接受来自 127.0.0.1 的请求，防御 DNS rebinding。
    if (context.mode == "local") {
      const std::string expected = "127.0.0.1:" + std::to_string(context.port);
      if (req.get_header_value("Host") != expected) {
        json_error(res, 403, "FORBIDDEN", "仅允许本机访问");
        return httplib::Server::HandlerResponse::Handled;
      }
    }

#ifdef XIAOTU_DEV_WEB
    // 开发态：前端跑在本地 dev server，允许其跨端口调用 API。
    const std::string origin = req.get_header_value("Origin");
    if (origin.rfind("http://127.0.0.1:", 0) == 0 || origin.rfind("http://localhost:", 0) == 0) {
      res.set_header("Access-Control-Allow-Origin", origin);
      res.set_header("Access-Control-Allow-Credentials", "true");
      res.set_header("Access-Control-Allow-Headers", "Content-Type, X-CSRF-Token, X-API-Version");
      res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
      res.set_header("Vary", "Origin");
    }
    if (req.method == "OPTIONS") {
      res.status = 204;
      return httplib::Server::HandlerResponse::Handled;
    }
#endif

    const std::string& path = req.path;
    const bool is_api = path.rfind("/api/", 0) == 0;

    // 能力协商：客户端带版本时只接受当前版本与 N-1，其余回 426 让其升级。
    const std::string client_api = req.get_header_value("X-API-Version");
    if (is_api && path != "/api/health" && !client_api.empty()) {
      int version = -1;
      try {
        version = std::stoi(client_api);
      } catch (const std::exception&) {
        version = -1;
      }
      if (version != gen::kApiVersion && version != gen::kApiVersion - 1) {
        json_error(res, 426, "API_VERSION_UNSUPPORTED",
                   "客户端 API 版本不兼容：服务端 " + std::to_string(gen::kApiVersion) +
                       "，客户端 " + client_api);
        return httplib::Server::HandlerResponse::Handled;
      }
    }
    if (!is_api) {
      return httplib::Server::HandlerResponse::Unhandled;
    }
    // 维护态（恢复向导已提交）：读请求放行，写请求明确拒绝，避免恢复期间继续下单。
    if (context.maintenance.load() && state_changing(req.method)) {
      json_error(res, 503, "MAINTENANCE_MODE", "正在进行数据库恢复，写操作已暂停，请稍后重试");
      return httplib::Server::HandlerResponse::Handled;
    }
    // 导入走原始二进制，其余 API 的请求体仍限制在 1MB。
    // pre-routing 阶段请求体尚未读取，只能按 Content-Length 预检。
    const bool is_import =
        path.size() > 7 && path.compare(path.size() - 7, 7, "/import") == 0;
    if (!is_import) {
      const std::string declared = req.get_header_value("Content-Length");
      std::size_t length = 0;
      try {
        length = declared.empty() ? 0 : static_cast<std::size_t>(std::stoull(declared));
      } catch (const std::exception&) {
        length = 0;
      }
      if (length > (1U << 20U)) {
        json_error(res, 413, "PAYLOAD_TOO_LARGE", "请求体超过 1MB 上限");
        return httplib::Server::HandlerResponse::Handled;
      }
    }
    if (path == "/api/health") {
      return httplib::Server::HandlerResponse::Unhandled;
    }
    // 更新检查在登录前也可能发生（网络版轻量握手之前），且不返回敏感数据。
    if (path == "/api/update/check") {
      return httplib::Server::HandlerResponse::Unhandled;
    }
    if (path == "/api/session" && req.method == "POST") {
      return httplib::Server::HandlerResponse::Unhandled;
    }
    // 单机版本机口令的设置与跳过发生在登录之前，按路径放行，
    // 但必须确认请求来自本机页面，否则可被跨站改写门禁。
    if ((path == "/api/local/password" || path == "/api/local/password/skip") &&
        req.method == "POST") {
      if (!local_page_request(req, context.port, context.mode)) {
        json_error(res, 403, "FORBIDDEN", "仅允许本机页面调用");
        return httplib::Server::HandlerResponse::Handled;
      }
      return httplib::Server::HandlerResponse::Unhandled;
    }
    // 用钥匙串里的长期凭据换会话同样发生在登录之前。
    if (path == "/api/local/remember" && req.method == "POST") {
      return httplib::Server::HandlerResponse::Unhandled;
    }
    if (path == "/api/login" && req.method == "POST") {
      return httplib::Server::HandlerResponse::Unhandled;
    }
    // 优雅关闭：只允许回环地址；未登录时用一次性令牌（X-Xiaotu-Shutdown-Token）
    // 鉴权，具体判定在路由内完成。已登录时继续走下面的 CSRF / 强制改密检查。
    if (path == "/api/system/shutdown" && req.method == "POST") {
      if (!shutdown_from_local_origin(req)) {
        json_error(res, 403, "FORBIDDEN", "仅允许本机访问");
        return httplib::Server::HandlerResponse::Handled;
      }
      if (!session.has_value()) {
        if (req.get_header_value("X-Xiaotu-Shutdown-Token").empty()) {
          json_error(res, 401, "UNAUTHORIZED", "未登录或会话已过期");
          return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
      }
    }
    if (!session.has_value()) {
      json_error(res, 401, "UNAUTHORIZED", "未登录或会话已过期");
      return httplib::Server::HandlerResponse::Handled;
    }
    // Cookie 会话的写操作必须带同步令牌，防止跨站表单直接调用 API。
    if (state_changing(req.method) &&
        !constant_time_equals(req.get_header_value("X-CSRF-Token"), session->csrf)) {
      json_error(res, 403, "CSRF_INVALID", "请求缺少有效的 CSRF 令牌");
      return httplib::Server::HandlerResponse::Handled;
    }
    // 强制改密：除改密、退出与查看自身信息外的接口一律拒绝，前端据此跳改密页。
    if (session->must_change_password && path != "/api/password/change" &&
        path != "/api/session/logout" && path != "/api/me") {
      json_error(res, 403, "MUST_CHANGE_PASSWORD", "请先修改口令后再使用其他功能");
      return httplib::Server::HandlerResponse::Handled;
    }
    return httplib::Server::HandlerResponse::Unhandled;
  });
}

}  // namespace xiaotu
