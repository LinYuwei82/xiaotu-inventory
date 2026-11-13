#include "xiaotu/restore_wizard.h"

#include <fstream>
#include <system_error>

#include <nlohmann/json.hpp>

#include "xiaotu/backup.h"
#include "xiaotu/db.h"
#include "xiaotu/error.h"
#include "xiaotu/log.h"

namespace xiaotu {
namespace {

std::filesystem::path request_path(const std::filesystem::path& data_dir) {
  return data_dir / ".restore-request.json";
}

std::filesystem::path result_path(const std::filesystem::path& data_dir) {
  return data_dir / ".restore-result.json";
}

// 先写临时文件再改名，避免进程中断留下半个 JSON。
bool write_json_atomic(const std::filesystem::path& target, const nlohmann::json& body,
                       std::string& error) {
  std::error_code fs_error;
  const std::filesystem::path temp = target.string() + ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) {
      error = "无法写入 " + temp.string();
      return false;
    }
    out << body.dump(2) << '\n';
  }
  std::filesystem::rename(temp, target, fs_error);
  if (fs_error) {
    std::filesystem::remove(target, fs_error);
    fs_error.clear();
    std::filesystem::rename(temp, target, fs_error);
  }
  if (fs_error) {
    error = "无法替换 " + target.string() + "：" + fs_error.message();
    return false;
  }
  return true;
}

std::optional<nlohmann::json> read_json(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    return std::nullopt;
  }
  nlohmann::json body = nlohmann::json::parse(input, nullptr, false);
  if (body.is_discarded() || !body.is_object()) {
    return std::nullopt;
  }
  return body;
}

}  // namespace

bool write_restore_request(const std::filesystem::path& data_dir,
                           const RestoreRequestInfo& request, std::string& error) {
  nlohmann::json body{{"file", request.file},
                      {"source", request.source},
                      {"actor", request.actor},
                      {"origin", request.origin},
                      {"requested_at", request.requested_at}};
  return write_json_atomic(request_path(data_dir), body, error);
}

std::optional<RestoreRequestInfo> read_restore_request(const std::filesystem::path& data_dir) {
  const auto body = read_json(request_path(data_dir));
  if (!body) {
    return std::nullopt;
  }
  RestoreRequestInfo request;
  request.file = body->value("file", std::string{});
  request.source = body->value("source", std::string{});
  request.actor = body->value("actor", std::string{});
  request.origin = body->value("origin", std::string{"网页向导"});
  request.requested_at = body->value("requested_at", std::string{});
  if (request.file.empty() || request.source.empty()) {
    return std::nullopt;
  }
  return request;
}

void clear_restore_request(const std::filesystem::path& data_dir) {
  std::error_code error;
  std::filesystem::remove(request_path(data_dir), error);
}

void write_restore_result(const std::filesystem::path& data_dir, const RestoreOutcome& outcome) {
  nlohmann::json body{{"ran", outcome.ran},
                      {"ok", outcome.ok},
                      {"file", outcome.file},
                      {"at", outcome.at},
                      {"schema_version", outcome.schema_version},
                      {"safety_backup", outcome.safety_backup},
                      {"error_code", outcome.error_code},
                      {"error_message", outcome.error_message}};
  std::string error;
  if (!write_json_atomic(result_path(data_dir), body, error)) {
    log_warn("恢复结果写入失败：" + error);
  }
}

std::optional<RestoreOutcome> read_restore_result(const std::filesystem::path& data_dir) {
  const auto body = read_json(result_path(data_dir));
  if (!body) {
    return std::nullopt;
  }
  RestoreOutcome outcome;
  outcome.ran = body->value("ran", false);
  outcome.ok = body->value("ok", false);
  outcome.file = body->value("file", std::string{});
  outcome.at = body->value("at", std::string{});
  outcome.schema_version = body->value("schema_version", 0);
  outcome.safety_backup = body->value("safety_backup", std::string{});
  outcome.error_code = body->value("error_code", std::string{});
  outcome.error_message = body->value("error_message", std::string{});
  return outcome;
}

void clear_restore_result(const std::filesystem::path& data_dir) {
  std::error_code error;
  std::filesystem::remove(result_path(data_dir), error);
}

std::optional<RestoreOutcome> apply_pending_restore(const std::filesystem::path& data_dir,
                                                    const std::filesystem::path& database_path,
                                                    const std::filesystem::path& backup_dir) {
  const auto request = read_restore_request(data_dir);
  if (!request) {
    return std::nullopt;
  }

  RestoreOutcome outcome;
  outcome.ran = true;
  outcome.file = request->file;
  outcome.at = now_text();
  const std::filesystem::path source(request->source);
  try {
    if (!std::filesystem::exists(source)) {
      throw AppError("RESTORE_SOURCE_MISSING",
                     "待恢复的备份文件已不存在：" + request->file + "，恢复已取消");
    }
    const RestoreResult restored =
        restore_database(source, database_path, request->actor, request->origin, backup_dir);
    outcome.ok = true;
    outcome.schema_version = restored.schema_version;
    outcome.safety_backup = restored.safety_backup.filename().string();
    log_info("网页向导恢复完成：" + request->file + " schema=v" +
             std::to_string(restored.schema_version));
  } catch (const AppError& error) {
    outcome.error_code = error.code();
    outcome.error_message = error.what();
    log_error("网页向导恢复失败：" + outcome.error_message);
  } catch (const std::exception& error) {
    outcome.error_code = "RESTORE_FAILED";
    outcome.error_message = error.what();
    log_error("网页向导恢复失败：" + outcome.error_message);
  }

  clear_restore_request(data_dir);
  write_restore_result(data_dir, outcome);
  return outcome;
}

}  // namespace xiaotu
