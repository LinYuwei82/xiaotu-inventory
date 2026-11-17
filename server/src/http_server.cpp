#include "xiaotu/http_server.h"

#include <httplib.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>
#include <utility>

#include "xiaotu/audit.h"
#include "xiaotu/backup.h"
#include "xiaotu/context.h"
#include "xiaotu/edition.h"
#include "xiaotu/gen/edition_caps.h"
#include "xiaotu/error.h"
#include "xiaotu/http_guard.h"
#include "xiaotu/http_json.h"
#include "xiaotu/ids.h"
#include "xiaotu/log.h"
#include "xiaotu/masterdata_io.h"
#include "xiaotu/migrations.h"
#include "xiaotu/permissions.h"
#include "xiaotu/resources.h"
#include "xiaotu/restore_wizard.h"
#include "xiaotu/routes.h"
#include "xiaotu/token_hash.h"
#include "xiaotu/update.h"
#include "xiaotu/update_install.h"
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

// 单机版本机口令开关：键不存在=从未询问，'1'=已启用门禁，'0'=已选择跳过。
constexpr const char* kLocalPasswordSettingKey = "local_password_enabled";

std::string read_setting_value(sqlite3* conn, const char* key) {
  Stmt stmt(conn, "SELECT value FROM app_setting WHERE key = ?;");
  stmt.bind(1, key);
  return stmt.step() ? stmt.text(0) : std::string{};
}

// 单机版「本机免输」：钥匙串里放服务端签发的随机 token，这里只存哈希与时间。
constexpr const char* kLocalRememberSettingKey = "local_remember_enabled";
constexpr int kLocalRememberDays = 30;

void write_setting_value(sqlite3* conn, const char* key, const std::string& value) {
  Stmt stmt(conn,
            "INSERT INTO app_setting (key, value, updated_at, updated_by)"
            " VALUES (?, ?, ?, 'system')"
            " ON CONFLICT(key) DO UPDATE SET value = excluded.value,"
            " updated_at = excluded.updated_at;");
  stmt.bind(1, key).bind(2, value).bind(3, now_text()).run();
}

// 默认 30 天免输；只有显式写 '0' 才表示「每次都要输」。
bool local_remember_enabled(Db& db) {
  bool enabled = true;
  db.read([&](sqlite3* conn) {
    enabled = read_setting_value(conn, kLocalRememberSettingKey) != "0";
  });
  return enabled;
}

bool has_local_remember_token(Db& db) {
  bool exists = false;
  db.read([&](sqlite3* conn) {
    Stmt stmt(conn, "SELECT EXISTS(SELECT 1 FROM local_remember_token);");
    exists = stmt.step() && stmt.i64(0) != 0;
  });
  return exists;
}

struct IssuedRememberToken {
  std::string token;
  std::string expires_at;
};

// 同一时刻只保留一枚有效凭据：签发新凭据即让旧凭据失效。
IssuedRememberToken issue_local_remember_token(Db& db, std::int64_t user_id,
                                               const std::string& username) {
  IssuedRememberToken issued;
  issued.token = random_hex(32);
  const std::string digest = hash_token(issued.token);
  db.write([&](sqlite3* conn) {
    Stmt clear(conn, "DELETE FROM local_remember_token;");
    clear.run();
    Stmt insert(conn,
                "INSERT INTO local_remember_token"
                " (token_hash, issued_at, expires_at, last_used_at)"
                " VALUES (?, datetime('now'), datetime('now', ?), datetime('now'));");
    insert.bind(1, digest)
        .bind(2, std::string("+") + std::to_string(kLocalRememberDays) + " days")
        .run();
    Stmt select(conn, "SELECT expires_at FROM local_remember_token WHERE token_hash = ?;");
    select.bind(1, digest);
    if (select.step()) {
      issued.expires_at = select.text(0);
    }
    audit_write_as(conn, username, "remember_issue", "auth", user_id, username,
                   "签发本机免输凭据");
  });
  return issued;
}

