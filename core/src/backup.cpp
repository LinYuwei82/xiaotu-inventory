#include "xiaotu/backup.h"

#include <sqlite3.h>

#include <chrono>
#include <ctime>
#include <system_error>

#include "xiaotu/error.h"
#include "xiaotu/platform.h"

namespace xiaotu {
namespace {

std::string timestamp_tag() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
  const std::tm local = local_time(seconds);
  char buffer[32] = {};
  std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", &local);
  return buffer;
}

std::filesystem::path unique_path(const std::filesystem::path& directory,
                                  std::string_view name) {
  std::filesystem::path candidate = directory / (std::string(name) + ".db");
  int suffix = 1;
  while (std::filesystem::exists(candidate)) {
    candidate = directory / (std::string(name) + "-" + std::to_string(suffix++) + ".db");
  }
  return candidate;
}

}  // namespace

void verify_database(const std::filesystem::path& path) {
  if (!std::filesystem::exists(path)) {
    throw AppError("BACKUP_NOT_FOUND", "备份文件不存在：" + path.string());
  }
  sqlite3* connection = nullptr;
  if (sqlite3_open_v2(path_to_utf8(path).c_str(), &connection, SQLITE_OPEN_READONLY, nullptr) !=
      SQLITE_OK) {
    const std::string detail = connection == nullptr ? "无法打开" : sqlite3_errmsg(connection);
    if (connection != nullptr) sqlite3_close(connection);
    throw AppError("BACKUP_INVALID", "无法打开备份：" + detail);
  }
  std::string result;
  try {
    Stmt stmt(connection, "PRAGMA integrity_check;");
    result = stmt.step() ? stmt.text(0) : std::string{};
  } catch (const AppError& error) {
    sqlite3_close(connection);
    throw AppError("BACKUP_INVALID", "备份文件无法读取：" + std::string(error.what()));
  } catch (...) {
    sqlite3_close(connection);
    throw AppError("BACKUP_INVALID", "备份文件无法读取");
  }
  sqlite3_close(connection);
  if (result != "ok") {
    throw AppError("BACKUP_INVALID", "备份完整性检查失败：" + result);
  }
}

BackupResult backup_database(Db& db, const std::filesystem::path& directory,
                             std::string_view tag) {
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    throw AppError("BACKUP_FAILED", "无法创建备份目录：" + error.message());
  }
  const std::string name = "app-" + timestamp_tag() + "-" + std::string(tag);
  const std::filesystem::path target = unique_path(directory, name);
  const std::string target_text = target.string();
  db.write_raw([&](sqlite3* connection) {
    Stmt stmt(connection, "VACUUM INTO ?;");
    stmt.bind(1, target_text);
    stmt.run();
  });
  verify_database(target);
  return BackupResult{.path = target, .bytes = std::filesystem::file_size(target)};
}

BackupResult restore_database(const std::filesystem::path& source,
                              const std::filesystem::path& database_path) {
  verify_database(source);
  std::error_code error;
  const std::filesystem::path absolute_source = std::filesystem::absolute(source, error);
  const std::filesystem::path absolute_target = std::filesystem::absolute(database_path, error);
  if (absolute_source == absolute_target) {
    throw AppError("INVALID_INPUT", "恢复源与目标数据库相同");
  }

  const std::filesystem::path backup_dir = database_path.parent_path() / "backups";
  std::filesystem::create_directories(backup_dir, error);
  if (error) {
    throw AppError("RESTORE_FAILED", "无法创建备份目录：" + error.message());
  }
  if (std::filesystem::exists(database_path)) {
    // 先 checkpoint，让单文件安全备份包含 WAL 中已提交的内容。
    sqlite3* connection = nullptr;
    if (sqlite3_open(path_to_utf8(database_path).c_str(), &connection) == SQLITE_OK) {
      sqlite3_exec(connection, "PRAGMA wal_checkpoint(TRUNCATE);", nullptr, nullptr, nullptr);
      sqlite3_close(connection);
    }
    const std::filesystem::path safety =
        unique_path(backup_dir, "pre-restore-" + timestamp_tag());
    std::filesystem::copy_file(database_path, safety,
                               std::filesystem::copy_options::overwrite_existing, error);
    if (error) {
      throw AppError("RESTORE_FAILED", "无法创建恢复前备份：" + error.message());
    }
  }

  for (const char* suffix : {"", "-wal", "-shm"}) {
    const std::filesystem::path sidecar = database_path.string() + suffix;
    if (std::filesystem::exists(sidecar)) {
      std::filesystem::remove(sidecar, error);
      if (error) {
        throw AppError("RESTORE_FAILED", "无法清理旧数据库文件：" + error.message());
      }
    }
  }
  std::filesystem::copy_file(source, database_path,
                             std::filesystem::copy_options::overwrite_existing, error);
  if (error) {
    throw AppError("RESTORE_FAILED", "复制备份失败：" + error.message());
  }
  verify_database(database_path);
  return BackupResult{.path = database_path,
                      .bytes = std::filesystem::file_size(database_path)};
}

}  // namespace xiaotu
