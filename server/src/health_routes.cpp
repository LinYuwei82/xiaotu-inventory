#include <httplib.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <system_error>

#include "local_auth_common.h"
#include "xiaotu/backup.h"
#include "xiaotu/context.h"
#include "xiaotu/edition.h"
#include "xiaotu/gen/edition_caps.h"
#include "xiaotu/http_json.h"
#include "xiaotu/http_server.h"
#include "xiaotu/migrations.h"
#include "xiaotu/resources.h"
#include "xiaotu/services.h"

namespace xiaotu {

void HttpServer::register_health_routes() {
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
}

}  // namespace xiaotu
