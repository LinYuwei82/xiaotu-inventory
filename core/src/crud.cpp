#include "xiaotu/crud.h"

#include "xiaotu/db.h"

#include "xiaotu/error.h"

namespace xiaotu {

void soft_delete(Conn& conn, std::string_view table, std::int64_t id) {
  const AuditStamp stamp = audit_stamp();
  const std::string sql = "UPDATE " + std::string(table) +
                          " SET is_deleted = 1, updated_at = ?, updated_by = ?"
                          " WHERE id = ? AND is_deleted = 0;";
  Stmt stmt(conn, sql);
  stmt.bind(1, stamp.at).bind(2, stamp.by).bind(3, id);
  stmt.run();
  if (conn.changes() == 0) {
    throw AppError("NOT_FOUND", "记录不存在或已删除");
  }
}

void ensure_unique(Conn& conn, std::string_view table, std::string_view column,
                   std::string_view value, std::int64_t exclude_id, std::string_view label) {
  const std::string sql = "SELECT 1 FROM " + std::string(table) + " WHERE " + std::string(column) +
                          " = ? AND id <> ? AND is_deleted = 0 LIMIT 1;";
  Stmt stmt(conn, sql);
  stmt.bind(1, value).bind(2, exclude_id);
  if (stmt.step()) {
    throw AppError("DUPLICATE_CODE", std::string(label) + "已存在: " + std::string(value));
  }
}

void ensure_exists(Conn& conn, std::string_view table, std::int64_t id, std::string_view label) {
  const std::string sql =
      "SELECT 1 FROM " + std::string(table) + " WHERE id = ? AND is_deleted = 0 LIMIT 1;";
  Stmt stmt(conn, sql);
  stmt.bind(1, id);
  if (!stmt.step()) {
    throw AppError("NOT_FOUND", std::string(label) + "不存在");
  }
}

}  // namespace xiaotu
