#include "xiaotu/user_service.h"

#include <sqlite3.h>

#include <optional>
#include <string>
#include <utility>

#include "xiaotu/crud.h"
#include "xiaotu/error.h"
#include "xiaotu/permissions.h"

namespace xiaotu {
namespace {

// 内置管理员由迁移写入，是单机版唯一的兜底入口，不允许删除、停用或改角色。
constexpr std::int64_t kBuiltinAdminId = 1;

constexpr const char* kSelect =
    "SELECT u.id, u.username, u.display_name, r.code, u.status, u.created_at"
    " FROM app_user u JOIN app_role r ON r.id = u.role_id";

UserRow read_row(const Stmt& stmt) {
  return UserRow{.id = stmt.i64(0),
                 .username = stmt.text(1),
                 .display_name = stmt.text(2),
                 .role = stmt.text(3),
                 .active = stmt.i64(4) != 0,
                 .created_at = stmt.text(5)};
}

void validate(const UserInput& input) {
  if (input.username.empty()) {
    throw AppError("INVALID_INPUT", "用户名不能为空");
  }
  if (!role_exists(input.role)) {
    throw AppError("INVALID_ROLE", "角色不存在: " + input.role);
  }
}

std::int64_t role_id_of(sqlite3* conn, std::string_view role) {
  Stmt stmt(conn, "SELECT id FROM app_role WHERE code = ?;");
  stmt.bind(1, role);
  if (!stmt.step()) {
    throw AppError("INVALID_ROLE", "角色不存在: " + std::string(role));
  }
  return stmt.i64(0);
}

// 用户名唯一性覆盖软删除行：唯一索引不区分 is_deleted，提前拦下给出可读错误。
void ensure_username_free(sqlite3* conn, std::string_view username, std::int64_t exclude_id) {
  Stmt stmt(conn, "SELECT 1 FROM app_user WHERE username = ? AND id <> ? LIMIT 1;");
  stmt.bind(1, username).bind(2, exclude_id);
  if (stmt.step()) {
    throw AppError("DUPLICATE_CODE", "用户名已存在: " + std::string(username));
  }
}

void ensure_user_exists(sqlite3* conn, std::int64_t id) {
  Stmt stmt(conn, "SELECT 1 FROM app_user WHERE id = ? AND is_deleted = 0 LIMIT 1;");
  stmt.bind(1, id);
  if (!stmt.step()) {
    throw AppError("NOT_FOUND", "用户不存在或已删除");
  }
}

}  // namespace

std::vector<UserRow> UserService::list() const {
  std::vector<UserRow> rows;
  db_.read([&](sqlite3* conn) {
    Stmt stmt(conn, std::string(kSelect) + " WHERE u.is_deleted = 0 ORDER BY u.id;");
    while (stmt.step()) {
      rows.push_back(read_row(stmt));
    }
  });
  return rows;
}

UserRow UserService::get(std::int64_t id) const {
  std::optional<UserRow> row;
  db_.read([&](sqlite3* conn) {
    Stmt stmt(conn, std::string(kSelect) + " WHERE u.id = ? AND u.is_deleted = 0 LIMIT 1;");
    stmt.bind(1, id);
    if (stmt.step()) {
      row = read_row(stmt);
    }
  });
  if (!row.has_value()) {
    throw AppError("NOT_FOUND", "用户不存在或已删除");
  }
  return *row;
}

UserRow UserService::find_by_username(std::string_view username) const {
  std::optional<UserRow> row;
  db_.read([&](sqlite3* conn) {
    Stmt stmt(conn, std::string(kSelect) + " WHERE u.username = ? AND u.is_deleted = 0 LIMIT 1;");
    stmt.bind(1, username);
    if (stmt.step()) {
      row = read_row(stmt);
    }
  });
  if (!row.has_value()) {
    throw AppError("NOT_FOUND", "用户不存在: " + std::string(username));
  }
  return *row;
}

UserRow UserService::create(const UserInput& input) {
  validate(input);
  std::int64_t id = 0;
  db_.write([&](sqlite3* conn) {
    ensure_username_free(conn, input.username, 0);
    const std::int64_t role_id = role_id_of(conn, input.role);
    const AuditStamp stamp = audit_stamp();
    Stmt stmt(conn,
              "INSERT INTO app_user (username, display_name, role_id, password_hash, status,"
              " is_deleted, created_at, created_by, updated_at, updated_by)"
              " VALUES (?, ?, ?, '', ?, 0, ?, ?, ?, ?);");
    stmt.bind(1, input.username)
        .bind(2, input.display_name)
        .bind(3, role_id)
        .bind(4, static_cast<std::int64_t>(input.active ? 1 : 0))
        .bind(5, stamp.at)
        .bind(6, stamp.by)
        .bind(7, stamp.at)
        .bind(8, stamp.by)
        .run();
    id = sqlite3_last_insert_rowid(conn);
  });
  return get(id);
}

UserRow UserService::update(std::int64_t id, const UserInput& input) {
  validate(input);
  db_.write([&](sqlite3* conn) {
    ensure_user_exists(conn, id);
    if (id == kBuiltinAdminId) {
      if (input.role != "admin") {
        throw AppError("USER_PROTECTED", "内置管理员不能改角色");
      }
      if (!input.active) {
        throw AppError("USER_PROTECTED", "内置管理员不能停用");
      }
    }
    ensure_username_free(conn, input.username, id);
    const std::int64_t role_id = role_id_of(conn, input.role);
    const AuditStamp stamp = audit_stamp();
    Stmt stmt(conn,
              "UPDATE app_user SET username = ?, display_name = ?, role_id = ?, status = ?,"
              " updated_at = ?, updated_by = ? WHERE id = ? AND is_deleted = 0;");
    stmt.bind(1, input.username)
        .bind(2, input.display_name)
        .bind(3, role_id)
        .bind(4, static_cast<std::int64_t>(input.active ? 1 : 0))
        .bind(5, stamp.at)
        .bind(6, stamp.by)
        .bind(7, id)
        .run();
  });
  return get(id);
}

void UserService::remove(std::int64_t id) {
  if (id == kBuiltinAdminId) {
    throw AppError("USER_PROTECTED", "内置管理员不能删除");
  }
  db_.write([&](sqlite3* conn) { soft_delete(conn, "app_user", id); });
}

std::int64_t UserService::count_active() const {
  std::int64_t count = 0;
  db_.read([&](sqlite3* conn) {
    Stmt stmt(conn,
              "SELECT COUNT(*) FROM app_user WHERE is_deleted = 0 AND status = 1;");
    if (stmt.step()) {
      count = stmt.i64(0);
    }
  });
  return count;
}

}  // namespace xiaotu
