// 备份与恢复补强项的冒烟测试：保留策略、schema 兼容性、每日备份与状态。
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>

#include <sqlite3.h>

#include "xiaotu/audit.h"
#include "xiaotu/backup.h"
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
  // 非本程序命名规则的文件一律跳过，不参与统计与删除。
  touch(directory, "notes.txt");
  touch(directory, "backup-status.json");
  touch(directory, "app-20240101-120000-dailydb.db");
  touch(directory, "app-20240101-120000-daily.db.bak");
  touch(directory, "pre-import-item-20240101.db");

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
  for (const char* name : {"notes.txt", "backup-status.json", "app-20240101-120000-dailydb.db",
                           "app-20240101-120000-daily.db.bak", "pre-import-item-20240101.db"}) {
    check(std::filesystem::exists(directory / name),
          std::string("异名文件不参与清理：") + name);
  }

  const xiaotu::RetentionResult again = xiaotu::apply_daily_retention(directory, 30);
  check(again.kept == 33 && again.removed == 0, "重复清理幂等");
}

void test_retention_monthly_beyond_window(const std::filesystem::path& root) {
  const std::filesystem::path directory = root / "retention-monthly";
  std::filesystem::create_directories(directory);

  // 8 月 10 份、9 月 20 份、10 月 10 份，共 40 份；最近 30 份窗口只覆盖 9、10 月。
  for (int day = 1; day <= 10; ++day) touch(directory, daily_name(2026, 8, day));
  for (int day = 1; day <= 20; ++day) touch(directory, daily_name(2026, 9, day));
  for (int day = 1; day <= 10; ++day) touch(directory, daily_name(2026, 10, day));
  touch(directory, manual_name(2026, 8, 5));
  touch(directory, manual_name(2026, 9, 15));
  touch(directory, "app-20260801-090000-pre-v35.db");
  touch(directory, "app-20260901-090000-pre-v35.db");
  touch(directory, "pre-restore-20261001-090000.db");

  const xiaotu::RetentionResult result = xiaotu::apply_daily_retention(directory, 30);
  check(result.kept == 31, "40 份日备份收口为最近 30 份 + 窗口外 8 月代表 1 份");
  check(result.removed == 9, "8 月除代表外的 9 份被清理");
  check(std::filesystem::exists(directory / daily_name(2026, 8, 10)),
        "窗口外的月份代表保留");
  check(!std::filesystem::exists(directory / daily_name(2026, 8, 9)),
        "同月更旧的备份被清理");
  check(std::filesystem::exists(directory / daily_name(2026, 9, 20)) &&
            std::filesystem::exists(directory / daily_name(2026, 10, 10)),
        "窗口内各月最新一份保留");
  check(std::filesystem::exists(directory / manual_name(2026, 8, 5)) &&
            std::filesystem::exists(directory / manual_name(2026, 9, 15)),
        "手动备份不参与清理");
  check(std::filesystem::exists(directory / "app-20260801-090000-pre-v35.db") &&
            std::filesystem::exists(directory / "app-20260901-090000-pre-v35.db") &&
            std::filesystem::exists(directory / "pre-restore-20261001-090000.db"),
        "迁移前与恢复前备份不参与清理");

  const xiaotu::RetentionResult again = xiaotu::apply_daily_retention(directory, 30);
  check(again.kept == 31 && again.removed == 0, "重复清理不再删除");
}

