#include "db_sqlite.h"

#include <sqlite3.h>

#include <string>

#include "xiaotu/error.h"
#include "xiaotu/platform.h"

namespace xiaotu {
namespace {

std::string sqlite_message(sqlite3* db) {
  return db != nullptr ? std::string(sqlite3_errmsg(db)) : std::string("unknown sqlite error");
}

void check_ok(sqlite3* db, int rc, std::string_view what) {
  if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW) {
    throw AppError("DB_ERROR", std::string(what) + ": " + sqlite_message(db));
  }
}

class SqliteStmt : public IStmt {
 public:
  SqliteStmt(sqlite3* db, std::string_view sql) : db_(db) {
    const int rc = sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &stmt_,
                                      nullptr);
    if (rc != SQLITE_OK) {
      throw AppError("DB_PREPARE",
                     "SQL 准备失败: " + sqlite_message(db) + " | " + std::string(sql));
    }
  }
  ~SqliteStmt() override {
    if (stmt_ != nullptr) {
      sqlite3_finalize(stmt_);
    }
  }

  void bind_i64(int index, std::int64_t value) override {
    check_ok(db_, sqlite3_bind_int64(stmt_, index, value), "绑定整数参数");
  }
  void bind_text(int index, std::string_view value) override {
    check_ok(db_,
             sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()),
                               SQLITE_TRANSIENT),
             "绑定文本参数");
  }
  bool step() override {
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) {
      return true;
    }
    if (rc == SQLITE_DONE) {
      return false;
    }
    throw AppError("DB_STEP", "SQL 执行失败: " + sqlite_message(db_));
  }
  std::int64_t i64(int column) const override { return sqlite3_column_int64(stmt_, column); }
  std::string text(int column) const override {
    const auto* data = sqlite3_column_text(stmt_, column);
    const int size = sqlite3_column_bytes(stmt_, column);
    if (data == nullptr) {
      return {};
    }
    return std::string(reinterpret_cast<const char*>(data), static_cast<std::size_t>(size));
  }

 private:
  sqlite3* db_ = nullptr;
  sqlite3_stmt* stmt_ = nullptr;
};

class SqliteConn : public IConn {
 public:
  explicit SqliteConn(sqlite3* db) : db_(db) {}
  ~SqliteConn() override {
    // 必须关连接：Db 析构后文件句柄若残留，Windows 上 remove_all/恢复清理
    // 会因文件占用失败（mac 允许删除打开的文件，问题会被掩盖）。
    if (db_ != nullptr) {
      sqlite3_close(db_);
    }
  }

  std::unique_ptr<IStmt> prepare(std::string_view sql) override {
    return std::make_unique<SqliteStmt>(db_, sql);
  }
  void exec(std::string_view sql) override {
    char* error = nullptr;
    const int rc = sqlite3_exec(db_, std::string(sql).c_str(), nullptr, nullptr, &error);
    if (rc != SQLITE_OK) {
      const std::string message = error != nullptr ? std::string(error) : sqlite_message(db_);
      sqlite3_free(error);
      throw AppError("DB_EXEC", "SQL 执行失败: " + message + " | " + std::string(sql));
    }
  }
  int changes() const override { return sqlite3_changes(db_); }
  std::int64_t last_insert_id() const override { return sqlite3_last_insert_rowid(db_); }
  void begin_write() override { exec("BEGIN IMMEDIATE;"); }
  void begin_read() override { exec("BEGIN;"); }
  void commit() override { exec("COMMIT;"); }
  void rollback() noexcept override {
    try {
      exec("ROLLBACK;");
    } catch (...) {
    }
  }
  std::int64_t schema_version() const override {
    std::int64_t version = 0;
    SqliteStmt stmt(db_, "PRAGMA user_version;");
    if (stmt.step()) {
      version = stmt.i64(0);
    }
    return version;
  }

 private:
  sqlite3* db_ = nullptr;
};

sqlite3* open_connection(const std::filesystem::path& file, int flags) {
  sqlite3* db = nullptr;
  const int rc = sqlite3_open_v2(path_to_utf8(file).c_str(), &db,
                                 flags | SQLITE_OPEN_FULLMUTEX, nullptr);
  if (rc != SQLITE_OK) {
    const std::string message = sqlite_message(db);
    sqlite3_close(db);
    throw AppError("DB_OPEN", "打开数据库失败: " + message);
  }
  return db;
}

class SqliteBackend : public IDbBackend {
 public:
  SqliteBackend(const std::filesystem::path& file, int /*read_connections*/) : file_(file) {
    write_ = std::make_unique<SqliteConn>(
        open_connection(file, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE));
    // WAL 让读不阻塞写；busy_timeout 兜底多进程；外键约束必须显式打开。
    write_->exec("PRAGMA journal_mode=WAL;");
    write_->exec("PRAGMA synchronous=NORMAL;");
    write_->exec("PRAGMA busy_timeout=5000;");
    write_->exec("PRAGMA foreign_keys=ON;");
  }

  IConn& write_conn() override { return *write_; }

  std::vector<std::unique_ptr<IConn>> create_read_conns(int count) override {
    std::vector<std::unique_ptr<IConn>> conns;
    conns.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
      std::unique_ptr<SqliteConn> reader =
          std::make_unique<SqliteConn>(open_connection(file_, SQLITE_OPEN_READONLY));
      reader->exec("PRAGMA busy_timeout=5000;");
      reader->exec("PRAGMA foreign_keys=ON;");
      reader->exec("PRAGMA query_only=ON;");
      conns.push_back(std::move(reader));
    }
    return conns;
  }

 private:
  std::filesystem::path file_;
  std::unique_ptr<SqliteConn> write_;
};

}  // namespace

std::unique_ptr<IDbBackend> make_sqlite_backend(const std::filesystem::path& file,
                                                int read_connections) {
  return std::make_unique<SqliteBackend>(file, read_connections);
}

}  // namespace xiaotu
