#include "xiaotu/http_server.h"

#include <httplib.h>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#include "xiaotu/audit.h"
#include "xiaotu/backup.h"
#include "xiaotu/context.h"
#include "xiaotu/edition.h"
#include "xiaotu/gen/edition_caps.h"
#include "xiaotu/error.h"
#include "xiaotu/http_json.h"
#include "xiaotu/ids.h"
#include "xiaotu/log.h"
#include "xiaotu/masterdata_io.h"
#include "xiaotu/migrations.h"
#include "xiaotu/permissions.h"
#include "xiaotu/resources.h"
#include "xiaotu/routes.h"
#include "xiaotu/update_service.h"

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

// 反向代理终结 TLS 时按 X-Forwarded-Proto 决定是否加 Secure。
// max_age_seconds 与空闲超时一致，服务端续期时同步延长。
std::string cookie_attributes(const httplib::Request& req, int max_age_seconds) {
  std::string attributes =
      "; Path=/; HttpOnly; SameSite=Strict; Max-Age=" + std::to_string(max_age_seconds);
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
          services_.db.write([&](sqlite3* conn) {
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

  server_->set_pre_routing_handler(
      [this](const httplib::Request& req, httplib::Response& res) {
        const std::string sid = cookie_value(req, "sid");
        bool renewed = false;
        const SessionPolicy policy = services_.settings.session_policy();
        const auto session =
            sessions_.get(sid, policy.idle_minutes, policy.absolute_hours, &renewed);
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
        if (path == "/api/login" && req.method == "POST") {
          return httplib::Server::HandlerResponse::Unhandled;
        }
        // 优雅关闭：只允许回环地址；未登录时用一次性令牌（X-Xiaotu-Shutdown-Token）
        // 鉴权，具体判定在路由内完成。已登录时继续走下面的 CSRF / 强制改密检查。
        if (path == "/api/system/shutdown" && req.method == "POST") {
          if (!is_loopback_address(req.remote_addr)) {
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

  server_->Get("/api/health", [this](const httplib::Request&, httplib::Response& res) {
    nlohmann::json body;
    body["status"] = "ok";
    body["app"] = info_.app_name;
    body["version"] = info_.version;
    body["mode"] = info_.mode;
    body["edition"] = std::string(EditionPolicy::instance().code());
    body["api_version"] = gen::kApiVersion;
    {
      nlohmann::json capabilities;
      capabilities["features"] = nlohmann::json::object();
      for (const auto& feature : gen::active_edition().features) {
        capabilities["features"][std::string(feature.key)] = std::string(feature.state);
      }
      capabilities["quotas"] = nlohmann::json::object();
      for (const auto& quota : gen::active_edition().quotas) {
        capabilities["quotas"][std::string(quota.key)] = quota.limit;
      }
      body["capabilities"] = capabilities;
    }
    body["schema_version"] = services_.db.schema_version();
    body["embedded_resources"] = resources::count();
    body["data_dir_warning"] = info_.data_dir_warning;

    const std::filesystem::path backup_dir =
        info_.backup_dir.empty() ? std::filesystem::path(info_.data_dir) / "backups"
                                 : std::filesystem::path(info_.backup_dir);
    const BackupStatus backup = backup_status(backup_dir);
    nlohmann::json backup_body;
    // 免登录接口：只给状态与文件名，不返回任何目录或绝对路径；
    // backup_dir_is_custom 仅说明是否单独指定了备份目录。
    backup_body["backup_dir_is_custom"] = info_.backup_dir_custom;
    if (backup.last_success_at.empty()) {
      backup_body["last_success"] = nullptr;
    } else {
      backup_body["last_success"] = {{"at", backup.last_success_at},
                                     {"file", backup.last_success_file},
                                     {"bytes", backup.last_success_bytes}};
    }
    if (backup.last_failure_at.empty()) {
      backup_body["last_failure"] = nullptr;
    } else {
      backup_body["last_failure"] = {{"at", backup.last_failure_at},
                                     {"reason", backup.last_failure_reason}};
    }
    backup_body["schema_version"] = backup.schema_version;
    backup_body["count"] = backup.backup_count;
    backup_body["daily_keep"] = kDailyBackupKeep;
    backup_body["monthly_keep"] = kMonthlyBackupKeep;
    backup_body["due"] = daily_backup_due(backup_dir);
    backup_body["database_writable"] = database_writable(services_.db);
    body["backup"] = backup_body;
    json_ok(res, body);
  });

  // 更新检查：免登录，只读；不采集匿名统计。清单由部署侧同步到本地文件，
  // 服务端不在此处发起 HTTPS 请求（见 docs/更新模块技术方案.md 第 5 节）。
  server_->Get("/api/update/check", [this](const httplib::Request& req, httplib::Response& res) {
    auto int_param = [&req](const char* name, std::int64_t fallback) -> std::int64_t {
      const std::string raw = req.get_param_value(name);
      if (raw.empty()) {
        return fallback;
      }
      try {
        return std::stoll(raw);
      } catch (const std::exception&) {
        return -1;
      }
    };
    UpdateQuery query;
    query.version = req.get_param_value("current_version");
    query.build = int_param("build", -1);
    query.channel = req.get_param_value("channel");
    query.platform = req.get_param_value("platform");
    query.schema_version = static_cast<int>(int_param("schema_version", -1));
    query.api_version = static_cast<int>(int_param("api_version", 0));
    query.os_version = req.get_param_value("os_version");
    query.install_id = req.get_param_value("install_id");
    if (query.version.empty() || query.build <= 0 || query.schema_version <= 0 ||
        query.channel.empty() || query.platform.empty()) {
      json_error(res, 400, "UPDATE_QUERY_INVALID",
                 "需要 current_version、build、channel、platform、schema_version");
      return;
    }

    UpdateSettings settings;
    settings.manifest_file = info_.update_manifest_file;
    settings.channel = info_.update_channel;
    settings.min_supported_version = info_.update_min_supported_version;
    settings.rollout_percent = info_.update_rollout_percent;
    settings.public_key_file = info_.update_public_key_file;
    settings.verifier_python = info_.update_verifier_python;
    settings.verifier_script = info_.update_verifier_script;
    const UpdateCheckResult result =
        check_for_update(settings, query, static_cast<std::int64_t>(std::time(nullptr)));
    if (!result.manifest_ok) {
      json_error(res, result.configured ? 503 : 200, result.error_code, result.error_message);
      if (!result.configured) {
        // 未配置更新源不是错误：返回“无更新”，客户端按正常节奏重试。
        res.body = nlohmann::json{{"ok", true},
                                  {"update_available", false},
                                  {"code", "UPDATE_NOT_CONFIGURED"},
                                  {"latest", nullptr}}
                       .dump();
        res.status = 200;
      }
      return;
    }

    nlohmann::json body;
    body["update_available"] = result.decision.update_available;
    body["mandatory"] = result.decision.mandatory;
    body["code"] = result.decision.code;
    body["mandatory_reason"] =
        result.decision.reason.empty() ? nlohmann::json(nullptr) : nlohmann::json(result.decision.reason);
    body["min_supported_version"] = info_.update_min_supported_version.empty()
                                        ? nlohmann::json(nullptr)
                                        : nlohmann::json(info_.update_min_supported_version);
    body["check_after_seconds"] = 86400;
    body["server_version"] = info_.version;
    body["api_version"] = current_api_version();
    if (result.decision.intermediate_required) {
      body["intermediate"] = {{"version", result.decision.intermediate.version},
                              {"build", result.decision.intermediate.build},
                              {"schema", result.decision.intermediate.schema}};
    } else {
      body["intermediate"] = nullptr;
    }
    if (result.decision.update_available) {
      body["latest"] = {{"version", result.manifest.version},
                        {"build", result.manifest.build},
                        {"url", result.manifest.url},
                        {"sha256", result.manifest.sha256},
                        {"size", result.manifest.size},
                        {"published_at", result.manifest.published_at},
                        {"release_notes_url", result.manifest.release_notes_url},
                        {"mandatory", result.manifest.mandatory},
                        {"mandatory_deadline", result.manifest.mandatory_deadline.empty()
                                                   ? nlohmann::json(nullptr)
                                                   : nlohmann::json(result.manifest.mandatory_deadline)}};
    } else {
      body["latest"] = nullptr;
    }
    json_ok(res, body);
  });

  // 关于与诊断：只读，任何已登录角色都能看；不返回口令、密钥、会话令牌。
  server_->Get("/api/diagnostics", [this](const httplib::Request&, httplib::Response& res) {
    std::uintmax_t database_bytes = 0;
    if (!info_.database_path.empty()) {
      std::error_code ignored;
      database_bytes = std::filesystem::file_size(info_.database_path, ignored);
      if (ignored) {
        database_bytes = 0;
      }
    }
    const std::string listen =
        info_.mode == "server"
            ? (info_.listen_host.empty() ? std::string("127.0.0.1") : info_.listen_host) +
                  ":" + std::to_string(port_)
            : "127.0.0.1:" + std::to_string(port_);

    nlohmann::json body;
    body["app"] = info_.app_name;
    body["version"] = info_.version;
    body["mode"] = info_.mode;
    body["edition"] = {{"code", std::string(EditionPolicy::instance().code())},
                       {"label", std::string(EditionPolicy::instance().label())}};
    body["schema_version"] = services_.db.schema_version();
    body["migrations"] = all_migrations().size();
    // __VERSION__ 是 GCC/Clang 专有宏，MSVC 下未定义；按编译器分流给出等价信息。
#if defined(_MSC_VER)
    body["build"] = {{"compiler", "MSVC " + std::to_string(_MSC_FULL_VER)},
#else
    body["build"] = {{"compiler", __VERSION__},
#endif
                     {"date", __DATE__},
                     {"time", __TIME__},
                     {"standard", std::to_string(__cplusplus)},
#ifdef NDEBUG
                     {"profile", "release"}};
#else
                     {"profile", "debug"}};
#endif
    body["data_dir"] = info_.data_dir;
    body["database_path"] = info_.database_path;
    body["backup_dir"] = info_.backup_dir.empty()
                             ? (std::filesystem::path(info_.data_dir) / "backups").string()
                             : info_.backup_dir;
    body["database_bytes"] = database_bytes;
    body["listen"] = listen;
    body["user"] = current_context().user;
    body["role"] = current_context().role;
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
    body["must_change_password"] = context.must_change_password;
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
    const SessionPolicy policy = services_.settings.session_policy();
    const CreatedSession session = sessions_.create(admin.id, policy.idle_minutes);
    services_.db.write([&](sqlite3* conn) {
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

  // 优雅关闭：只允许回环地址；管理员会话或一次性令牌二选一。
  // 先写审计与响应，再在独立线程里 stop：在 handler 线程内 stop 会等待自身
  // 所在的连接/任务队列，造成死锁；在途请求由 httplib 的任务队列排空后自然完成。
  server_->Post("/api/system/shutdown", [this](const httplib::Request& req,
                                               httplib::Response& res) {
    if (!is_loopback_address(req.remote_addr)) {
      json_error(res, 403, "FORBIDDEN", "仅允许本机访问");
      return;
    }
    const std::string& role = current_context().role;
    bool authorized = false;
    const char* source = "";
    if (!role.empty() && role == "admin" && role_has_permission(role, "setting.write")) {
      authorized = true;
      source = "session";
    } else if (shutdown_token_.consume(req.get_header_value("X-Xiaotu-Shutdown-Token"))) {
      authorized = true;
      source = "token";
    }
    if (!authorized) {
      if (role.empty()) {
        json_error(res, 401, "UNAUTHORIZED", "未登录或会话已过期");
      } else {
        json_error(res, 403, "FORBIDDEN", "仅管理员可关闭服务");
      }
      return;
    }
    // 审计写失败不应阻止关闭。
    try {
      std::string detail = std::string("来源=") + source + " 客户端=" + req.remote_addr;
      services_.db.write([&](sqlite3* conn) {
        audit_write(conn, "shutdown", "system", 0, "", detail);
      });
    } catch (const std::exception& error) {
      log_warn(std::string("关闭审计写入失败: ") + error.what());
    }
    // 重复调用保持幂等：只触发一次 stop。
    if (!shutdown_started_.exchange(true)) {
      shutdown_thread_ = std::thread([this] {
        // 给响应写出留出时间，避免关闭监听套接字时打断正在回写的响应。
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        server_->stop();
      });
    }
    json_ok(res, nlohmann::json{{"status", "stopping"}});
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
  if (shutdown_thread_.joinable()) {
    shutdown_thread_.join();
  }
  // 关闭线程已经 stop 过监听套接字，这里不再重复调用，避免并发断言。
  if (!shutdown_started_.load() && server_) {
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
