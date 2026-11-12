#include "xiaotu/http_server.h"

#include <httplib.h>

#include <exception>
#include <string>
#include <utility>

#include "xiaotu/context.h"
#include "xiaotu/error.h"
#include "xiaotu/http_json.h"
#include "xiaotu/ids.h"
#include "xiaotu/resources.h"
#include "xiaotu/routes.h"

namespace xiaotu {
namespace {

std::string cookie_value(const httplib::Request& req, const std::string& name) {
  const std::string header = req.get_header_value("Cookie");
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

}  // namespace

HttpServer::HttpServer(Services& services, ServerInfo info, std::string bootstrap_token)
    : services_(services),
      info_(std::move(info)),
      bootstrap_token_(std::move(bootstrap_token)),
      bootstrap_expire_(std::chrono::steady_clock::now() + std::chrono::minutes(10)),
      server_(std::make_unique<httplib::Server>()) {
  server_->set_payload_max_length(1U << 20U);
  server_->set_read_timeout(15, 0);
  server_->set_write_timeout(15, 0);
  server_->set_keep_alive_max_count(100);

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
        const auto session = sessions_.get(sid);
        current_context() = RequestContext{session ? session->user : std::string{},
                                           session ? session->role : std::string{},
                                           random_hex(8)};

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
        if (path == "/api/health") {
          return httplib::Server::HandlerResponse::Unhandled;
        }
        if (path == "/api/session" && req.method == "POST") {
          return httplib::Server::HandlerResponse::Unhandled;
        }
        if (!session.has_value()) {
          json_error(res, 401, "UNAUTHORIZED", "未登录或会话已过期");
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
    body["schema_version"] = services_.db.schema_version();
    body["embedded_resources"] = resources::count();
    json_ok(res, body);
  });

  // 一次性 token 换会话：单机由壳层带 #t=，网络版由启动日志输出的链接携带。
  server_->Post("/api/session", [this](const httplib::Request& req, httplib::Response& res) {
    const auto body = json_body(req);
    const std::string token = body.value("token", std::string{});
    if (!consume_bootstrap_token(token)) {
      json_error(res, 401, "UNAUTHORIZED", "访问凭据无效或已过期");
      return;
    }
    const std::string sid = sessions_.create("local-admin", "admin");
    res.set_header("Set-Cookie", "sid=" + sid + "; Path=/; HttpOnly; SameSite=Lax");
    nlohmann::json result;
    result["ok"] = true;
    result["user"] = "local-admin";
    result["role"] = "admin";
    json_ok(res, result);
  });

  server_->Post("/api/session/logout", [this](const httplib::Request& req,
                                              httplib::Response& res) {
    sessions_.erase(cookie_value(req, "sid"));
    res.set_header("Set-Cookie", "sid=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax");
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
