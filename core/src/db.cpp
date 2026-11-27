#include "xiaotu/db.h"

#include <algorithm>
#include <ctime>
#include <utility>

#include "db_sqlite.h"
#include "xiaotu/error.h"
#include "xiaotu/platform.h"

namespace xiaotu {

// ---- Conn ----

int Conn::changes() const { return raw_.changes(); }

std::int64_t Conn::last_insert_id() const { return raw_.last_insert_id(); }

// ---- Stmt ----

Stmt::Stmt(Conn& conn, std::string_view sql) : impl_(conn.raw_.prepare(sql)) {}

Stmt::~Stmt() = default;

Stmt& Stmt::bind(std::int64_t index, std::int64_t value) {
  impl_->bind_i64(static_cast<int>(index), value);
  return *this;
}

Stmt& Stmt::bind(std::int64_t index, std::string_view value) {
  impl_->bind_text(static_cast<int>(index), value);
  return *this;
}

bool Stmt::step() { return impl_->step(); }

void Stmt::run() {
  while (step()) {
  }
}

std::int64_t Stmt::i64(int column) const { return impl_->i64(column); }

std::string Stmt::text(int column) const { return impl_->text(column); }

// ---- Db ----

Db::Db(std::filesystem::path file, int read_connections, std::chrono::milliseconds wait_timeout)
    : file_(std::move(file)), wait_timeout_(wait_timeout) {
  if (file_.has_parent_path()) {
    std::filesystem::create_directories(file_.parent_path());
  }
  // 当前唯一后端是 SQLite；接入 PostgreSQL 时改为按配置选择工厂。
  backend_ = make_sqlite_backend(file_, read_connections);

  const int count = std::max(1, read_connections);
  for (auto& conn : backend_->create_read_conns(count)) {
    reads_.push_back(ReadSlot{std::move(conn), false});
  }
}

Db::~Db() = default;

void Db::write(const std::function<void(Conn&)>& fn) {
  // 写锁不能无限等：一次大导入占住写连接时，其他写请求（保存单据、记审计）
  // 会全部堆在这里，客户端也拿不到任何反馈。超时让调用方回 503 重试。
  std::unique_lock<std::timed_mutex> lock(write_mutex_, std::defer_lock);
  if (!lock.try_lock_for(wait_timeout_)) {
    throw AppError("DB_BUSY", "数据库繁忙，请稍后重试");
  }
  Conn conn{backend_->write_conn()};
  conn.raw_.begin_write();
  try {
    fn(conn);
    conn.raw_.commit();
  } catch (...) {
    conn.raw_.rollback();
    throw;
  }
}

void Db::write_raw(const std::function<void(Conn&)>& fn) {
  std::unique_lock<std::timed_mutex> lock(write_mutex_, std::defer_lock);
  if (!lock.try_lock_for(wait_timeout_)) {
    throw AppError("DB_BUSY", "数据库繁忙，请稍后重试");
  }
  Conn conn{backend_->write_conn()};
  fn(conn);
}

void Db::read(const std::function<void(Conn&)>& fn) const {
  std::size_t index = reads_.size();
  {
    std::unique_lock<std::mutex> lock(pool_mutex_);
    // 不能无限等：读连接被长导出占满时，包括会话校验在内的所有读请求都会挂住，
    // 整个服务失去响应。超时后让调用方回 503，由前端重试。
    const bool acquired =
        pool_cv_.wait_for(lock, wait_timeout_, [this] {
          return std::any_of(reads_.begin(), reads_.end(),
                             [](const ReadSlot& slot) { return !slot.busy; });
        });
    if (!acquired) {
      throw AppError("DB_BUSY", "数据库繁忙，请稍后重试");
    }
    for (std::size_t i = 0; i < reads_.size(); ++i) {
      if (!reads_[i].busy) {
        reads_[i].busy = true;
        index = i;
        break;
      }
    }
  }

  auto release = [&] {
    {
      std::lock_guard<std::mutex> lock(pool_mutex_);
      reads_[index].busy = false;
    }
    pool_cv_.notify_one();
  };

  IConn& raw = *reads_[index].conn;
  Conn conn{raw};
  try {
    raw.begin_read();
    fn(conn);
    raw.commit();
  } catch (...) {
    raw.rollback();
    release();
    throw;
  }
  release();
}

std::int64_t Db::schema_version() const {
  std::int64_t version = 0;
  read([&](Conn& conn) { version = conn.raw_.schema_version(); });
  return version;
}

void exec_sql(Conn& conn, std::string_view sql) { conn.raw_.exec(sql); }

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
