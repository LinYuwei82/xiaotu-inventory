#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>

#include "xiaotu/db.h"

namespace xiaotu {

struct BackupResult {
  std::filesystem::path path;
  std::uintmax_t bytes = 0;
};

// 用 VACUUM INTO 生成一致性快照，文件名带时间戳，目录不存在会创建。
BackupResult backup_database(Db& db, const std::filesystem::path& directory,
                             std::string_view tag);

// 打开备份并执行 PRAGMA integrity_check，失败抛 AppError。
void verify_database(const std::filesystem::path& path);

// 用备份替换目标数据库；目标已存在时先留一份 pre-restore 安全备份，
// 并清理 -wal / -shm。调用前必须确保没有进程占用目标数据库。
BackupResult restore_database(const std::filesystem::path& source,
                              const std::filesystem::path& database_path);

}  // namespace xiaotu
