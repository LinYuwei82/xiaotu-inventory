#include "xiaotu/db.h"

#include <sqlite3.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <utility>

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

void open_connection(const std::filesystem::path& file, int flags, sqlite3** out) {
  sqlite3* db = nullptr;
  const int rc = sqlite3_open_v2(path_to_utf8(file).c_str(), &db,
                                 flags | SQLITE_OPEN_FULLMUTEX, nullptr);
  if (rc != SQLITE_OK) {
    const std::string message = sqlite_message(db);
    sqlite3_close(db);
    throw AppError("DB_OPEN", "打开数据库失败: " + message);
  }
  *out = db;
}

}  // namespace

Stmt::Stmt(sqlite3* db, std::string_view sql) : db_(db) {
  const int rc = sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &stmt_, nullptr);
  if (rc != SQLITE_OK) {
    throw AppError("DB_PREPARE", "SQL 准备失败: " + sqlite_message(db) + " | " + std::string(sql));
  }
}

Stmt::~Stmt() {
  if (stmt_ != nullptr) {
    sqlite3_finalize(stmt_);
  }
}

Stmt& Stmt::bind(std::int64_t index, std::int64_t value) {
  check_ok(db_, sqlite3_bind_int64(stmt_, static_cast<int>(index), value), "绑定整数参数");
  return *this;
}

Stmt& Stmt::bind(std::int64_t index, std::string_view value) {
  check_ok(db_,
           sqlite3_bind_text(stmt_, static_cast<int>(index), value.data(),
                             static_cast<int>(value.size()), SQLITE_TRANSIENT),
           "绑定文本参数");
  return *this;
}

bool Stmt::step() {
  const int rc = sqlite3_step(stmt_);
  if (rc == SQLITE_ROW) {
    return true;
  }
  if (rc == SQLITE_DONE) {
    return false;
  }
  throw AppError("DB_STEP", "SQL 执行失败: " + sqlite_message(db_));
}

void Stmt::run() {
  while (step()) {
  }
}

std::int64_t Stmt::i64(int column) const {
  return sqlite3_column_int64(stmt_, column);
}

std::string Stmt::text(int column) const {
  const auto* data = sqlite3_column_text(stmt_, column);
  const int size = sqlite3_column_bytes(stmt_, column);
  if (data == nullptr) {
    return {};
  }
  return std::string(reinterpret_cast<const char*>(data), static_cast<std::size_t>(size));
}

Db::Db(std::filesystem::path file, int read_connections) : file_(std::move(file)) {
  if (file_.has_parent_path()) {
    std::filesystem::create_directories(file_.parent_path());
  }

  open_connection(file_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, &write_);
  exec_sql(write_, "PRAGMA journal_mode=WAL;");
  exec_sql(write_, "PRAGMA synchronous=NORMAL;");
  exec_sql(write_, "PRAGMA busy_timeout=5000;");
  exec_sql(write_, "PRAGMA foreign_keys=ON;");

  const int count = std::max(1, read_connections);
  reads_.reserve(static_cast<std::size_t>(count));
  read_busy_.reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    sqlite3* reader = nullptr;
    open_connection(file_, SQLITE_OPEN_READONLY, &reader);
    exec_sql(reader, "PRAGMA busy_timeout=5000;");
    exec_sql(reader, "PRAGMA foreign_keys=ON;");
    exec_sql(reader, "PRAGMA query_only=ON;");
    reads_.push_back(reader);
    read_busy_.push_back(false);
  }
}

Db::~Db() {
  for (sqlite3* reader : reads_) {
    sqlite3_close(reader);
  }
  reads_.clear();
  read_busy_.clear();
  if (write_ != nullptr) {
    sqlite3_close(write_);
    write_ = nullptr;
  }
}

void Db::write(const std::function<void(sqlite3*)>& fn) {
  std::lock_guard<std::mutex> lock(write_mutex_);
  exec_sql(write_, "BEGIN IMMEDIATE;");
  try {
    fn(write_);
    exec_sql(write_, "COMMIT;");
  } catch (...) {
    try {
      exec_sql(write_, "ROLLBACK;");
    } catch (...) {
    }
    throw;
  }
}

void Db::write_raw(const std::function<void(sqlite3*)>& fn) {
  std::lock_guard<std::mutex> lock(write_mutex_);
  fn(write_);
}

void Db::read(const std::function<void(sqlite3*)>& fn) const {
  std::size_t index = read_busy_.size();
  {
    std::unique_lock<std::mutex> lock(pool_mutex_);
    pool_cv_.wait(lock, [this] {
      return std::any_of(read_busy_.begin(), read_busy_.end(), [](bool busy) { return !busy; });
    });
    for (std::size_t i = 0; i < read_busy_.size(); ++i) {
      if (!read_busy_[i]) {
        read_busy_[i] = true;
        index = i;
        break;
      }
    }
  }

  auto release = [&] {
    {
      std::lock_guard<std::mutex> lock(pool_mutex_);
      read_busy_[index] = false;
    }
    pool_cv_.notify_one();
  };

  sqlite3* conn = reads_[index];
  try {
    exec_sql(conn, "BEGIN;");
    fn(conn);
    exec_sql(conn, "COMMIT;");
  } catch (...) {
    try {
      exec_sql(conn, "ROLLBACK;");
    } catch (...) {
    }
    release();
    throw;
  }
  release();
}

std::int64_t Db::schema_version() const {
  std::int64_t version = 0;
  read([&](sqlite3* conn) {
    Stmt stmt(conn, "PRAGMA user_version;");
    if (stmt.step()) {
      version = stmt.i64(0);
    }
  });
  return version;
}

void exec_sql(sqlite3* db, std::string_view sql) {
  char* error = nullptr;
  const int rc = sqlite3_exec(db, std::string(sql).c_str(), nullptr, nullptr, &error);
  if (rc != SQLITE_OK) {
    const std::string message = error != nullptr ? std::string(error) : sqlite_message(db);
    sqlite3_free(error);
    throw AppError("DB_EXEC", "SQL 执行失败: " + message + " | " + std::string(sql));
  }
}

std::string now_text() {
  std::time_t now = std::time(nullptr);
  const std::tm local = local_time(now);
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local);
  return std::string(buffer);
}

std::string today_text() {
  std::time_t now = std::time(nullptr);
  const std::tm local = local_time(now);
  char buffer[16];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &local);
  return std::string(buffer);
}

}  // namespace xiaotu
