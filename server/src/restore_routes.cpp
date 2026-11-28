#include <httplib.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

#include "xiaotu/backup.h"
#include "xiaotu/context.h"
#include "xiaotu/http_guard.h"
#include "xiaotu/http_json.h"
#include "xiaotu/http_server.h"
#include "xiaotu/restore_wizard.h"
#include "xiaotu/services.h"

namespace xiaotu {
namespace {

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

}  // namespace

void HttpServer::register_restore_routes() {
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
}

}  // namespace xiaotu
