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
  // 诊断页展示用：数据目录、数据库文件与监听地址；不包含口令或密钥。
  std::string data_dir;
  std::string database_path;
  std::string listen_host;  // 单机版为空，监听 127.0.0.1 的自动端口
  // 网络版更新源（见 docs/更新模块技术方案.md）：清单文件由部署侧同步，
  // 服务端只做解析与判定；空 manifest_file 表示未启用更新检查。
  std::string update_manifest_file;
  std::string update_channel = "stable";
  std::string update_min_supported_version;
  int update_rollout_percent = 100;
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
