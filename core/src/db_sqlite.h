#pragma once

#include <filesystem>
#include <memory>

#include "db_backend.h"

namespace xiaotu {

// SQLite 后端：单写连接 + 只读连接池，WAL 模式。
// 所有 sqlite3 API 只允许出现在 db_sqlite.cpp 与 backup.cpp（备份是 SQLite 文件级特性）。
std::unique_ptr<IDbBackend> make_sqlite_backend(const std::filesystem::path& file,
                                                int read_connections);

}  // namespace xiaotu