void clear_local_remember_tokens(Db& db, std::int64_t user_id, const std::string& username,
                                 const char* detail) {
  db.write([&](sqlite3* conn) {
    Stmt clear(conn, "DELETE FROM local_remember_token;");
    clear.run();
    audit_write_as(conn, username, "remember_clear", "auth", user_id, username, detail);
  });
}

// 到期与时钟回拨都在服务端判断：
// - now >= expires_at 视为过期；
// - now < issued_at 或 now < last_used_at 说明系统时间被回拨，一律作废，
//   不顺着回拨延长有效期。
bool validate_local_remember_token(Db& db, const std::string& token, std::string* failure) {
  if (token.empty()) {
    *failure = "缺少本机免输凭据";
    return false;
  }
  const std::string digest = hash_token(token);
  bool found = false;
  bool expired = false;
  bool rolled_back = false;
  db.read([&](sqlite3* conn) {
    Stmt stmt(conn,
              "SELECT expires_at <= datetime('now'),"
              " issued_at > datetime('now'),"
              " last_used_at > datetime('now')"
              " FROM local_remember_token WHERE token_hash = ? LIMIT 1;");
    stmt.bind(1, digest);
    if (stmt.step()) {
      found = true;
      expired = stmt.i64(0) != 0;
      rolled_back = stmt.i64(1) != 0 || stmt.i64(2) != 0;
    }
  });
  if (!found) {
    *failure = "本机免输凭据无效，请重新输入口令";
    return false;
  }
  if (expired || rolled_back) {
    db.write([&](sqlite3* conn) {
      Stmt stmt(conn, "DELETE FROM local_remember_token WHERE token_hash = ?;");
      stmt.bind(1, digest).run();
    });
    *failure = rolled_back ? "系统时间早于凭据签发时间，本机免输已失效"
                           : "本机免输已过期，请重新输入口令";
    return false;
  }
  db.write([&](sqlite3* conn) {
    Stmt stmt(conn,
              "UPDATE local_remember_token SET last_used_at = datetime('now')"
              " WHERE token_hash = ?;");
    stmt.bind(1, digest).run();
  });
  return true;
}