void test_backup_directory(const std::filesystem::path& root) {
  namespace fs = std::filesystem;
  const fs::path data = root / "dir-data";
  fs::create_directories(data);
  xiaotu::Db db(data / "app.db", 2);
  xiaotu::migrate(db);

  // 默认回落：未配置/空串 → <data_dir>/backups。
  check(xiaotu::resolve_backup_dir(data, "") == data / "backups",
        "空配置应回落到 <data_dir>/backups");
  // 绝对路径原样使用。用平台自己的绝对路径：Windows 下 "/tmp/..." 没有盘符，
  // 按 fs::path 的判定不算绝对路径，只有 root（临时目录）这种带盘符的才算。
  const fs::path absolute_backups = root / "abs-backups";
  check(absolute_backups.is_absolute(), "测试用的绝对路径在当前平台确实为绝对路径");
  check(xiaotu::resolve_backup_dir(data, absolute_backups.string()) == absolute_backups,
        "绝对路径应原样使用");
  // 相对路径按 data_dir 解析，不受 cwd 影响。
  check(xiaotu::resolve_backup_dir(data, "nested/bk") == data / "nested/bk",
        "相对路径应按数据目录解析");

  // 自定义绝对目录：不存在时自动递归创建，且不写默认目录。
  const fs::path custom = root / "custom-backups" / "deep";
  check(!fs::exists(custom), "自定义备份目录初始不存在");
  const xiaotu::BackupResult first = xiaotu::run_manual_backup(db, data, custom);
  check(first.path.parent_path() == custom, "手动备份应写入自定义目录");
  check(fs::is_directory(custom), "备份前应自动创建目录");
  check(!fs::exists(data / "backups"), "自定义目录生效时不应写默认目录");

  // 相对配置解析出的目录同样生效。
  const fs::path relative = xiaotu::resolve_backup_dir(data, "relative-backups");
  xiaotu::ensure_backup_directory(relative);
  check(fs::is_directory(data / "relative-backups"), "相对目录应创建在数据目录下");

  // 备份目录不可用：必须报 BACKUP_DIR_UNWRITABLE，且不回落到默认目录。
  // 两个平台制造「不可用」的手段不同，但验证的是同一个错误码：
  // - POSIX：目录存在、去掉写权限。走的是 create_directories 成功、is_directory 为真、
  //   探针文件写失败这条路径（覆盖实现里的写探针）。
  // - Windows：chmod 语义拦不住创建文件，改用「同名普通文件占据该路径」。
  //   走的是 create_directories 失败、is_directory 为假这条路径。
  const fs::path locked = root / "locked-backups";
#if defined(_WIN32)
  {
    std::ofstream placeholder(locked, std::ios::binary | std::ios::trunc);
    placeholder << "occupied";
    placeholder.close();
  }
  check(fs::is_regular_file(locked), "Windows：占用路径的普通文件已就位");
#else
  fs::create_directories(locked);
  fs::permissions(locked, fs::perms::owner_read | fs::perms::owner_exec,
                  fs::perm_options::replace);
#endif
  check(error_code_of([&] { xiaotu::ensure_backup_directory(locked); }) ==
            "BACKUP_DIR_UNWRITABLE",
        "不可写目录应报 BACKUP_DIR_UNWRITABLE");
  fs::remove_all(data / "backups");
  check(error_code_of([&] { xiaotu::run_manual_backup(db, data, locked); }) ==
            "BACKUP_DIR_UNWRITABLE",
        "不可写备份目录时手动备份应失败");
  check(!fs::exists(data / "backups"), "不可写时不得静默回落到默认目录");
#if defined(_WIN32)
  fs::remove(locked);
#else
  fs::permissions(locked, fs::perms::owner_all, fs::perm_options::replace);
#endif

  // 恢复前安全备份也写入解析后的目录。
  const xiaotu::BackupResult snapshot = xiaotu::backup_database(db, custom, "smoke");
  const fs::path restored = root / "restored-custom.db";
  fs::copy_file(snapshot.path, restored);
  const fs::path safety_dir = root / "restore-safety";
  const xiaotu::RestoreResult restored_result =
      xiaotu::restore_database(snapshot.path, restored, "cli", "命令行", safety_dir);
  check(!restored_result.safety_backup.empty(), "覆盖恢复应留下安全备份");
  check(restored_result.safety_backup.parent_path() == safety_dir,
        "恢复前安全备份应写入指定目录");
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

void test_restore_wizard_support(const std::filesystem::path& root) {
  const int target = xiaotu::target_schema_version();
  const std::filesystem::path data = root / "wizard-data";
  std::filesystem::create_directories(data);
  xiaotu::Db db(data / "app.db", 2);
  xiaotu::migrate(db);
  const xiaotu::BackupResult manual = xiaotu::run_manual_backup(db, data);

  const xiaotu::BackupInspection inspection = xiaotu::inspect_backup(manual.path);
  check(inspection.ok && inspection.is_app_database && inspection.integrity_ok,
        "预检查应识别可恢复的本程序备份");
  check(inspection.schema_version == target, "预检查返回备份 schema");
  check(inspection.compatible && !inspection.will_migrate, "同版本备份不需要迁移");

  const auto items = xiaotu::list_backups(data / "backups");
  check(!items.empty() && items.front().file == manual.path.filename().string(),
        "备份列表应包含刚生成的备份");
  check(items.front().readable && items.front().schema_version == target,
        "列表项应带 schema 与可读标记");
  check(!items.front().modified_at.empty(), "列表项应带修改时间");

  // 非 SQLite 文件：列表标记不可读，预检查返回 BACKUP_INVALID。
  const std::filesystem::path broken = data / "backups" / "broken.db";
  std::ofstream(broken) << "not a database";
  check(xiaotu::inspect_backup(broken).error_code == "BACKUP_INVALID",
        "损坏文件预检查应返回 BACKUP_INVALID");
  bool broken_listed = false;
  for (const auto& item : xiaotu::list_backups(data / "backups")) {
    if (item.file == "broken.db") {
      broken_listed = true;
      check(!item.readable, "损坏文件应标记不可读");
    }
  }
  check(broken_listed, "备份列表应包含损坏文件且不中断");
  std::filesystem::remove(broken);

  // 非本程序的 SQLite 库：预检查返回 NOT_APP_DATABASE。
  const std::filesystem::path foreign = root / "foreign.db";
  sqlite3* connection = nullptr;
  check(sqlite3_open(foreign.string().c_str(), &connection) == SQLITE_OK, "创建非本程序库");
  if (connection != nullptr) {
    sqlite3_exec(connection, "CREATE TABLE t(x);", nullptr, nullptr, nullptr);
    sqlite3_close(connection);
  }
  check(xiaotu::inspect_backup(foreign).error_code == "NOT_APP_DATABASE",
        "非本程序库预检查应返回 NOT_APP_DATABASE");

  // 恢复源损坏时必须在覆盖前失败，目标库仍可打开且版本不变。
  const std::filesystem::path corrupt = root / "corrupt.db";
  std::ofstream(corrupt) << "broken";
  const std::string code =
      error_code_of([&] { xiaotu::restore_database(corrupt, data / "app.db", "test", "单元测试"); });
  check(code == "BACKUP_INVALID" || code == "RESTORE_FAILED", "损坏备份恢复应失败");
  xiaotu::Db reopened(data / "app.db", 1);
  check(reopened.schema_version() == target, "恢复失败后目标库仍可打开且版本不变");
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
  test_retention_monthly_beyond_window(root);
  test_backup_directory(root);
  test_schema_compatibility(root);
  test_daily_backup(root);
  test_restore_wizard_support(root);
  test_backup_validation(root);

  std::filesystem::remove_all(root, ignored);
  if (g_failures != 0) {
    std::cerr << g_failures << " 项失败" << std::endl;
    return 1;
  }
  std::cout << "backup_smoke 全部通过" << std::endl;
  return 0;
}
