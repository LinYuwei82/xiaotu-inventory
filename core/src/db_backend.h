#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace xiaotu {

// 后端语句接口：SQLite 以 sqlite3_stmt 实现，未来 PostgreSQL 以 PGresult/PQexecParams 实现。
class IStmt {
 public:
  virtual ~IStmt() = default;
  virtual void bind_i64(int index, std::int64_t value) = 0;
  virtual void bind_text(int index, std::string_view value) = 0;
  // 单步：有结果行返回 true，结果耗尽返回 false。
  virtual bool step() = 0;
  virtual std::int64_t i64(int column) const = 0;
  virtual std::string text(int column) const = 0;
};

// 后端连接接口：prepare/exec/行级元信息 + 事务原语。
class IConn {
 public:
  virtual ~IConn() = default;
  virtual std::unique_ptr<IStmt> prepare(std::string_view sql) = 0;
  virtual void exec(std::string_view sql) = 0;
  virtual int changes() const = 0;
  virtual std::int64_t last_insert_id() const = 0;
  // 事务原语：SQLite 的 begin_write 是 BEGIN IMMEDIATE；
  // PostgreSQL 用普通 BEGIN + 行锁等价语义。
  virtual void begin_write() = 0;
  virtual void begin_read() = 0;
  virtual void commit() = 0;
  virtual void rollback() noexcept = 0;
  // 已应用的 schema 版本（SQLite：PRAGMA user_version；PG：schema_migration 查询）。
  virtual std::int64_t schema_version() const = 0;
};

// 后端工厂：负责建连与连接级初始化（PRAGMA / 连接参数）。
class IDbBackend {
 public:
  virtual ~IDbBackend() = default;
  virtual IConn& write_conn() = 0;
  virtual std::vector<std::unique_ptr<IConn>> create_read_conns(int count) = 0;
};

}  // namespace xiaotu
