#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
  // 网络版更新源（见 docs/更新模块技术方案.md）：manifest_url 非空时由服务端
  // 拉取厂商清单并缓存，manifest_file 作为本地/离线清单兜底；两者都空表示未启用。
  std::string update_manifest_file;
  std::string update_manifest_url;
  std::string update_channel = "stable";
  std::string update_min_supported_version;
  int update_rollout_percent = 100;
  std::string update_public_key_file;
  std::string update_verifier_python = "python3";
  std::string update_verifier_script;
  // 网络版后台策略：auto_check 按 24h±2h 自动检查，prefetch 命中新版本后自动下载。
  bool update_auto_check = true;
  bool update_prefetch = true;
  // 启动参数（含 argv[0]）：自动替换后按原参数重启，不重新拼命令行。
  std::vector<std::string> launch_args;
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
  // 路由装配：构造只做传输层配置，具体路由按职责分文件注册。
  void register_health_routes();
  void register_update_routes();
  void register_restore_routes();
  void register_auth_routes();
  void register_local_auth_routes();
  void register_system_routes();
  void register_static_routes();
  bool consume_bootstrap_token(const std::string& token);
  // 单机版本机口令：直接复用 app_user 里的内置管理员口令，不另建表。
  // required 表示用户已在本机启用口令门禁，启动时先显示门禁页。
  bool local_password_required() const;
  // 已有业务数据、未设置口令且从未选择过跳过（新装无数据不询问）。
  bool local_password_prompt_pending() const;
  // 记录首次询问结果：true=设置口令并启用门禁，false=跳过。
  void save_local_password_choice(bool enabled);
  // 用服务端自身的版本与 schema 组装更新查询，前端不参与版本判定。
  UpdateQuery self_update_query() const;
  UpdateSettings update_settings() const;
  std::filesystem::path update_download_dir() const;
  // 网络版后台更新：启动后 30 秒做首轮检查，之后每 24h±2h；prefetch 打开时自动下载。
  void start_update_scheduler();
  void update_scheduler_loop();
  // 启动一次后台下载；已有任务在跑时返回 false。retry 为 true 时失败按 60s / 600s 退避重试。
  bool start_update_download(const UpdateSettings& settings, const std::string& manifest_file,
                             const UpdateManifest& manifest, bool retry);

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
  // 维护态：恢复向导提交后置位，其他写请求一律返回 MAINTENANCE_MODE，
  // 避免一边恢复一边下单。进程随优雅关闭退出，下次启动前完成文件替换。
  std::atomic<bool> maintenance_{false};
  // 更新下载任务：大安装包会超过 15 秒写超时，改为后台执行 + 轮询状态。
  std::mutex update_job_mutex_;
  std::condition_variable update_job_cv_;
  std::thread update_job_thread_;
  // idle / running / done / failed / canceled；canceling 只作为取消接口的即时响应。
  std::string update_job_state_ = "idle";
  std::string update_job_code_;
  std::string update_job_message_;
  std::string update_job_file_;
  std::uintmax_t update_job_bytes_ = 0;
  std::string update_job_at_;
  // 取消标志：置位后下载线程终止 curl、删除半成品并置 canceled，不再自动重试。
  std::atomic<bool> update_job_cancel_{false};
  // 自动检查线程：与下载线程分开，检查很轻，下载可能很慢。
  std::mutex update_scheduler_mutex_;
  std::condition_variable update_scheduler_cv_;
  std::thread update_scheduler_thread_;
  // 停止信号：唤醒定时等待与下载退避等待，避免退出时干等 10 分钟。
  std::atomic<bool> stopping_{false};
  std::unique_ptr<httplib::Server> server_;
  std::thread thread_;
  std::atomic<bool> serving_{false};
  int port_ = 0;
};

}  // namespace xiaotu
