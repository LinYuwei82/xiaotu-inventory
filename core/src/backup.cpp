#include "xiaotu/backup.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <system_error>
#include <vector>

#include "nlohmann/json.hpp"
#include "xiaotu/audit.h"
#include "xiaotu/error.h"
#include "xiaotu/log.h"
#include "xiaotu/migrations.h"
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

// 解析每日备份文件名 app-YYYYMMDD-HHMMSS-daily[-N].db。
// 成功时 stamp=YYYYMMDD-HHMMSS，ym=YYYYMM。
bool parse_daily_name(const std::filesystem::path& path, std::string& stamp, std::string& ym) {
  const std::string name = path.filename().string();
  if (name.rfind("app-", 0) != 0 || path.extension() != ".db") {
    return false;
  }
  const std::string core = name.substr(4, name.size() - 4 - 3);
  if (core.size() < 16 || core[8] != '-' || core[15] != '-') {
    return false;
  }
  for (std::size_t i : {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 9U, 10U, 11U, 12U, 13U, 14U}) {
    if (core[i] < '0' || core[i] > '9') {
      return false;
    }
  }
  if (core.compare(16, 5, "daily") != 0) {
    return false;
  }
  // tag 后允许唯一后缀（-1、-2…），不允许其他形态。
  if (core.size() > 21 && core[21] != '-') {
    return false;
  }
  stamp = core.substr(0, 15);
  ym = stamp.substr(0, 6);
  return true;
}

std::filesystem::path status_path(const std::filesystem::path& directory) {
  return directory / "backup-status.json";
}

void write_status(const std::filesystem::path& directory, const BackupStatus& status) {
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    return;
  }
  nlohmann::json body;
  if (!status.last_success_at.empty()) {
    body["last_success"] = {{"at", status.last_success_at},
                            {"file", std::filesystem::path(status.last_success_file).filename().string()},
                            {"bytes", status.last_success_bytes}};
  } else {
    body["last_success"] = nullptr;
  }
  if (!status.last_failure_at.empty()) {
    body["last_failure"] = {{"at", status.last_failure_at}, {"reason", status.last_failure_reason}};
  } else {
    body["last_failure"] = nullptr;
  }
  body["schema_version"] = status.schema_version;
  body["updated_at"] = now_text();

  const std::filesystem::path target = status_path(directory);
  const std::filesystem::path temp = target.string() + ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) {
      return;
    }
    out << body.dump(2) << "\n";
  }
  std::filesystem::rename(temp, target, error);
  if (error) {
    // Windows 不允许 rename 覆盖已存在的文件，退化为先删后改名。
    std::filesystem::remove(target, error);
    error.clear();
    std::filesystem::rename(temp, target, error);
    if (error) {
      std::filesystem::remove(temp, error);
    }
  }
}

BackupStatus read_status(const std::filesystem::path& directory) {
  BackupStatus status;
  std::ifstream in(status_path(directory), std::ios::binary);
  if (!in) {
    return status;
  }
  try {
    nlohmann::json body;
    in >> body;
    if (body.contains("last_success") && body["last_success"].is_object()) {
      const auto& success = body["last_success"];
      status.last_success_at = success.value("at", "");
      status.last_success_file = success.value("file", "");
      status.last_success_bytes = success.value("bytes", std::uintmax_t{0});
    } else if (body.value("last_result", "") == "ok") {
      // 兼容旧格式：last_result/last_at 单字段。
      status.last_success_at = body.value("last_at", "");
      status.last_success_file = std::filesystem::path(body.value("last_path", "")).filename().string();
      status.last_success_bytes = body.value("last_bytes", std::uintmax_t{0});
    }
    if (body.contains("last_failure") && body["last_failure"].is_object()) {
      const auto& failure = body["last_failure"];
      status.last_failure_at = failure.value("at", "");
      status.last_failure_reason = failure.value("reason", "");
    } else if (body.value("last_result", "") == "failed") {
      status.last_failure_at = body.value("last_at", "");
      status.last_failure_reason = body.value("last_message", "");
    }
    status.schema_version = body.value("schema_version", 0);
  } catch (...) {
    // 状态文件损坏时按「从未备份」处理，由每日任务重新生成。
  }
  return status;
}