// 恢复只接受备份目录内的文件名：拒绝路径分隔符与 . / ..，避免调到任意文件。
bool safe_backup_name(const std::string& name) {
  if (name.empty() || name == "." || name == "..") {
    return false;
  }
  if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
    return false;
  }
  return std::filesystem::path(name).filename().string() == name;
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
        // 维护态（恢复向导已提交）：读请求放行，写请求明确拒绝，避免恢复期间继续下单。
        if (maintenance_.load() && state_changing(req.method)) {
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
        // 单机版本机口令的设置与跳过发生在登录之前，按路径放行，具体判定在路由内。
        if ((path == "/api/local/password" || path == "/api/local/password/skip") &&
            req.method == "POST") {
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
    // 轻量握手：桌面壳据此判断是否需要阻断旧客户端；未配置时为空。
    body["min_supported_version"] = info_.update_min_supported_version.empty()
                                        ? nlohmann::json(nullptr)
                                        : nlohmann::json(info_.update_min_supported_version);
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
    // 单机版本机口令门禁：前端据此决定是否先显示口令页或首次设置提示。
    body["local_gate"] = {{"password_required", local_password_required()},
                          {"prompt_set_password", local_password_prompt_pending()},
                          {"remember_enabled", local_remember_enabled(services_.db)}};

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

    const UpdateSettings settings = update_settings();
    const UpdateCheckResult result =
        check_for_update(settings, query, static_cast<std::int64_t>(std::time(nullptr)));
    if (!result.manifest_ok) {
      json_error(res, result.configured ? 503 : 200, result.error_code, result.error_message);
      if (!result.configured) {
        // 未配置更新源不是错误：返回“无更新”，客户端按正常节奏重试。
        res.body = nlohmann::json{{"ok", true},
                                  {"update_available", false},
                                  {"code", "UPDATE_NOT_CONFIGURED"},
                                  {"configured", false},
                                  {"message", result.error_message},
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

  // 更新状态：设置页用。版本、schema 与平台全部由服务端自身给出，
  // 前端只展示「当前版本 / 最新版本 / 发布日期 / 是否已是最新 / 失败原因」。
  server_->Get("/api/update/status",
               guard("setting.read", [this](const httplib::Request&, httplib::Response& res) {
                 const UpdateQuery query = self_update_query();
                 const UpdateCheckResult result = check_for_update(
                     update_settings(), query, static_cast<std::int64_t>(std::time(nullptr)));
                 nlohmann::json body;
                 body["ok"] = true;
                 body["configured"] = result.configured;
                 // 清单来源与回退提示：network=本次刚拉取，cache=本地缓存（可能是网络失败后的回退），
                 // file=部署侧同步的本地清单。
                 body["manifest_source"] = result.manifest_source.empty()
                                               ? nlohmann::json(nullptr)
                                               : nlohmann::json(result.manifest_source);
                 body["manifest_stale"] = result.manifest_stale;
                 body["manifest_warning"] = result.manifest_warning.empty()
                                                ? nlohmann::json(nullptr)
                                                : nlohmann::json(result.manifest_warning);
                 // 后台策略只读展示：由 config.toml 的 [update] 段决定，运行中不提供切换。
                 body["auto_check"] = info_.update_auto_check;
                 body["prefetch"] = info_.update_prefetch;
                 body["current"] = {{"version", info_.version},
                                    {"build", current_build_number()},
                                    {"schema_version", services_.db.schema_version()},
                                    {"platform", query.platform}};
                 body["update_available"] = result.manifest_ok && result.decision.update_available;
                 // 强制标记可能来自清单 mandatory，也可能来自服务端 min_supported_version；
                 // 与 latest.mandatory（仅清单字段）分开，界面以这个为准。
                 body["mandatory"] = result.manifest_ok && result.decision.mandatory;
                 body["mandatory_reason"] = result.manifest_ok && !result.decision.reason.empty()
                                                ? nlohmann::json(result.decision.reason)
                                                : nlohmann::json(nullptr);
                 body["min_supported_version"] = info_.update_min_supported_version.empty()
                                                     ? nlohmann::json(nullptr)
                                                     : nlohmann::json(info_.update_min_supported_version);
                 if (result.manifest_ok) {
                   body["up_to_date"] = result.decision.code == "NO_UPDATE";
                   body["code"] = result.decision.code;
                   body["error"] = nullptr;
                 } else {
                   body["up_to_date"] = nullptr;
                   body["code"] = result.error_code;
                   body["error"] = {{"code", result.error_code},
                                     {"message", result.error_message}};
                 }
                 if (result.manifest_ok && result.decision.update_available) {
                   body["latest"] = {{"version", result.manifest.version},
                                     {"build", result.manifest.build},
                                     {"published_at", result.manifest.published_at},
                                     {"release_notes_url", result.manifest.release_notes_url},
                                     {"size", result.manifest.size},
                                     {"sha256", result.manifest.sha256},
                                     {"mandatory", result.manifest.mandatory}};
                 } else {
                   body["latest"] = nullptr;
                 }
                 {
                   std::lock_guard<std::mutex> lock(update_job_mutex_);
                   body["download"] = {
                       {"state", update_job_state_},
                       {"file", update_job_file_.empty() ? nlohmann::json(nullptr)
                                                         : nlohmann::json(update_job_file_)},
                       {"bytes", update_job_bytes_},
                       {"code", update_job_code_.empty() ? nlohmann::json(nullptr)
                                                         : nlohmann::json(update_job_code_)},
                       {"message", update_job_message_.empty()
                                       ? nlohmann::json(nullptr)
                                       : nlohmann::json(update_job_message_)},
                       {"at", update_job_at_.empty() ? nlohmann::json(nullptr)
                                                     : nlohmann::json(update_job_at_)}};
                 }
                 json_ok(res, body);
               }));

  // 下载安装包：后台执行（大包会超过 15 秒写超时），下载完成后校验 sha256 与签名。
  // 校验失败不保留文件，原因写入日志与任务状态，前端轮询 /api/update/status 展示。
  server_->Post("/api/update/download",
                guard("setting.write", [this](const httplib::Request&, httplib::Response& res) {
                  if (current_context().role != "admin") {
                    json_error(res, 403, "FORBIDDEN", "仅管理员可下载更新");
                    return;
                  }
                  const UpdateSettings settings = update_settings();
                  const UpdateCheckResult result = check_for_update(
                      settings, self_update_query(), static_cast<std::int64_t>(std::time(nullptr)));
                  if (!result.configured) {
                    json_error(res, 400, "UPDATE_NOT_CONFIGURED", result.error_message);
                    return;
                  }
                  if (!result.manifest_ok) {
                    json_error(res, 503, result.error_code, result.error_message);
                    return;
                  }
                  if (!result.decision.update_available) {
                    json_error(res, 409, "NO_UPDATE_AVAILABLE", "当前已是最新版本，无需下载");
                    return;
                  }
                  const std::string manifest_file = result.manifest_path;
                  const UpdateManifest manifest = result.manifest;
                  if (!start_update_download(settings, manifest_file, manifest, false)) {
                    json_error(res, 409, "UPDATE_BUSY", "已有更新包正在下载");
                    return;
                  }
                  json_ok(res, nlohmann::json{{"ok", true},
                                              {"state", "running"},
                                              {"version", manifest.version},
                                              {"build", manifest.build}});
                }));

  // 取消后台下载：置位取消标志并唤醒退避等待；正在跑的 curl 会被终止，
  // 任务状态随后由下载线程改为 canceled，前端轮询 /api/update/status 获取结果。
  server_->Post("/api/update/download/cancel",
                guard("setting.write", [this](const httplib::Request&, httplib::Response& res) {
                  if (current_context().role != "admin") {
                    json_error(res, 403, "FORBIDDEN", "仅管理员可取消下载更新");
                    return;
                  }
                  bool running = false;
                  {
                    std::lock_guard<std::mutex> lock(update_job_mutex_);
                    running = update_job_state_ == "running";
                    if (running) {
                      update_job_cancel_ = true;
                    }
                  }
                  if (!running) {
                    json_error(res, 409, "UPDATE_NOT_DOWNLOADING", "当前没有正在进行的下载");
                    return;
                  }
                  update_job_cv_.notify_all();
                  json_ok(res, nlohmann::json{{"ok", true}, {"state", "canceling"}});
                }));

  // 安装更新：必须显式 confirm。服务端复验安装包哈希与签名后，准备替换脚本、
  // 返回响应，然后优雅退出；脚本等进程退出后备份旧程序、覆盖新程序并按原参数重启。
  // 失败时脚本回滚旧程序，不留下无法启动的状态。
  server_->Post("/api/update/install",
                guard("setting.write", [this](const httplib::Request& req,
                                              httplib::Response& res) {
                  if (current_context().role != "admin") {
                    json_error(res, 403, "FORBIDDEN", "仅管理员可安装更新");
                    return;
                  }
                  const nlohmann::json body = json_body(req);
                  if (!body.value("confirm", false)) {
                    json_error(res, 400, "UPDATE_CONFIRM_REQUIRED", "安装更新需要二次确认");
                    return;
                  }
                  const UpdateSettings settings = update_settings();
                  const UpdateCheckResult result = check_for_update(
                      settings, self_update_query(), static_cast<std::int64_t>(std::time(nullptr)));
                  if (!result.manifest_ok) {
                    json_error(res, result.configured ? 503 : 400, result.error_code,
                               result.error_message);
                    return;
                  }
                  if (!result.decision.update_available) {
                    json_error(res, 409, "NO_UPDATE_AVAILABLE", "当前已是最新版本");
                    return;
                  }
                  const std::filesystem::path package =
                      update_package_path(update_download_dir(), result.manifest);
                  // 解包/复验后准备替换：真正的替换由脚本在本进程退出后完成，
                  // 避免覆盖正在运行的二进制；失败时不改动当前程序。
                  const std::filesystem::path staging =
                      update_download_dir() /
                      ("staging-" + result.manifest.version + "-" +
                       std::to_string(result.manifest.build));
                  const std::filesystem::path executable = current_executable_path();
                  const UpdatePackageResult staged = stage_update_package(
                      settings, result.manifest_path, package, staging, executable);
                  if (!staged.ok) {
                    int status = 500;
                    if (staged.error_code == "UPDATE_PACKAGE_MISSING" ||
                        staged.error_code == "UPDATE_PACKAGE_PAYLOAD_MISSING" ||
                        staged.error_code == "UPDATE_PACKAGE_UNSUPPORTED" ||
                        staged.error_code == "UPDATE_EXTRACT_FAILED" ||
                        staged.error_code == "UPDATE_SIGNATURE_INVALID") {
                      status = 409;
                    } else if (staged.error_code == "UPDATE_UNVERIFIED" ||
                               staged.error_code == "UPDATE_VERIFY_UNAVAILABLE") {
                      status = 503;
                    }
                    json_error(res, status, staged.error_code, staged.error_message);
                    return;
                  }
                  UpdateApplyOptions options;
                  options.executable = executable;
                  if (info_.launch_args.size() > 1) {
                    options.args.assign(info_.launch_args.begin() + 1, info_.launch_args.end());
                  }
                  std::error_code cwd_error;
                  options.working_dir = std::filesystem::current_path(cwd_error);
                  if (cwd_error) {
                    options.working_dir = executable.parent_path();
                  }
                  options.log_file = staging / "update-apply.log";
                  const UpdatePackageResult scheduled =
                      schedule_update_replace(staged.path, options, staging);
                  if (!scheduled.ok) {
                    json_error(res, 500, scheduled.error_code, scheduled.error_message);
                    return;
                  }
                  try {
                    services_.db.write([&](sqlite3* conn) {
                      audit_write(conn, "update_install", "system", 0, result.manifest.version,
                                  "自动替换: " + package.filename().string());
                    });
                  } catch (const std::exception& error) {
                    log_warn(std::string("更新安装审计写入失败: ") + error.what());
                  }
                  json_ok(res, nlohmann::json{{"ok", true},
                                              {"mode", scheduled.mode},
                                              {"restarting", true},
                                              {"file", package.filename().string()},
                                              {"message", scheduled.error_message}});
                  // 先让响应写出，再停止监听：主循环退出后替换脚本接管。
                  // 置维护态，避免退出前的几百毫秒里再接受新的写请求。
                  maintenance_ = true;
                  if (!shutdown_started_.exchange(true)) {
                    shutdown_thread_ = std::thread([this] {
                      std::this_thread::sleep_for(std::chrono::milliseconds(800));
                      server_->stop();
                    });
                  }
                }));

  // 恢复向导：列出备份 → 预检查 → 二次确认后落请求并优雅退出。
  // 真正的替换在下次启动、打开数据库之前复用 restore_database 完成，
  // 避免与运行中的连接和 WAL 冲突；失败由 restore_database 回滚。
  server_->Get("/api/restore/backups",
               guard("setting.read", [this](const httplib::Request&, httplib::Response& res) {
                 const auto items = list_backups(services_.effective_backup_dir());
                 nlohmann::json rows = nlohmann::json::array();
                 for (const auto& item : items) {
                   rows.push_back({{"file", item.file},
                                   {"bytes", item.bytes},
                                   {"modified_at", item.modified_at},
                                   {"schema_version", item.schema_version},
                                   {"readable", item.readable},
                                   {"error", item.error.empty() ? nlohmann::json(nullptr)
                                                                : nlohmann::json(item.error)}});
                 }
                 json_ok(res, nlohmann::json{{"ok", true},
                                             {"rows", rows},
                                             {"count", rows.size()},
                                             {"dir_is_custom", info_.backup_dir_custom}});
               }));

  server_->Post("/api/restore/precheck",
                guard("setting.read", [this](const httplib::Request& req,
                                             httplib::Response& res) {
                  const nlohmann::json body = json_body(req);
                  const std::string file = body.value("file", std::string{});
                  if (!safe_backup_name(file)) {
                    json_error(res, 400, "INVALID_FILE_NAME", "备份文件名不合法");
                    return;
                  }
                  const BackupInspection inspection =
                      inspect_backup(services_.effective_backup_dir() / file);
                  json_ok(res, nlohmann::json{
                                   {"ok", inspection.ok},
                                   {"file", file},
                                   {"bytes", inspection.bytes},
                                   {"modified_at", inspection.modified_at},
                                   {"schema_version", inspection.schema_version},
                                   {"current_schema_version", inspection.current_schema_version},
                                   {"compatible", inspection.compatible},
                                   {"will_migrate", inspection.will_migrate},
                                   {"is_app_database", inspection.is_app_database},
                                   {"integrity_ok", inspection.integrity_ok},
                                   {"error", inspection.ok
                                                 ? nlohmann::json(nullptr)
                                                 : nlohmann::json{{"code", inspection.error_code},
                                                                  {"message",
                                                                   inspection.error_message}}}});
                }));

  server_->Post("/api/restore/apply",
                guard("setting.write", [this](const httplib::Request& req,
                                              httplib::Response& res) {
                  if (current_context().role != "admin") {
                    json_error(res, 403, "FORBIDDEN", "仅管理员可执行恢复");
                    return;
                  }
                  const nlohmann::json body = json_body(req);
                  if (!body.value("confirm", false)) {
                    json_error(res, 400, "RESTORE_CONFIRM_REQUIRED", "恢复数据库需要二次确认");
                    return;
                  }
                  const std::string file = body.value("file", std::string{});
                  if (!safe_backup_name(file)) {
                    json_error(res, 400, "INVALID_FILE_NAME", "备份文件名不合法");
                    return;
                  }
                  const std::filesystem::path backup =
                      services_.effective_backup_dir() / file;
                  const BackupInspection inspection = inspect_backup(backup);
                  if (!inspection.ok) {
                    json_error(res, 409, inspection.error_code, inspection.error_message);
                    return;
                  }
                  RestoreRequestInfo request;
                  request.file = file;
                  request.source = std::filesystem::absolute(backup).string();
                  request.actor = current_context().user;
                  request.origin = "网页向导";
                  request.requested_at = now_text();
                  std::string error;
                  if (!write_restore_request(info_.data_dir, request, error)) {
                    json_error(res, 500, "RESTORE_REQUEST_FAILED", error);
                    return;
                  }
                  // 先进入维护态再返回响应：之后的写请求一律 503。
                  maintenance_ = true;
                  if (!shutdown_started_.exchange(true)) {
                    shutdown_thread_ = std::thread([this] {
                      std::this_thread::sleep_for(std::chrono::milliseconds(300));
                      server_->stop();
                    });
                  }
                  json_ok(res, nlohmann::json{
                                   {"ok", true},
                                   {"state", "restarting"},
                                   {"file", file},
                                   {"message",
                                    "已进入维护状态，程序即将退出；重新打开后将完成恢复并显示结果"}});
                }));

  server_->Get("/api/restore/result",
               guard("setting.read", [this](const httplib::Request&, httplib::Response& res) {
                 const auto outcome = read_restore_result(info_.data_dir);
                 if (!outcome) {
                   json_ok(res, nlohmann::json{{"ok", true}, {"result", nullptr}});
                   return;
                 }
                 json_ok(res, nlohmann::json{
                                  {"ok", true},
                                  {"result",
                                   {{"ran", outcome->ran},
                                    {"ok", outcome->ok},
                                    {"file", outcome->file},
                                    {"at", outcome->at},
                                    {"schema_version", outcome->schema_version},
                                    {"safety_backup", outcome->safety_backup},
                                    {"error_code", outcome->error_code.empty()
                                                       ? nlohmann::json(nullptr)
                                                       : nlohmann::json(outcome->error_code)},
                                    {"error_message", outcome->error_message}}}});
               }));

  server_->Post("/api/restore/result/clear",
                guard("setting.write", [this](const httplib::Request&,
                                              httplib::Response& res) {
                  if (current_context().role != "admin") {
                    json_error(res, 403, "FORBIDDEN", "仅管理员可清理恢复结果");
                    return;
                  }
                  clear_restore_result(info_.data_dir);
                  json_ok(res, nlohmann::json{{"ok", true}});
                }));

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

  // 单机版首次设置本机口令：仅在尚未设置口令时可用，写内置 admin 的 password_hash。
  // 不走「首次设置强制改密」：门禁页本身已经要求输入口令，再弹改密页没有意义。
  server_->Post("/api/local/password",
                [this](const httplib::Request& req, httplib::Response& res) {
                  if (info_.mode != "local") {
                    json_error(res, 403, "FORBIDDEN", "仅单机版支持本机口令");
                    return;
                  }
                  const auto body = json_body(req);
                  const std::string password = body.value("password", std::string{});
                  const UserRow admin = services_.users.find_by_username("admin");
                  if (!admin.active) {
                    json_error(res, 403, "USER_DISABLED", "内置管理员已停用");
                    return;
                  }
                  if (services_.users.has_password(admin.id)) {
                    json_error(res, 409, "LOCAL_PASSWORD_ALREADY_SET",
                               "本机口令已设置；如需重置，请在数据目录上执行 --set-password");
                    return;
                  }
                  services_.users.set_password(admin.id, password, /*force_change_on_first_set=*/false);
                  save_local_password_choice(/*enabled=*/true);
                  json_ok(res, nlohmann::json{{"ok", true}});
                });

  // 跳过首次设置提示：记下「已询问」，之后保持一次性 token 自动登录的现状。
  server_->Post("/api/local/password/skip",
                [this](const httplib::Request&, httplib::Response& res) {
                  if (info_.mode != "local") {
                    json_error(res, 403, "FORBIDDEN", "仅单机版支持本机口令");
                    return;
                  }
                  save_local_password_choice(/*enabled=*/false);
                  json_ok(res, nlohmann::json{{"ok", true}});
                });

  // 用 macOS 钥匙串里的长期凭据换正常会话：凭据是服务端签发的随机 token，
  // 原始口令不进入钥匙串。到期、时钟回拨与作废都在服务端判断。
  server_->Post("/api/local/remember", [this](const httplib::Request& req,
                                              httplib::Response& res) {
    if (info_.mode != "local") {
      json_error(res, 403, "FORBIDDEN", "仅单机版支持本机免输");
      return;
    }
    if (!local_password_required()) {
      json_error(res, 401, "REMEMBER_TOKEN_INVALID", "本机口令门禁未启用");
      return;
    }
    const std::string token = json_body(req).value("token", std::string{});
    std::string failure;
    if (!validate_local_remember_token(services_.db, token, &failure)) {
      json_error(res, 401, "REMEMBER_TOKEN_INVALID", failure);
      return;
    }
    const UserRow admin = services_.users.find_by_username("admin");
    if (!admin.active) {
      json_error(res, 403, "USER_DISABLED", "内置管理员已停用");
      return;
    }
    const SessionPolicy policy = services_.settings.session_policy();
    const CreatedSession session = sessions_.create(admin.id, policy.idle_minutes);
    res.set_header("Set-Cookie",
                   "sid=" + session.id + cookie_attributes(req, policy.idle_minutes * 60));
    services_.db.write([&](sqlite3* conn) {
      audit_write_as(conn, admin.username, "login", "auth", admin.id, admin.username,
                     "本机免输");
    });
    nlohmann::json result;
    result["ok"] = true;
    result["user"] = admin.username;
    result["role"] = admin.role;
    result["csrf"] = session.csrf;
    result["must_change_password"] = admin.must_change_password;
    json_ok(res, result);
  });

  // 本机免输设置：默认 30 天免输；切到「每次都要输」时立即作废服务端凭据。
  server_->Get("/api/local/remember/setting",
               guard("setting.write",
                     [this](const httplib::Request&, httplib::Response& res) {
                       if (info_.mode != "local") {
                         json_error(res, 403, "FORBIDDEN", "仅单机版支持本机免输");
                         return;
                       }
                       json_ok(res, nlohmann::json{
                                        {"enabled", local_remember_enabled(services_.db)},
                                        {"has_token", has_local_remember_token(services_.db)}});
                     }));

  server_->Put("/api/local/remember/setting",
               guard("setting.write",
                     [this](const httplib::Request& req, httplib::Response& res) {
                       if (info_.mode != "local") {
                         json_error(res, 403, "FORBIDDEN", "仅单机版支持本机免输");
                         return;
                       }
                       const bool enabled = json_body(req).value("enabled", true);
                       services_.db.write([&](sqlite3* conn) {
                         write_setting_value(conn, kLocalRememberSettingKey,
                                             enabled ? "1" : "0");
                       });
                       const UserRow admin = services_.users.find_by_username("admin");
                       const std::string actor = current_context().user;
                       if (!enabled) {
                         clear_local_remember_tokens(services_.db, admin.id, actor,
                                                     "切换为每次都要输");
                         json_ok(res, nlohmann::json{{"ok", true}, {"enabled", false}});
                         return;
                       }
                       const IssuedRememberToken issued =
                           issue_local_remember_token(services_.db, admin.id, actor);
                       json_ok(res, nlohmann::json{{"ok", true},
                                                   {"enabled", true},
                                                   {"remember_token", issued.token},
                                                   {"expires_at", issued.expires_at}});
                     }));

  server_->Post("/api/local/remember/clear",
                guard("setting.write",
                      [this](const httplib::Request&, httplib::Response& res) {
                        if (info_.mode != "local") {
                          json_error(res, 403, "FORBIDDEN", "仅单机版支持本机免输");
                          return;
                        }
                        const UserRow admin = services_.users.find_by_username("admin");
                        clear_local_remember_tokens(services_.db, admin.id,
                                                    current_context().user, "用户清除本机记忆");
                        json_ok(res, nlohmann::json{{"ok", true}});
                      }));

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

  // 浏览器会无条件请求 /favicon.ico：返回内嵌图标，未内嵌（开发态）时保持 204。
  server_->Get("/favicon.ico", [](const httplib::Request&, httplib::Response& res) {
    resources::Resource resource;
    if (!resources::find("/favicon.ico", resource)) {
      res.status = 204;
      return;
    }
    res.set_content(reinterpret_cast<const char*>(resource.data), resource.size,
                    std::string(resource.content_type));
    res.set_header("Cache-Control", "no-cache");
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
         "latest.json")
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
    services_.db.read([&](sqlite3* conn) {
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
  services_.db.read([&](sqlite3* conn) {
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
  services_.db.write([&](sqlite3* conn) {
    Stmt stmt(conn,
              "INSERT INTO app_setting (key, value, updated_at, updated_by)"
              " VALUES (?, ?, ?, 'system')"
              " ON CONFLICT(key) DO UPDATE SET value = excluded.value,"
              " updated_at = excluded.updated_at;");
    stmt.bind(1, kLocalPasswordSettingKey)
        .bind(2, enabled ? std::string("1") : std::string("0"))
        .bind(3, now_text())
        .run();
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
