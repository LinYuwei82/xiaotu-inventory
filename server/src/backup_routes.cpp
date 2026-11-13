#include <httplib.h>

#include <filesystem>
#include <nlohmann/json.hpp>

#include "xiaotu/backup.h"
#include "xiaotu/context.h"
#include "xiaotu/db.h"
#include "xiaotu/error.h"
#include "xiaotu/http_guard.h"
#include "xiaotu/http_json.h"
#include "xiaotu/services.h"

namespace xiaotu {
namespace {

// POST /api/backup：管理员且具备 setting.write 才可执行；只做手动备份，不提供在线恢复。
// 返回文件名、大小与时间，不返回任何绝对路径（与 /api/health 同口径）。
void register_manual_backup(httplib::Server& server, Services& services) {
  server.Post("/api/backup",
              guard("setting.write",
                    [&services](const httplib::Request&, httplib::Response& res) {
                      if (current_context().role != "admin") {
                        json_error(res, 403, "FORBIDDEN", "仅管理员可执行备份");
                        return;
                      }
                      try {
                        const std::filesystem::path data_dir =
                            services.db.file().parent_path();
                        const BackupResult result = run_manual_backup(services.db, data_dir);
                        nlohmann::json body;
                        body["ok"] = true;
                        body["file"] = result.path.filename().string();
                        body["bytes"] = result.bytes;
                        body["at"] = now_text();
                        json_ok(res, body);
                      } catch (const AppError& error) {
                        const int status = error.code() == "BACKUP_BUSY" ? 409 : 400;
                        json_error(res, status, error.code(), error.what());
                      } catch (const std::exception& error) {
                        json_error(res, 500, "BACKUP_FAILED", error.what());
                      }
                    }));
}

}  // namespace

void register_backup_routes(httplib::Server& server, Services& services) {
  register_manual_backup(server, services);
}

}  // namespace xiaotu
