#pragma once

#include <sqlite3.h>

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace xiaotu {

// sqlite3_stmt 的 RAII 薄封装：只做绑定、单步与取值，不含业务语义。
class Stmt {
 public:
  Stmt(sqlite3* db, std::string_view sql);
  ~Stmt();

  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;

  Stmt& bind(std::int64_t index, std::int64_t value);
  Stmt& bind(std::int64_t index, std::string_view value);

  bool step();   // 有结果行返回 true
  void run();    // 执行到完成，忽略结果行

  std::int64_t i64(int column) const;
  std::string text(int column) const;

 private:
  sqlite3* db_ = nullptr;
  sqlite3_stmt* stmt_ = nullptr;
};

// 连接管理：
//   - 写：单连接 + 互斥，write() 内含一个 BEGIN IMMEDIATE 事务
//   - 读：小连接池，read() 内含只读事务
// 单写连接是本方案「先查余额再扣减」得以原子的前提。
class Db {
 public:
  explicit Db(std::filesystem::path file, int read_connections = 4);
  ~Db();

  Db(const Db&) = delete;
  Db& operator=(const Db&) = delete;

  // 串行执行并包裹事务；fn 抛异常则回滚。
  void write(const std::function<void(sqlite3*)>& fn);
  // 串行执行但不包裹事务，供迁移脚本自行控制 BEGIN/COMMIT。
  void write_raw(const std::function<void(sqlite3*)>& fn);
  // 从连接池取一个读连接，包裹只读事务。
  void read(const std::function<void(sqlite3*)>& fn) const;

  std::int64_t schema_version() const;
  const std::filesystem::path& file() const { return file_; }

 private:
  std::filesystem::path file_;
  sqlite3* write_ = nullptr;

  std::mutex write_mutex_;

  mutable std::mutex pool_mutex_;
  mutable std::condition_variable pool_cv_;
  mutable std::vector<sqlite3*> reads_;
  mutable std::vector<bool> read_busy_;
};

// 执行无参数 SQL，失败抛 AppError。
void exec_sql(sqlite3* db, std::string_view sql);

std::string now_text();    // 2026-11-12 10:30:00
std::string today_text();  // 2026-11-12

}  // namespace xiaotu
