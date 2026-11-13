#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "xiaotu/login_limiter.h"
#include "xiaotu/services.h"
#include "xiaotu/session_store.h"
#include "xiaotu/system_shutdown.h"
#include "xiaotu/update.h"
#include "xiaotu/update_service.h"

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
  // 解析后的备份目录（诊断页展示）；backup_dir_custom 供免登录 health 说明是否单独指定，
  // health 本身不输出任何目录或绝对路径。
  std::string backup_dir;
  bool backup_dir_custom = false;
  // 网络版更新源（见 docs/更新模块技术方案.md）：清单文件由部署侧同步，
  // 服务端只做解析与判定；空 manifest_file 表示未启用更新检查。
  std::string update_manifest_file;
  std::string update_channel = "stable";
  std::string update_min_supported_version;
  int update_rollout_percent = 100;
  std::string update_public_key_file;
  std::string update_verifier_python = "python3";
  std::string update_verifier_script;
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
  // listen 是否仍在运行：主循环据此在服务被停止（如 /api/system/shutdown）后退出。
  bool serving() const { return serving_.load(); }

 private:
  void register_routes();
  bool consume_bootstrap_token(const std::string& token);
  // 用服务端自身的版本与 schema 组装更新查询，前端不参与版本判定。
  UpdateQuery self_update_query() const;
  UpdateSettings update_settings() const;
  std::filesystem::path update_download_dir() const;

  Services& services_;
  ServerInfo info_;
  std::string bootstrap_token_;
  std::chrono::steady_clock::time_point bootstrap_expire_;

  std::mutex bootstrap_mutex_;
  bool bootstrap_used_ = false;

  SessionStore sessions_;
  LoginLimiter login_limiter_;
  // 优雅关闭的一次性令牌：启动时写入 <data_dir>/.shutdown-token。
  ShutdownToken shutdown_token_;
  std::atomic<bool> shutdown_started_{false};
  std::thread shutdown_thread_;
  // 更新下载任务：大安装包会超过 15 秒写超时，改为后台执行 + 轮询状态。
  std::mutex update_job_mutex_;
  std::thread update_job_thread_;
  std::string update_job_state_ = "idle";  // idle / running / done / failed
  std::string update_job_code_;
  std::string update_job_message_;
  std::string update_job_file_;
  std::uintmax_t update_job_bytes_ = 0;
  std::string update_job_at_;
  std::unique_ptr<httplib::Server> server_;
  std::thread thread_;
  std::atomic<bool> serving_{false};
  int port_ = 0;
};

}  // namespace xiaotu