// 备份目录级互斥：防止定时任务与进程内调度同时执行。
// 上一次异常退出留下的锁超过 2 小时视为失效。
class BackupDirLock {
 public:
  explicit BackupDirLock(const std::filesystem::path& directory)
      : path_(directory / ".backup.lock") {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    held_ = std::filesystem::create_directory(path_, error);
    if (!held_ && !error && stale()) {
      std::filesystem::remove_all(path_, error);
      error.clear();
      held_ = std::filesystem::create_directory(path_, error);
    }
  }

  ~BackupDirLock() {
    if (held_) {
      std::error_code ignored;
      std::filesystem::remove_all(path_, ignored);
    }
  }

  BackupDirLock(const BackupDirLock&) = delete;
  BackupDirLock& operator=(const BackupDirLock&) = delete;

  bool held() const { return held_; }

 private:
  bool stale() const {
    std::error_code error;
    const auto modified = std::filesystem::last_write_time(path_, error);
    if (error) {
      return false;
    }
    return std::filesystem::file_time_type::clock::now() - modified >
           std::chrono::hours(2);
  }

  std::filesystem::path path_;
  bool held_ = false;
};

std::string today_stamp() {
  std::string text = today_text();  // YYYY-MM-DD
  text.erase(std::remove(text.begin(), text.end(), '-'), text.end());
  return text;
}

void update_success_status(const std::filesystem::path& directory,
                           const BackupResult& result, int schema_version) {
  BackupStatus status = read_status(directory);
  status.last_success_at = now_text();
  status.last_success_file = result.path.filename().string();
  status.last_success_bytes = result.bytes;
  status.schema_version = schema_version;
  write_status(directory, status);
}

void update_failure_status(const std::filesystem::path& directory, std::string_view reason) {
  BackupStatus status = read_status(directory);
  status.last_failure_at = now_text();
  status.last_failure_reason = std::string(reason);
  status.schema_version = target_schema_version();
  write_status(directory, status);
}

// 备份成功/失败都写 audit_log；数据库不可写时退化为日志，不影响主流程。
void audit_backup(Db& db, bool success, std::string_view tag, std::string_view detail) {
  try {
    db.write([&](sqlite3* connection) {
      audit_write(connection, "backup", "database", 0, tag,
                  success ? detail : ("失败：" + std::string(detail)));
    });
  } catch (const std::exception& error) {
    log_warn(std::string("备份审计写入失败：") + error.what());
  }
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
  verify_backup_schema(target, db.schema_version());
  return BackupResult{.path = target, .bytes = std::filesystem::file_size(target)};
}

namespace {

// 恢复审计：成功与失败都留痕。恢复出来的库可能没有 audit_log（v5 之前）或已损坏，
// 这时退化为写日志，不影响恢复本身的成败。
void audit_restore(const std::filesystem::path& database_path, std::string_view actor,
                   std::string_view origin, std::string_view source_name, int schema_version,
                   const std::filesystem::path& safety_backup, bool target_existed,
                   std::string_view failure) {
  if (actor.empty() || !std::filesystem::exists(database_path)) return;
  std::string detail = "来源: " + std::string(origin.empty() ? "未知" : origin) +
                       "；备份: " + std::string(source_name);
  if (!failure.empty()) {
    detail += "；失败原因: " + std::string(failure);
  } else {
    detail += "；schema: v" + std::to_string(schema_version);
  }
  detail += "；恢复前安全备份: ";
  if (!safety_backup.empty()) {
    detail += safety_backup.filename().string();
  } else {
    detail += target_existed ? "无（在创建安全备份前失败）" : "无（目标库原本不存在）";
  }
  try {
    Db db(database_path, 1);
    if (db.schema_version() < 5) {
      log_warn("恢复审计跳过：目标库 schema 早于 v5，没有 audit_log");
      return;
    }
    db.write([&](sqlite3* conn) {
      audit_write_as(conn, actor, "restore", "database", 0, source_name, detail);
    });
  } catch (const std::exception& error) {
    log_warn("恢复审计写入失败: " + std::string(error.what()));
  }
}

}  // namespace

RestoreResult restore_database(const std::filesystem::path& source,
                               const std::filesystem::path& database_path,
                               std::string_view actor, std::string_view origin) {
  RestoreResult result;
  result.path = database_path;
  const std::string source_name = source.filename().string();
  const bool target_existed = std::filesystem::exists(database_path);
  try {
    verify_database(source);
    ensure_restore_compatible(source);
    result.schema_version = backup_schema_version(source);
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
      result.safety_backup = safety;
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
    result.bytes = std::filesystem::file_size(database_path);
  } catch (const std::exception& error) {
    audit_restore(database_path, actor, origin, source_name, 0, result.safety_backup,
                  target_existed, error.what());
    throw;
  }
  audit_restore(database_path, actor, origin, source_name, result.schema_version,
                result.safety_backup, target_existed, {});
  return result;
}

