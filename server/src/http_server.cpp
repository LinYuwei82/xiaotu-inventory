#include "xiaotu/http_server.h"

#include <httplib.h>

#include <chrono>
#include <condition_variable>
#include <ctime>
#include <exception>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>
#include <utility>

#include "local_auth_common.h"
#include "xiaotu/audit.h"
#include "xiaotu/auth_middleware.h"
#include "xiaotu/context.h"
#include "xiaotu/error.h"
#include "xiaotu/http_json.h"
#include "xiaotu/ids.h"
#include "xiaotu/log.h"
#include "xiaotu/masterdata_io.h"
#include "xiaotu/routes.h"
#include "xiaotu/services.h"
#include "xiaotu/update.h"
#include "xiaotu/update_install.h"
#include "xiaotu/update_service.h"

namespace xiaotu {
namespace {

// 单机版本机口令开关：键不存在=从未询问，'1'=已启用门禁，'0'=已选择跳过。
constexpr const char* kLocalPasswordSettingKey = "local_password_enabled";

// 导出入口：报表/主子表的 format=csv|xlsx，以及主数据 /export。
bool is_export_request(const httplib::Request& req) {
  if (req.method != "GET") {
    return false;
  }
  if (req.path.size() >= 7 && req.path.compare(req.path.size() - 7, 7, "/export") == 0) {
    return true;
  }
  if (!req.has_param("format")) {
    return false;
  }
  const std::string format = req.get_param_value("format");
  return format == "csv" || format == "xlsx";
}

// 审计里保留筛选条件，便于回答「导出了哪个期间、哪个仓库」；过长的查询串截断。
std::string query_text(const httplib::Request& req) {
  std::string text;
  for (const auto& [key, value] : req.params) {
    if (!text.empty()) {
      text += "&";
    }
    text += key + "=" + value;
  }
  if (text.size() > 400) {
    text.resize(400);
    text += "…";
  }
  return text;
}

}  // namespace

HttpServer::HttpServer(Services& services, ServerInfo info, std::string bootstrap_token)
    : services_(services),
      info_(std::move(info)),
      bootstrap_token_(std::move(bootstrap_token)),
      bootstrap_expire_(std::chrono::steady_clock::now() + std::chrono::minutes(10)),
      sessions_(services.db),
      server_(std::make_unique<httplib::Server>()) {
  // 优雅关闭的一次性令牌：写入数据目录下的隐藏文件（0600），成功后删除。
  // 日志只打印路径，绝不打印令牌本身。
  if (!info_.data_dir.empty()) {
    const std::filesystem::path token_file =
        std::filesystem::path(info_.data_dir) / ".shutdown-token";
    if (shutdown_token_.issue(token_file)) {
      log_info("关闭令牌已写入 " + token_file.string());
    } else {
      log_warn("关闭令牌写入失败，本次仅管理员会话可触发关闭: " + token_file.string());
    }
  }

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

  // 导出成功的请求在响应写出之前落审计（httplib 的 post_routing 钩子在
  // 响应行/头/体之前执行）：否则浏览器先收到 200、审计稍后才入库，
  // 客户端立刻查询会数不到，进程异常退出时也可能丢掉这条审计。
  // 失败或被权限拦下的请求不记；写审计失败只记日志，不改变已确定的响应。
  server_->set_post_routing_handler(
      [this](const httplib::Request& req, httplib::Response& res) {
        if (res.status != 200 || !is_export_request(req)) {
          return;
        }
        try {
          const std::string detail = query_text(req);
          services_.db.write([&](Conn& conn) {
            audit_write(conn, "export", "report", 0, req.path, detail);
          });
        } catch (const std::exception& error) {
          log_warn(std::string("导出审计写入失败: ") + error.what());
        }
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

  install_auth_middleware(*server_, AuthMiddlewareContext{services_, sessions_,
                                                          info_.mode, port_,
                                                          maintenance_});

  // 路由按职责分文件注册：构造只负责传输层配置与装配顺序。
  register_health_routes();
  register_update_routes();
  register_restore_routes();
  register_auth_routes();
  register_local_auth_routes();
  register_system_routes();
  // 先注册全部 API 业务路由，再注册静态资源兜底路由，避免正则把 /api/* 吞掉。
  register_routes();
  register_static_routes();
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
  start_update_scheduler();
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
  // 先唤醒定时等待与下载退避等待，并中止正在跑的下载，避免退出时等满 10 分钟。
  stopping_ = true;
  update_job_cancel_ = true;
  update_job_cv_.notify_all();
  update_scheduler_cv_.notify_all();
  if (shutdown_thread_.joinable()) {
    shutdown_thread_.join();
  }
  // 先停调度线程：它可能在退出瞬间再拉起一次下载，必须先确保不会再新增任务。
  if (update_scheduler_thread_.joinable()) {
    update_scheduler_thread_.join();
  }
  // 下载线程可能仍在跑 curl；停止监听后等它收尾，避免析构时访问已释放的成员。
  if (update_job_thread_.joinable()) {
    update_job_thread_.join();
  }
  // 关闭线程已经 stop 过监听套接字，这里不再重复调用，避免并发断言。
  if (!shutdown_started_.load() && server_) {
    server_->stop();
  }
  if (thread_.joinable()) {
    thread_.join();
  }
}

UpdateQuery HttpServer::self_update_query() const {
  UpdateQuery query;
  query.version = info_.version;
  query.build = current_build_number();
  query.channel = info_.update_channel.empty() ? "stable" : info_.update_channel;
#if defined(_WIN32)
  query.platform = "windows";
#elif defined(__APPLE__)
  query.platform = "macos";
#else
  query.platform = "linux";
#endif
  query.schema_version = services_.db.schema_version();
  query.api_version = current_api_version();
  // min_os 描述桌面客户端的最低系统版本，与服务端所在机器无关。
  query.ignore_min_os = true;
  // 服务端灰度分桶需要稳定的 install_id：同数据目录视为同一安装。
  query.install_id = "server:" + info_.data_dir;
  return query;
}

UpdateSettings HttpServer::update_settings() const {
  UpdateSettings settings;
  settings.manifest_file = info_.update_manifest_file;
  settings.manifest_url = info_.update_manifest_url;
  if (!settings.manifest_url.empty()) {
    // 远端清单缓存放在 updates/manifest/<channel>/ 下：remove_update_packages 只清理
    // updates/ 顶层的安装包，不会误删子目录里的清单。
    const std::string channel =
        info_.update_channel.empty() ? "stable" : info_.update_channel;
    settings.manifest_cache_file =
        (std::filesystem::path(info_.data_dir) / "updates" / "manifest" / channel /
         "manifest.json")
            .string();
  }
  settings.channel = info_.update_channel;
  settings.min_supported_version = info_.update_min_supported_version;
  settings.rollout_percent = info_.update_rollout_percent;
  settings.public_key_file = info_.update_public_key_file;
  settings.verifier_python = info_.update_verifier_python;
  settings.verifier_script = info_.update_verifier_script;
  return settings;
}

std::filesystem::path HttpServer::update_download_dir() const {
  return std::filesystem::path(info_.data_dir) / "updates";
}

void HttpServer::start_update_scheduler() {
  // 单机版的自动检查由 macOS 壳负责；这里只服务网络版，避免同一进程双份检查。
  if (info_.mode != "server" || !info_.update_auto_check ||
      (info_.update_manifest_file.empty() && info_.update_manifest_url.empty())) {
    return;
  }
  update_scheduler_thread_ = std::thread([this] { update_scheduler_loop(); });
}

void HttpServer::update_scheduler_loop() {
  using namespace std::chrono;
  std::mt19937 rng(static_cast<std::mt19937::result_type>(
      steady_clock::now().time_since_epoch().count()));
  // 首轮与单机版保持一致：启动后 30 秒再检查，避免和启动阶段的初始化抢资源。
  auto delay = seconds(30);
  std::unique_lock<std::mutex> lock(update_scheduler_mutex_);
  while (!stopping_.load()) {
    if (update_scheduler_cv_.wait_for(lock, delay, [this] { return stopping_.load(); })) {
      return;
    }
    lock.unlock();
    try {
      const UpdateSettings settings = update_settings();
      if (!settings.manifest_file.empty() || !settings.manifest_url.empty()) {
        const UpdateCheckResult result =
            check_for_update(settings, self_update_query(),
                             static_cast<std::int64_t>(std::time(nullptr)));
        if (!result.manifest_ok) {
          log_warn("更新：后台自动检查未通过（" + result.error_code + "）" +
                   result.error_message);
        } else if (!result.decision.update_available) {
          log_info("更新：后台自动检查完成，当前已是最新");
          // 已是最新：清掉历史安装包回收磁盘。持锁操作，避免与手动下载并发删到正在写的文件。
          std::lock_guard<std::mutex> job_lock(update_job_mutex_);
          if (update_job_state_ != "running") {
            if (update_job_state_ == "done") {
              update_job_state_ = "idle";
              update_job_code_.clear();
              update_job_message_.clear();
              update_job_file_.clear();
              update_job_bytes_ = 0;
              update_job_at_ = now_text();
            }
            remove_update_packages(update_download_dir());
          }
        } else {
          log_info("更新：后台自动检查发现新版本 " + result.manifest.version + "（build " +
                   std::to_string(result.manifest.build) + "）");
          if (info_.update_prefetch && !stopping_.load()) {
            // 同版本安装包已经下载并校验过就不再重复拉一遍；文件只会在校验通过后保留。
            const std::filesystem::path package =
                update_package_path(update_download_dir(), result.manifest);
            std::error_code exists_error;
            const bool cached =
                std::filesystem::exists(package, exists_error) &&
                (result.manifest.size <= 0 ||
                 (std::filesystem::file_size(package, exists_error) ==
                  static_cast<std::uintmax_t>(result.manifest.size)));
            if (cached) {
              log_info("更新：本地已有同版本安装包，跳过后台下载");
              // 只保留当前待装包，清掉切换版本后遗留的旧包。
              std::lock_guard<std::mutex> job_lock(update_job_mutex_);
              if (update_job_state_ != "running") {
                remove_update_packages(update_download_dir(), package);
              }
            } else {
              start_update_download(settings, result.manifest_path, result.manifest, true);
            }
          }
        }
      }
    } catch (const std::exception& error) {
      log_warn(std::string("更新：后台自动检查异常：") + error.what());
    }
    lock.lock();
    // 24h ± 2h：与单机版相同的抖动规则，避免多台服务器同时拉清单。
    const int jitter = static_cast<int>(rng() % (2 * 7200 + 1)) - 7200;
    delay = seconds(86400 + jitter);
  }
}

bool HttpServer::start_update_download(const UpdateSettings& settings,
                                       const std::string& manifest_file,
                                       const UpdateManifest& manifest, bool retry) {
  if (stopping_.load()) {
    return false;
  }
  std::thread previous;
  {
    std::lock_guard<std::mutex> lock(update_job_mutex_);
    if (stopping_.load() || update_job_state_ == "running") {
      return false;
    }
    previous = std::move(update_job_thread_);
    update_job_cancel_ = false;
    update_job_state_ = "running";
    update_job_code_.clear();
    update_job_message_.clear();
    update_job_file_.clear();
    update_job_bytes_ = 0;
    update_job_at_ = now_text();
  }
  // 先解锁再 join，避免上一轮线程收尾时还在等锁。
  if (previous.joinable()) {
    previous.join();
  }
  const std::filesystem::path directory = update_download_dir();
  update_job_thread_ = std::thread([this, settings, manifest_file, manifest, directory, retry]() {
    for (int attempt = 1; attempt <= 3; ++attempt) {
      if (stopping_.load() || update_job_cancel_.load()) {
        std::lock_guard<std::mutex> lock(update_job_mutex_);
        update_job_state_ = "canceled";
        update_job_code_ = "UPDATE_DOWNLOAD_CANCELED";
        update_job_message_ = stopping_.load() ? "服务正在停止，已中止后台下载" : "已取消下载";
        update_job_at_ = now_text();
        return;
      }
      const UpdatePackageResult downloaded = download_update_package(
          settings, manifest_file, manifest, directory, &update_job_cancel_);
      if (downloaded.error_code == "UPDATE_DOWNLOAD_CANCELED") {
        // 用户取消：删除半成品（download 内已完成），不再自动重试。
        std::lock_guard<std::mutex> lock(update_job_mutex_);
        update_job_state_ = "canceled";
        update_job_code_ = downloaded.error_code;
        update_job_message_ = downloaded.error_message;
        update_job_file_.clear();
        update_job_bytes_ = 0;
        update_job_at_ = now_text();
        return;
      }
      {
        std::lock_guard<std::mutex> lock(update_job_mutex_);
        update_job_state_ = downloaded.ok ? "done" : "failed";
        update_job_code_ = downloaded.error_code;
        update_job_message_ = downloaded.ok ? "下载并校验通过" : downloaded.error_message;
        update_job_file_ =
            downloaded.ok ? downloaded.path.filename().string() : std::string();
        update_job_bytes_ = downloaded.bytes;
        update_job_at_ = now_text();
        // 自动预下载失败时最多再退避重试 2 次；手动下载保持原有的一次性行为。
        if (!downloaded.ok && retry && attempt < 3) {
          update_job_state_ = "running";
          update_job_message_ = "第 " + std::to_string(attempt) +
                                " 次下载失败，稍后自动重试：" + downloaded.error_message;
        }
      }
      if (downloaded.ok || !retry || attempt >= 3) {
        return;
      }
      const auto backoff = attempt == 1 ? std::chrono::seconds(60)
                                        : std::chrono::seconds(600);
      std::unique_lock<std::mutex> lock(update_job_mutex_);
      if (update_job_cv_.wait_for(lock, backoff, [this] {
            return stopping_.load() || update_job_cancel_.load();
          })) {
        update_job_state_ = "canceled";
        update_job_code_ = "UPDATE_DOWNLOAD_CANCELED";
        update_job_message_ = stopping_.load() ? "服务正在停止，已中止后台下载" : "已取消下载";
        update_job_file_.clear();
        update_job_bytes_ = 0;
        update_job_at_ = now_text();
        return;
      }
    }
  });
  return true;
}

void HttpServer::register_routes() { register_all_routes(*server_, services_); }

bool HttpServer::local_password_required() const {
  if (info_.mode != "local") {
    return false;
  }
  try {
    const UserRow admin = services_.users.find_by_username("admin");
    if (!admin.active || !services_.users.has_password(admin.id)) {
      return false;
    }
    bool enabled = false;
    services_.db.read([&](Conn& conn) {
      enabled = read_setting_value(conn, kLocalPasswordSettingKey) == "1";
    });
    return enabled;
  } catch (const AppError&) {
    // 内置管理员缺失时按未启用处理，由 /api/session 的原有错误路径兜底。
    return false;
  }
}

bool HttpServer::local_password_prompt_pending() const {
  if (info_.mode != "local") {
    return false;
  }
  std::string choice;
  bool has_data = false;
  services_.db.read([&](Conn& conn) {
    choice = read_setting_value(conn, kLocalPasswordSettingKey);
    // 只用业务数据判断「老库」：新装会预置仓库、单位等基础种子，不能当作用户数据。
    Stmt stmt(conn,
              "SELECT EXISTS(SELECT 1 FROM item WHERE is_deleted = 0)"
              " OR EXISTS(SELECT 1 FROM partner WHERE is_deleted = 0)"
              " OR EXISTS(SELECT 1 FROM doc_header);");
    has_data = stmt.step() && stmt.i64(0) != 0;
  });
  if (choice == "0") {
    return false;
  }
  try {
    const UserRow admin = services_.users.find_by_username("admin");
    return has_data && admin.active && !services_.users.has_password(admin.id);
  } catch (const AppError&) {
    return false;
  }
}

void HttpServer::save_local_password_choice(bool enabled) {
  services_.db.write([&](Conn& conn) {
    write_setting_value(conn, kLocalPasswordSettingKey,
                        enabled ? std::string("1") : std::string("0"));
  });
}

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
