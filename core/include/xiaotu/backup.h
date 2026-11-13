#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "xiaotu/db.h"

namespace xiaotu {

struct BackupResult {
  std::filesystem::path path;
  std::uintmax_t bytes = 0;
};

// 恢复结果。safety_backup 为恢复前留下的 pre-restore-*.db；目标库原本不存在时为空。
struct RestoreResult {
  std::filesystem::path path;
  std::uintmax_t bytes = 0;
  int schema_version = 0;
  std::filesystem::path safety_backup;
};

// 自动备份保留策略：保留最近 30 份日备份，并在每个自然月额外保留最新的 1 份。
// 只处理由每日任务生成的 app-<时间戳>-daily.db；manual / pre-* 备份不参与清理。
inline constexpr int kDailyBackupKeep = 30;
inline constexpr int kMonthlyBackupKeep = 1;

// 解析实际备份目录：
//   - 配置为空 → data_dir / "backups"
//   - 相对路径 → data_dir / 配置值（不受启动 cwd 影响）
//   - 绝对路径 → 原样使用
std::filesystem::path resolve_backup_dir(const std::filesystem::path& data_dir,
                                         const std::string& configured);

// 递归创建备份目录，并写一个临时探针文件确认可写（探针随即删除）。
// 创建失败、不是目录或不可写时抛 BACKUP_DIR_UNWRITABLE，信息带目录路径。
void ensure_backup_directory(const std::filesystem::path& directory);

// 用 VACUUM INTO 生成一致性快照，文件名带时间戳，目录不存在会创建。
BackupResult backup_database(Db& db, const std::filesystem::path& directory,
                             std::string_view tag);

// 打开备份并执行 PRAGMA integrity_check，失败抛 AppError。
void verify_database(const std::filesystem::path& path);

// 用备份替换目标数据库；目标已存在时先留一份 pre-restore 安全备份，
// 并清理 -wal / -shm。调用前必须确保没有进程占用目标数据库。
// 恢复前会比对备份的 schema 版本：高于当前程序支持的版本一律拒绝。
//
// 传了 actor 就写 restore 审计（成功与失败各一条，entity=database，
// entity_no=备份文件名，detail 含 schema 版本/来源/安全备份文件名/失败原因）。
// CLI 约定 actor="cli"、origin="命令行"：命令行没有登录用户，不伪造用户身份。
// backup_dir 为空时沿用旧行为：<目标库所在目录>/backups。
RestoreResult restore_database(const std::filesystem::path& source,
                               const std::filesystem::path& database_path,
                               std::string_view actor = {}, std::string_view origin = {},
                               const std::filesystem::path& backup_dir = {});

// 读取备份内的 schema 版本：优先 schema_migration 的最大 version，退回 user_version。
// 无法打开或不是本程序的数据库时抛出 AppError。
int backup_schema_version(const std::filesystem::path& backup);

// 校验刚生成的备份可用：备份自记录的版本必须等于源库版本，且不高于程序支持版本。
// 读不到版本、版本不一致或高于当前程序支持都抛 AppError，视为备份不可用于恢复。
void verify_backup_schema(const std::filesystem::path& backup, int expected_version);

// 恢复前校验兼容性：备份版本高于当前程序支持 → SCHEMA_TOO_NEW；
// 识别不到版本 → SCHEMA_UNKNOWN。旧版本允许恢复（下次启动会迁移）。
void ensure_restore_compatible(const std::filesystem::path& backup);

// 备份目录内可恢复的 .db 文件。schema_version 读不到时为 0，error 给出原因；
// 列表本身不抛异常，单个文件损坏不影响其他项展示。
struct BackupFileInfo {
  std::string file;  // 文件名，不含目录
  std::filesystem::path path;
  std::uintmax_t bytes = 0;
  std::string modified_at;  // 本地时间 YYYY-MM-DD HH:MM:SS
  int schema_version = 0;
  bool readable = false;
  std::string error;
};

// 按修改时间倒序列出目录内的 .db 文件；目录不存在时返回空列表。
std::vector<BackupFileInfo> list_backups(const std::filesystem::path& directory);

// 恢复前检查：能否打开、完整性是否通过、是否本程序的库、schema 是否兼容。
// 预检查只读，不修改备份与当前库。
struct BackupInspection {
  bool ok = false;
  bool is_app_database = false;
  bool integrity_ok = false;
  int schema_version = 0;
  int current_schema_version = 0;
  bool compatible = false;   // schema 不高于当前程序支持的版本
  bool will_migrate = false; // schema 低于当前版本，恢复后启动时会迁移
  std::uintmax_t bytes = 0;
  std::string modified_at;
  std::string error_code;
  std::string error_message;
};

BackupInspection inspect_backup(const std::filesystem::path& backup);

struct RetentionResult {
  std::size_t kept = 0;
  std::size_t removed = 0;
};

// 清理日备份：保留最近 daily_keep 份，并额外保留每个自然月最新的一份。
// 返回保留与删除的数量；幂等（重复执行不会再删）。
RetentionResult apply_daily_retention(const std::filesystem::path& directory,
                                      int daily_keep = kDailyBackupKeep);

// 备份目录的状态，来源是目录内的 backup-status.json 与现存 .db 文件数。
// 成功与失败分别记录：一次失败不会清掉上次成功信息，反之亦然。
struct BackupStatus {
  std::string last_success_at;    // 本地时间 YYYY-MM-DD HH:MM:SS；空表示从未成功
  std::string last_success_file;  // 文件名（不含目录）
  std::uintmax_t last_success_bytes = 0;
  int schema_version = 0;
  std::string last_failure_at;      // 空表示没有失败记录
  std::string last_failure_reason;
  std::size_t backup_count = 0;  // 目录内 .db 文件数（含手动与安全备份）
};

BackupStatus backup_status(const std::filesystem::path& directory);

// 判断数据库当前是否可写：用 BEGIN IMMEDIATE 探测写锁后立即回滚，
// 只读文件、磁盘满、被外部锁占用都会返回 false，不抛异常。
bool database_writable(Db& db);

// 今天是否还需要执行每日备份：今天已有成功的每日备份则返回 false。
// status 丢失时按目录内今天的 daily 文件兜底。
bool daily_backup_due(const std::filesystem::path& directory);

struct DailyBackupOutcome {
  bool ran = false;
  std::string skipped;  // 非空表示未执行的原因（如另一进程正在备份）
  BackupResult backup;
  RetentionResult retention;
};

// 生成一份 daily 备份 → 执行保留策略 → 写状态文件。今天已成功备份过则 ran=false。
// 备份失败会写 failed 状态并把异常原样抛出。
// backup_dir 为空时使用 data_dir / "backups"（兼容旧调用）。
DailyBackupOutcome run_daily_backup(Db& db, const std::filesystem::path& data_dir,
                                    const std::filesystem::path& backup_dir = {});

// 生成一份 manual 备份（CLI 与 POST /api/backup 共用）：写状态、成功/失败审计。
// 另一个备份任务正在执行时抛 BACKUP_BUSY；backup_dir 语义同 run_daily_backup。
BackupResult run_manual_backup(Db& db, const std::filesystem::path& data_dir,
                               const std::filesystem::path& backup_dir = {});

// 记录一次备份失败：更新状态文件的 last_failure 并写 backup 审计（失败原因）。
// 数据库不可写时退化为只写日志，不抛异常。backup_dir 为空时使用 data_dir / "backups"。
void record_backup_failure(Db& db, const std::filesystem::path& data_dir, std::string_view tag,
                           std::string_view reason,
                           const std::filesystem::path& backup_dir = {});

}  // namespace xiaotu
