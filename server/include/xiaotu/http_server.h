#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "xiaotu/login_limiter.h"
#include "xiaotu/services.h"
#include "xiaotu/session_store.h"

namespace httplib {
class Server;
}

namespace xiaotu {

struct ServerInfo {
  std::string mode;  // "local" / "server"
  std::string app_name;
  std::string version;
  // 启动时检测到的数据目录风险提示（如位于 iCloud / OneDrive 同步目录），
  // 空串表示正常；前端在 /api/health 里读取后显示横幅。
  std::string data_dir_warning;
};

// HTTP 外壳：只负责认证、路由注册与生命周期，业务全部在 Services 内。
class HttpServer {
 public:
  HttpServer(Services& services, ServerInfo info, std::string bootstrap_token);
  ~HttpServer();

  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  // port = 0 时由系统分配；返回实际端口。
  int bind(const std::string& host, int port);
  void serve_async();
  void serve_blocking();
  void stop();

  int port() const { return port_; }

 private:
  void register_routes();
  bool consume_bootstrap_token(const std::string& token);

  Services& services_;
  ServerInfo info_;
  std::string bootstrap_token_;
  std::chrono::steady_clock::time_point bootstrap_expire_;

  std::mutex bootstrap_mutex_;
  bool bootstrap_used_ = false;

  SessionStore sessions_;
  LoginLimiter login_limiter_;
  std::unique_ptr<httplib::Server> server_;
  std::thread thread_;
  std::atomic<bool> serving_{false};
  int port_ = 0;
};

}  // namespace xiaotu
