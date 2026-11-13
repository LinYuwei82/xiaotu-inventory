// 备份与恢复补强项的冒烟测试：保留策略、schema 兼容性、每日备份与状态。
#include <sqlite3.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>

#include "xiaotu/audit.h"
#include "xiaotu/backup.h"
#include "xiaotu/db.h"
#include "xiaotu/error.h"
#include "xiaotu/migrations.h"

namespace {

int g_failures = 0;

void check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << std::endl;
    ++g_failures;
  }
}

std::string error_code_of(const std::function<void()>& action) {
  try {
    action();
  } catch (const xiaotu::AppError& error) {
    return error.code();
  }
  return "";
}

void touch(const std::filesystem::path& directory, const std::string& name) {
  std::ofstream out(directory / name, std::ios::binary);
  out << "";
}

std::string daily_name(int year, int month, int day) {
  char buffer[64] = {};
  std::snprintf(buffer, sizeof(buffer), "app-%04d%02d%02d-120000-daily.db", year, month, day);
  return buffer;
}

std::string manual_name(int year, int month, int day) {
  char buffer[64] = {};
  std::snprintf(buffer, sizeof(buffer), "app-%04d%02d%02d-120000-manual.db", year, month, day);
  return buffer;
}

int days_in_month(int month) {
  switch (month) {
    case 2:
      return 28;
    case 4:
    case 6:
    case 9:
    case 11:
      return 30;
    default:
      return 31;
  }
}

void test_retention(const std::filesystem::path& root) {
  const std::filesystem::path directory = root / "retention";
  std::filesystem::create_directories(directory);

  // 2026-01-01 ~ 2026-04-30，共 120 份日备份。
  for (int month = 1; month <= 4; ++month) {
    for (int day = 1; day <= days_in_month(month); ++day) {
      touch(directory, daily_name(2026, month, day));
    }
  }
  touch(directory, manual_name(2025, 12, 31));
  touch(directory, "pre-restore-20251231-120000.db");

  const xiaotu::RetentionResult result = xiaotu::apply_daily_retention(directory, 30);
  check(result.kept == 33, "保留最近 30 份 + 3 个月份代表，共 33 份");
  check(result.removed == 87, "其余 87 份日备份被清理");
  check(std::filesystem::exists(directory / daily_name(2026, 4, 30)), "最新日备份保留");
  check(std::filesystem::exists(directory / daily_name(2026, 4, 1)), "最近 30 天窗口内保留");
  check(std::filesystem::exists(directory / daily_name(2026, 3, 31)), "3 月代表保留");
  check(std::filesystem::exists(directory / daily_name(2026, 2, 28)), "2 月代表保留");
  check(std::filesystem::exists(directory / daily_name(2026, 1, 31)), "1 月代表保留");
  check(!std::filesystem::exists(directory / daily_name(2026, 1, 30)), "超出窗口且非月份代表被删除");
  check(std::filesystem::exists(directory / manual_name(2025, 12, 31)), "手动备份不参与清理");
  check(std::filesystem::exists(directory / "pre-restore-20251231-120000.db"),
        "恢复前安全备份不参与清理");

  const xiaotu::RetentionResult again = xiaotu::apply_daily_retention(directory, 30);
  check(again.kept == 33 && again.removed == 0, "重复清理幂等");
}

void test_schema_compatibility(const std::filesystem::path& root) {
  const int target = xiaotu::target_schema_version();

  const std::filesystem::path future = root / "future.db";
  sqlite3* connection = nullptr;
  check(sqlite3_open(future.string().c_str(), &connection) == SQLITE_OK, "创建未来版本库");
  if (connection != nullptr) {
    const std::string sql = "PRAGMA user_version = " + std::to_string(target + 1) + ";";
    sqlite3_exec(connection, sql.c_str(), nullptr, nullptr, nullptr);
    sqlite3_close(connection);
  }
  check(xiaotu::backup_schema_version(future) == target + 1, "读取备份 schema 版本");

  const std::string too_new = error_code_of([&] {
    xiaotu::ensure_restore_compatible(future);
  });
  check(too_new == "SCHEMA_TOO_NEW", "高于当前版本应拒绝恢复");

  const std::filesystem::path unknown = root / "unknown.db";
  connection = nullptr;
  check(sqlite3_open(unknown.string().c_str(), &connection) == SQLITE_OK, "创建无版本库");
  if (connection != nullptr) {
    sqlite3_close(connection);
  }
  check(error_code_of([&] { xiaotu::ensure_restore_compatible(unknown); }) == "SCHEMA_UNKNOWN",
        "无法识别版本应拒绝恢复");

  // restore_database 也必须在覆盖目标库之前拒绝。
  const std::filesystem::path data = root / "schema-data";
  std::filesystem::create_directories(data);
  xiaotu::Db db(data / "app.db", 2);
  xiaotu::migrate(db);
  const std::string restore_code = error_code_of([&] {
    xiaotu::restore_database(future, data / "app.db");
  });
  check(restore_code == "SCHEMA_TOO_NEW", "restore_database 应拒绝不兼容备份");
  xiaotu::Db reopened(data / "app.db", 1);
  check(reopened.schema_version() == target, "拒绝恢复后目标库保持原版本");
  check(!std::filesystem::exists(data / "backups"), "拒绝发生在创建备份目录之前");
}

