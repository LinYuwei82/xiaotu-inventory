#include "xiaotu/http_server.h"

#include <httplib.h>

#include <chrono>
#include <exception>
#include <string>
#include <utility>

#include "xiaotu/context.h"
#include "xiaotu/edition.h"
#include "xiaotu/error.h"
#include "xiaotu/http_json.h"
#include "xiaotu/ids.h"
#include "xiaotu/log.h"
#include "xiaotu/masterdata_io.h"
#include "xiaotu/permissions.h"
#include "xiaotu/resources.h"
#include "xiaotu/routes.h"

namespace xiaotu {
namespace {

std::string cookie_value(const httplib::Request& req, const std::string& name) {  const std::string header = req.get_header_value("Cookie");
  std::size_t offset = 0;
  while (offset < header.size()) {
    const std::size_t end = header.find(';', offset);
    const std::string part =
        header.substr(offset, end == std::string::npos ? std::string::npos : end - offset);
    const std::size_t equals = part.find('=');
    if (equals != std::string::npos) {
      std::string key = part.substr(0, equals);
      const std::size_t first = key.find_first_not_of(' ');
      if (first != std::string::npos) {
        key = key.substr(first);
      }
      if (key == name) {
        return part.substr(equals + 1);
      }
    }
    if (end == std::string::npos) {
      break;
    }
    offset = end + 1;
  }
  return {};
}

bool state_changing(const std::string& method) {
  return method == "POST" || method == "PUT" || method == "DELETE" || method == "PATCH";
}

// 反向代理终结 TLS 时按 X-Forwarded-Proto 决定是否加 Secure。
std::string cookie_attributes(const httplib::Request& req) {
  std::string attributes = "; Path=/; HttpOnly; SameSite=Strict; Max-Age=28800";
  if (req.get_header_value("X-Forwarded-Proto") == "https") {
    attributes += "; Secure";
  }
  return attributes;
}

}  // namespace

HttpServer::HttpServer(Services& services, ServerInfo info, std::string bootstrap_token)
    : services_(services),
      info_(std::move(info)),
      bootstrap_token_(std::move(bootstrap_token)),
      bootstrap_expire_(std::chrono::steady_clock::now() + std::chrono::minutes(10)),
      sessions_(services.db),
      server_(std::make_unique<httplib::Server>()) {
  // 主数据导入按原始二进制上传，只给导入路径放宽到 5MB；
  // 其它接口仍在 pre-routing 里按 1MB 拦截，避免整体放大请求体上限。
  server_->set_payload_max_length(kImportMaxBytes);
  server_->set_read_timeout(15, 0);
  server_->set_write_timeout(15, 0);
  // 每个连接只处理一个请求（httplib 的计数为 0 表示一个都不处理，所以用 1）：
  // httplib 的线程池按连接占用线程，空闲长连接会把工作线程占到 keep-alive 超时
  // （默认 5s）为止；浏览器每个页面会开多条连接，用户一多新请求就要排队数秒。
  // 关掉复用后线程只在处理请求时被占用，局域网新建连接的开销远小于排队等待。
  server_->set_keep_alive_max_count(1);

  // 访问日志只记录方法、路径、状态与请求标识；查询串、Cookie 与请求体不落盘。
  server_->set_logger([](const httplib::Request& req, const httplib::Response& res) {
    const auto& context = current_context();
    std::string line = req.method + " " + req.path + " " + std::to_string(res.status);
    if (!context.request_id.empty()) {
      line += " req=" + context.request_id;
    }
    log_message(LogLevel::Info, line);
  });

  server_->set_exception_handler([](const httplib::Request&, httplib::Response& res,
                                    std::exception_ptr ep) {
    try {
      std::rethrow_exception(ep);
    } catch (const AppError& error) {
      json_from_exception(res, error);
    } catch (const std::exception& error) {
      json_from_exception(res, error);
    } catch (...) {
      json_error(res, 500, "INTERNAL", "服务器内部错误");
    }
  });

  server_->set_pre_routing_handler(
      [this](const httplib::Request& req, httplib::Response& res) {
        const std::string sid = cookie_value(req, "sid");
        bool renewed = false;
        const auto session = sessions_.get(sid, &renewed);
        if (renewed) {
          // 服务端滑动续期后同步延长浏览器 Cookie，否则 Cookie 会比会话先过期。
          res.set_header("Set-Cookie", "sid=" + sid + cookie_attributes(req));
        }
        current_context() = RequestContext{session ? session->user : std::string{},
                                           session ? session->role : std::string{},
                                           random_hex(8),
                                           session ? session->csrf : std::string{}};

        // 单机版只接受来自 127.0.0.1 的请求，防御 DNS rebinding。
        if (info_.mode == "local") {
          const std::string expected = "127.0.0.1:" + std::to_string(port_);
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
          res.set_header("Access-Control-Allow-Headers", "Content-Type");
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
        if (!is_api) {
          return httplib::Server::HandlerResponse::Unhandled;
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
        if (path == "/api/session" && req.method == "POST") {
          return httplib::Server::HandlerResponse::Unhandled;
        }
        if (path == "/api/login" && req.method == "POST") {
          return httplib::Server::HandlerResponse::Unhandled;
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
        return httplib::Server::HandlerResponse::Unhandled;
      });

  server_->Get("/api/health", [this](const httplib::Request&, httplib::Response& res) {
    nlohmann::json body;
    body["status"] = "ok";
    body["app"] = info_.app_name;
    body["version"] = info_.version;
    body["mode"] = info_.mode;
    body["edition"] = std::string(EditionPolicy::instance().code());
    body["schema_version"] = services_.db.schema_version();
    body["embedded_resources"] = resources::count();
    body["data_dir_warning"] = info_.data_dir_warning;
    json_ok(res, body);
  });

  server_->Get("/api/me", [this](const httplib::Request&, httplib::Response& res) {
    const auto& context = current_context();
    nlohmann::json body;
    body["user"] = context.user;
    body["role"] = context.role;
    body["csrf"] = context.csrf;
    // 前端据此隐藏无权限的入口；真正的拦截仍在路由的 guard 里。
    body["permissions"] = role_permissions(context.role);
    json_ok(res, body);
  });

  // 一次性 token 换会话：仅单机版使用；网络版必须口令登录。
  server_->Post("/api/session", [this](const httplib::Request& req, httplib::Response& res) {
    if (info_.mode != "local") {
      json_error(res, 403, "FORBIDDEN", "网络版请使用用户名与口令登录");
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
    const CreatedSession session = sessions_.create(admin.id);
    res.set_header("Set-Cookie", "sid=" + session.id + cookie_attributes(req));
    nlohmann::json result;
    result["ok"] = true;
    result["user"] = admin.username;
    result["role"] = admin.role;
    result["csrf"] = session.csrf;
    json_ok(res, result);
  });

  // 口令登录：失败计数按来源 IP + 用户名限流，成功即清零。
  server_->Post("/api/login", [this](const httplib::Request& req, httplib::Response& res) {
    const auto body = json_body(req);
    const std::string username = body.value("username", std::string{});
    const std::string password = body.value("password", std::string{});
    const std::string key = req.remote_addr + "|" + username;
    if (login_limiter_.blocked(key)) {
      res.set_header("Retry-After", "300");
      json_error(res, 429, "TOO_MANY_ATTEMPTS", "登录失败次数过多，请 5 分钟后再试");
      return;
    }
    UserRow user;
    try {
      user = services_.users.authenticate(username, password);
    } catch (const AppError& error) {
      if (error.code() != "INVALID_CREDENTIALS") {
        throw;
      }
      login_limiter_.record_failure(key);
      json_error(res, 401, "INVALID_CREDENTIALS", "用户名或口令错误");
      return;
    }
    login_limiter_.reset(key);
    // 登录成功后顺手清掉过期会话，不需要额外的定时任务。
    sessions_.purge_expired();
    const CreatedSession session = sessions_.create(user.id);
    res.set_header("Set-Cookie", "sid=" + session.id + cookie_attributes(req));
    nlohmann::json result;
    result["ok"] = true;
    result["user"] = user.username;
    result["role"] = user.role;
    result["csrf"] = session.csrf;
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

  // 先注册全部 API 业务路由，再注册静态资源兜底路由，避免正则把 /api/* 吞掉。
  register_routes();

  server_->Get("/favicon.ico", [](const httplib::Request&, httplib::Response& res) {
    res.status = 204;
  });

  server_->Get("/", [](const httplib::Request&, httplib::Response& res) {
    resources::Resource resource;
    if (!resources::find("/index.html", resource)) {
      json_error(res, 503, "NO_WEB_ASSET", "未内嵌前端资源（开发态请使用 dev server）");
      return;
    }
    res.set_content(reinterpret_cast<const char*>(resource.data), resource.size,
                    std::string(resource.content_type));
    res.set_header("Cache-Control", "no-cache");
  });

  server_->Get(R"(/([A-Za-z0-9_.\-/]+))",
               [](const httplib::Request& req, httplib::Response& res) {
                 resources::Resource resource;
                 if (!resources::find("/" + req.matches[1].str(), resource)) {
                   res.status = 404;
                   res.set_content("Not Found", "text/plain; charset=utf-8");
                   return;
                 }
                 res.set_content(reinterpret_cast<const char*>(resource.data), resource.size,
                                 std::string(resource.content_type));
                 res.set_header("Cache-Control", "no-cache");
               });
}

HttpServer::~HttpServer() { stop(); }

int HttpServer::bind(const std::string& host, int port) {
  int actual = -1;
  if (port == 0) {
    actual = server_->bind_to_any_port(host);
  } else if (server_->bind_to_port(host, port)) {
    actual = port;
  }
  if (actual <= 0) {
    throw AppError("BIND_FAILED", "无法监听 " + host + ":" + std::to_string(port));
  }
  port_ = actual;
  return actual;
}

void HttpServer::serve_async() {
  serving_ = true;
  thread_ = std::thread([this] {
    server_->listen_after_bind();
    serving_ = false;
  });
}

void HttpServer::serve_blocking() {
  serving_ = true;
  server_->listen_after_bind();
  serving_ = false;
}

void HttpServer::stop() {
  if (server_) {
    server_->stop();
  }
  if (thread_.joinable()) {
    thread_.join();
  }
}

void HttpServer::register_routes() { register_all_routes(*server_, services_); }

bool HttpServer::consume_bootstrap_token(const std::string& token) {
  std::lock_guard<std::mutex> lock(bootstrap_mutex_);
  if (bootstrap_used_ || token.empty()) {
    return false;
  }
  if (std::chrono::steady_clock::now() > bootstrap_expire_) {
    return false;
  }
  if (!constant_time_equals(token, bootstrap_token_)) {
    return false;
  }
  bootstrap_used_ = true;
  return true;
}

}  // namespace xiaotu