int backup_schema_version(const std::filesystem::path& backup) {
  sqlite3* connection = nullptr;
  const std::string path = path_to_utf8(backup);
  if (sqlite3_open_v2(path.c_str(), &connection, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    if (connection != nullptr) {
      sqlite3_close(connection);
    }
    throw AppError("BACKUP_INVALID", "无法打开备份文件：" + path);
  }

  int version = 0;
  try {
    Stmt migrations(connection, "SELECT COALESCE(MAX(version), 0) FROM schema_migration;");
    if (migrations.step()) {
      version = static_cast<int>(migrations.i64(0));
    }
  } catch (...) {
    // 旧库可能还没有 schema_migration 表，退回 user_version。
  }
  try {
    Stmt pragma(connection, "PRAGMA user_version;");
    if (pragma.step()) {
      version = std::max(version, static_cast<int>(pragma.i64(0)));
    }
  } catch (...) {
    // 读不到 user_version 时保留上面的结果。
  }
  sqlite3_close(connection);
  return version;
}

void ensure_restore_compatible(const std::filesystem::path& backup) {
  const int source = backup_schema_version(backup);
  const int target = target_schema_version();
  if (source <= 0) {
    throw AppError("SCHEMA_UNKNOWN",
                   "无法识别备份的数据库结构版本（缺少 schema_migration / user_version），"
                   "为避免覆盖现有数据，已拒绝恢复：" + path_to_utf8(backup));
  }
  if (source > target) {
    throw AppError("SCHEMA_TOO_NEW",
                   "备份的数据库结构版本为 v" + std::to_string(source) + "，高于当前程序支持的 v" +
                       std::to_string(target) + "，请先把程序升级到对应版本再恢复。");
  }
  if (source < target) {
    log_warn("恢复的备份结构版本 v" + std::to_string(source) + " 低于当前 v" +
             std::to_string(target) + "，恢复后将在启动时自动迁移");
  }
}

void verify_backup_schema(const std::filesystem::path& backup, int expected_version) {
  const int actual = backup_schema_version(backup);
  const int supported = target_schema_version();
  if (actual <= 0) {
    throw AppError("BACKUP_SCHEMA_UNKNOWN",
                   "备份未记录有效的数据库结构版本，不可用于恢复：" +
                       backup.filename().string());
  }
  if (actual != expected_version) {
    throw AppError("BACKUP_SCHEMA_MISMATCH",
                   "备份记录的数据库结构版本为 v" + std::to_string(actual) + "，与源库 v" +
                       std::to_string(expected_version) + " 不一致，已判定该备份不可用。");
  }
  if (actual > supported) {
    throw AppError("BACKUP_SCHEMA_TOO_NEW",
                   "备份的数据库结构版本为 v" + std::to_string(actual) + "，高于当前程序支持的 v" +
                       std::to_string(supported) + "，已判定该备份不可用。");
  }
}

RetentionResult apply_daily_retention(const std::filesystem::path& directory, int daily_keep) {
  RetentionResult result;
  std::error_code error;
  if (!std::filesystem::is_directory(directory, error)) {
    return result;
  }

  struct Entry {
    std::filesystem::path path;
    std::string stamp;
    std::string month;
  };
  std::vector<Entry> entries;
  for (const auto& item : std::filesystem::directory_iterator(directory, error)) {
    if (error) {
      break;
    }
    if (!item.is_regular_file(error)) {
      continue;
    }
    std::string stamp;
    std::string month;
    if (parse_daily_name(item.path(), stamp, month)) {
      entries.push_back(Entry{.path = item.path(), .stamp = stamp, .month = month});
    }
  }

  std::sort(entries.begin(), entries.end(),
            [](const Entry& left, const Entry& right) { return left.stamp > right.stamp; });

  std::set<std::filesystem::path> keep;
  const std::size_t daily = std::min<std::size_t>(
      entries.size(), static_cast<std::size_t>(std::max(daily_keep, 0)));
  for (std::size_t i = 0; i < daily; ++i) {
    keep.insert(entries[i].path);
  }
  std::map<std::string, int> monthly_seen;
  for (const auto& entry : entries) {
    if (monthly_seen[entry.month] >= kMonthlyBackupKeep) {
      continue;
    }
    ++monthly_seen[entry.month];
    keep.insert(entry.path);
  }

  for (const auto& entry : entries) {
    if (keep.count(entry.path) != 0) {
      ++result.kept;
      continue;
    }
    std::filesystem::remove(entry.path, error);
    if (error) {
      log_warn("清理旧备份失败：" + path_to_utf8(entry.path) + "：" + error.message());
      error.clear();
      continue;
    }
    ++result.removed;
  }
  return result;
}

BackupStatus backup_status(const std::filesystem::path& directory) {
  BackupStatus status = read_status(directory);
  std::error_code error;
  if (std::filesystem::is_directory(directory, error)) {
    for (const auto& item : std::filesystem::directory_iterator(directory, error)) {
      if (error) {
        break;
      }
      if (item.is_regular_file(error) && item.path().extension() == ".db") {
        ++status.backup_count;
      }
    }
  }
  return status;
}

bool daily_backup_due(const std::filesystem::path& directory) {
  const std::string today = today_stamp();
  const BackupStatus status = read_status(directory);
  if (status.last_success_at.size() >= 10 && status.last_success_at.substr(0, 10) == today_text()) {
    return false;
  }

  std::error_code error;
  if (std::filesystem::is_directory(directory, error)) {
    for (const auto& item : std::filesystem::directory_iterator(directory, error)) {
      if (error) {
        break;
      }
      if (!item.is_regular_file(error)) {
        continue;
      }
      std::string stamp;
      std::string month;
      if (parse_daily_name(item.path(), stamp, month) && stamp.rfind(today, 0) == 0) {
        return false;
      }
    }
  }
  return true;
}

DailyBackupOutcome run_daily_backup(Db& db, const std::filesystem::path& data_dir) {
  DailyBackupOutcome outcome;
  const std::filesystem::path directory = data_dir / "backups";
  BackupDirLock lock(directory);
  if (!lock.held()) {
    outcome.skipped = "另一进程正在备份";
    return outcome;
  }
  if (!daily_backup_due(directory)) {
    outcome.skipped = "今天已备份";
    return outcome;
  }

  try {
    outcome.backup = backup_database(db, directory, "daily");
    outcome.ran = true;
    try {
      outcome.retention = apply_daily_retention(directory);
    } catch (const std::exception& error) {
      log_warn(std::string("每日备份保留策略执行失败：") + error.what());
    }

    update_success_status(directory, outcome.backup, db.schema_version());
    audit_backup(db, true, "daily", std::to_string(outcome.backup.bytes) + " 字节");
    log_info("每日备份完成：" + path_to_utf8(outcome.backup.path));
  } catch (const std::exception& error) {
    record_backup_failure(db, data_dir, "daily", error.what());
    throw;
  }
  return outcome;
}

void record_backup_failure(Db& db, const std::filesystem::path& data_dir, std::string_view tag,
                           std::string_view reason) {
  update_failure_status(data_dir / "backups", reason);
  audit_backup(db, false, tag, reason);
  log_error("备份失败（" + std::string(tag) + "）：" + std::string(reason));
}

BackupResult run_manual_backup(Db& db, const std::filesystem::path& data_dir) {
  const std::filesystem::path directory = data_dir / "backups";
  BackupDirLock lock(directory);
  if (!lock.held()) {
    throw AppError("BACKUP_BUSY", "另一个备份任务正在进行，请稍后重试。");
  }
  try {
    const BackupResult result = backup_database(db, directory, "manual");
    update_success_status(directory, result, db.schema_version());
    audit_backup(db, true, "manual", std::to_string(result.bytes) + " 字节");
    log_info("手动备份完成：" + path_to_utf8(result.path));
    return result;
  } catch (const std::exception& error) {
    record_backup_failure(db, data_dir, "manual", error.what());
    throw;
  }
}

bool database_writable(Db& db) {
  bool writable = false;
  try {
    db.write_raw([&](sqlite3* connection) {
      if (sqlite3_exec(connection, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return;
      }
      writable = sqlite3_exec(connection, "ROLLBACK;", nullptr, nullptr, nullptr) == SQLITE_OK;
    });
  } catch (...) {
    writable = false;
  }
  return writable;
}

}  // namespace xiaotu