void test_backup_validation(const std::filesystem::path& root) {
  const int target = xiaotu::target_schema_version();
  const std::filesystem::path data = root / "validate-data";
  std::filesystem::create_directories(data);
  xiaotu::Db db(data / "app.db", 2);
  xiaotu::migrate(db);

  const xiaotu::BackupResult manual = xiaotu::run_manual_backup(db, data);
  check(manual.path.filename().string().find("-manual") != std::string::npos,
        "手动备份文件名带 manual 标记");

  bool valid = true;
  try {
    xiaotu::verify_backup_schema(manual.path, target);
  } catch (const std::exception&) {
    valid = false;
  }
  check(valid, "同版本备份应通过 schema 校验");

  check(error_code_of([&] { xiaotu::verify_backup_schema(manual.path, target + 1); }) ==
            "BACKUP_SCHEMA_MISMATCH",
        "备份版本与源库不一致应判定不可用");

  const std::filesystem::path unknown = root / "validate-unknown.db";
  sqlite3* connection = nullptr;
  check(sqlite3_open(unknown.string().c_str(), &connection) == SQLITE_OK, "创建无版本备份");
  if (connection != nullptr) {
    sqlite3_close(connection);
  }
  check(error_code_of([&] { xiaotu::verify_backup_schema(unknown, target); }) ==
            "BACKUP_SCHEMA_UNKNOWN",
        "读不到版本的备份应判定不可用");

  const std::filesystem::path future = root / "validate-future.db";
  std::filesystem::copy_file(manual.path, future,
                             std::filesystem::copy_options::overwrite_existing);
  check(sqlite3_open(future.string().c_str(), &connection) == SQLITE_OK, "打开未来版本备份");
  if (connection != nullptr) {
    const std::string sql = "PRAGMA user_version = " + std::to_string(target + 1) + ";";
    sqlite3_exec(connection, sql.c_str(), nullptr, nullptr, nullptr);
    sqlite3_close(connection);
  }
  check(error_code_of([&] { xiaotu::verify_backup_schema(future, target + 1); }) ==
            "BACKUP_SCHEMA_TOO_NEW",
        "高于程序支持的备份应判定不可用");

  // 备份目录被占用时手动备份明确失败。
  std::filesystem::create_directories(data / "backups" / ".backup.lock");
  check(error_code_of([&] { xiaotu::run_manual_backup(db, data); }) == "BACKUP_BUSY",
        "备份目录被占用时手动备份应返回 BACKUP_BUSY");
  std::filesystem::remove_all(data / "backups" / ".backup.lock");

  // 失败原因同时进入状态与审计，且不清掉上次成功。
  const std::string success_at = xiaotu::backup_status(data / "backups").last_success_at;
  xiaotu::record_backup_failure(db, data, "manual", "磁盘已满");
  const xiaotu::BackupStatus failed = xiaotu::backup_status(data / "backups");
  check(failed.last_failure_reason == "磁盘已满", "失败原因写入状态文件");
  check(failed.last_success_at == success_at, "失败不应清掉上次成功");
  bool audit_found = false;
  for (const auto& row : xiaotu::audit_recent(db, 20)) {
    if (row.action == "backup" && row.entity_no == "manual" &&
        row.detail.find("磁盘已满") != std::string::npos) {
      audit_found = true;
      break;
    }
  }
  check(audit_found, "备份失败应写审计并包含原因");
}

void test_daily_backup(const std::filesystem::path& root) {
  const std::filesystem::path data = root / "daily-data";
  std::filesystem::create_directories(data);
  xiaotu::Db db(data / "app.db", 2);
  xiaotu::migrate(db);

  check(xiaotu::daily_backup_due(data / "backups"), "首次启动应需要备份");

  const xiaotu::DailyBackupOutcome first = xiaotu::run_daily_backup(db, data);
  check(first.ran, "首次每日备份应执行");
  check(std::filesystem::exists(first.backup.path), "每日备份文件存在");
  check(first.backup.path.filename().string().find("-daily") != std::string::npos,
        "每日备份文件名带 daily 标记");

  const xiaotu::BackupStatus status = xiaotu::backup_status(data / "backups");
  check(status.last_success_at.size() == 19, "备份状态记录成功时间");
  check(status.last_success_bytes > 0, "备份状态记录大小");
  check(status.last_failure_at.empty(), "成功备份不产生失败记录");
  check(status.backup_count == 1, "备份状态统计可用备份数");
  check(xiaotu::database_writable(db), "正常库应判定为可写");
  check(!xiaotu::daily_backup_due(data / "backups"), "当天已备份后不再到期");

  // 成功与失败分别记录：手工写入一条失败状态，不影响上次成功。
  {
    std::ofstream out(data / "backups" / "backup-status.json", std::ios::trunc);
    out << R"({"last_success":{"at":"2026-11-13 09:00:00",)"
           R"("file":"app-20261113-090000-daily.db","bytes":4096},)"
           R"("last_failure":{"at":"2026-11-12 02:30:00","reason":"磁盘已满"},)"
           R"("schema_version":26})";
  }
  const xiaotu::BackupStatus mixed = xiaotu::backup_status(data / "backups");
  check(mixed.last_success_at == "2026-11-13 09:00:00" && !mixed.last_failure_at.empty(),
        "成功与失败状态可同时保留并区分");
  check(mixed.last_failure_reason == "磁盘已满", "失败原因可读取");

  const xiaotu::DailyBackupOutcome second = xiaotu::run_daily_backup(db, data);
  check(!second.ran, "同一天重复调用不再生成备份");
  check(second.skipped == "今天已备份", "重复调用说明跳过原因");
}

}  // namespace

int main() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "xiaotu_backup_smoke";
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  std::filesystem::create_directories(root);

  test_retention(root);
  test_schema_compatibility(root);
  test_daily_backup(root);
  test_backup_validation(root);

  std::filesystem::remove_all(root, ignored);
  if (g_failures != 0) {
    std::cerr << g_failures << " 项失败" << std::endl;
    return 1;
  }
  std::cout << "backup_smoke 全部通过" << std::endl;
  return 0;
}
