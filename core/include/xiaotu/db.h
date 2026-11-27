#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace xiaotu {

// 后端接口（定义在 core/src/db_backend.h，业务代码不直接接触）：
// 抽象层允许未来以 PostgreSQL 实现同一接口，业务层只见 Conn/Stmt/Db。
class IConn;
class IStmt;
class IDbBackend;

// 后端连接的不透明句柄，作为 Db::read/write 回调的参数传入。
// 事务由 Db 包裹（write=写事务，read=只读事务），Conn 本身不管生命周期。
class Conn {
 public:
  Conn(const Conn&) = delete;
  Conn& operator=(const Conn&) = delete;

  // 最近一次 INSERT/UPDATE/DELETE 影响的行数（替代 sqlite3_changes）。
  int changes() const;
  // 最近一次插入行的自增主键（替代 sqlite3_last_insert_rowid）。
  std::int64_t last_insert_id() const;

 private:
  friend class Db;
  friend class Stmt;
  friend void exec_sql(Conn&, std::string_view);
  explicit Conn(IConn& raw) : raw_(raw) {}
  IConn& raw_;
};

// 预编译语句的 RAII 薄封装：只做绑定、单步与取值，不含业务语义与后端类型。
class Stmt {
 public:
  Stmt(Conn& conn, std::string_view sql);
  ~Stmt();

  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;

  Stmt& bind(std::int64_t index, std::int64_t value);
  Stmt& bind(std::int64_t index, std::string_view value);

  bool step();  // 有结果行返回 true
  void run();   // 执行到完成，忽略结果行

  std::int64_t i64(int column) const;
  std::string text(int column) const;

 private:
  std::unique_ptr<IStmt> impl_;
};

// 连接管理（后端中立）：
//   - 写：单连接 + 互斥，write() 内含一个写事务
//   - 读：小连接池，read() 内含只读事务
// 单写连接是本方案「先查余额再扣减」得以原子的前提；
// PostgreSQL 后端以行锁等价语义实现（见后续阶段）。
class Db {
 public:
  // 当前仅 SQLite 后端；后续按配置选择后端时改为工厂。
  // wait_timeout：取读连接或写锁的最长等待，超时抛 AppError("DB_BUSY")。
  explicit Db(std::filesystem::path file, int read_connections = 4,
              std::chrono::milliseconds wait_timeout = std::chrono::seconds(10));
  ~Db();

  Db(const Db&) = delete;
  Db& operator=(const Db&) = delete;

  // 串行执行并包裹写事务；fn 抛异常则回滚。
  void write(const std::function<void(Conn&)>& fn);
  // 串行执行但不包裹事务，供迁移脚本自行控制 BEGIN/COMMIT。
  void write_raw(const std::function<void(Conn&)>& fn);
  // 从连接池取一个读连接，包裹只读事务。
  void read(const std::function<void(Conn&)>& fn) const;

  std::int64_t schema_version() const;
  const std::filesystem::path& file() const { return file_; }

 private:
  std::filesystem::path file_;
  std::unique_ptr<IDbBackend> backend_;

  std::timed_mutex write_mutex_;

  mutable std::mutex pool_mutex_;
  mutable std::condition_variable pool_cv_;
  std::chrono::milliseconds wait_timeout_;
  struct ReadSlot {
    std::unique_ptr<IConn> conn;
    bool busy = false;
  };
  mutable std::vector<ReadSlot> reads_;
};

// 执行无参数 SQL，失败抛 AppError。
void exec_sql(Conn& conn, std::string_view sql);

std::string now_text();    // 2026-11-12 10:30:00
std::string today_text();  // 2026-11-12

}  // namespace xiaotu
