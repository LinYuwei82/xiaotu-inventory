#include "xiaotu/user_service.h"

#include <sqlite3.h>

#include <optional>
#include <string>
#include <utility>

#include "xiaotu/audit.h"
#include "xiaotu/crud.h"
#include "xiaotu/error.h"
#include "xiaotu/password.h"
#include "xiaotu/permissions.h"

namespace xiaotu {
namespace {

// 内置管理员由迁移写入，是单机版唯一的兜底入口，不允许删除、停用或改角色。
constexpr std::int64_t kBuiltinAdminId = 1;

// 连续失败达到阈值即锁定；锁定期内不再校验口令。
constexpr std::int64_t kMaxFailures = 5;
constexpr int kLockMinutes = 15;

constexpr const char* kSelect =
    "SELECT u.id, u.username, u.display_name, r.code, u.status, u.must_change_password,"
    " u.created_at, u.updated_at"
    " FROM app_user u JOIN app_role r ON r.id = u.role_id";

UserRow read_row(const Stmt& stmt) {
  return UserRow{.id = stmt.i64(0),
                 .username = stmt.text(1),
                 .display_name = stmt.text(2),
                 .role = stmt.text(3),
                 .active = stmt.i64(4) != 0,
                 .must_change_password = stmt.i64(5) != 0,
                 .created_at = stmt.text(6),
                 .updated_at = stmt.text(7)};
}

void validate(const UserInput& input) {
  if (input.username.empty()) {
    throw AppError("INVALID_INPUT", "用户名不能为空");
  }
  if (!role_exists(input.role)) {
    throw AppError("INVALID_ROLE", "角色不存在: " + input.role);
  }
}

void validate_password(std::string_view password) {
  if (!password_acceptable(password)) {
    throw AppError("INVALID_PASSWORD", "口令长度应为 8–128 个字符");
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

// 改密、停用、删除都让该用户已登录的会话立即失效。直接删会话表而不是
// 依赖查询时的状态判断，是为了与用户变更落在同一个事务里，不留时间窗。
void revoke_sessions(sqlite3* conn, std::int64_t user_id) {
  Stmt stmt(conn, "DELETE FROM app_session WHERE user_id = ?;");
  stmt.bind(1, user_id).run();
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
  validate_password(input.password);
  const std::string hash = hash_password(input.password);
  std::int64_t id = 0;
  db_.write([&](sqlite3* conn) {
    ensure_username_free(conn, input.username, 0);
    const std::int64_t role_id = role_id_of(conn, input.role);
    const AuditStamp stamp = audit_stamp();
    Stmt stmt(conn,
              "INSERT INTO app_user (username, display_name, role_id, password_hash, status,"
              " is_deleted, created_at, created_by, updated_at, updated_by)"
              " VALUES (?, ?, ?, ?, ?, 0, ?, ?, ?, ?);");
    stmt.bind(1, input.username)
        .bind(2, input.display_name)
        .bind(3, role_id)
        .bind(4, hash)
        .bind(5, static_cast<std::int64_t>(input.active ? 1 : 0))
        .bind(6, stamp.at)
        .bind(7, stamp.by)
        .bind(8, stamp.at)
        .bind(9, stamp.by)
        .run();
    id = sqlite3_last_insert_rowid(conn);
    audit_write(conn, "create", "user", id, input.username,
                "{\"role\":\"" + input.role + "\"}");
  });
  return get(id);
}

UserRow UserService::update(std::int64_t id, const UserInput& input) {
  validate(input);
  if (!input.password.empty()) {
    validate_password(input.password);
  }
  const std::string new_hash =
      input.password.empty() ? std::string{} : hash_password(input.password);
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
    std::string previous_role;
    {
      Stmt stmt(conn,
                "SELECT r.code FROM app_user u JOIN app_role r ON r.id = u.role_id"
                " WHERE u.id = ? AND u.is_deleted = 0 LIMIT 1;");
      stmt.bind(1, id);
      if (stmt.step()) {
        previous_role = stmt.text(0);
      }
    }
    const AuditStamp stamp = audit_stamp();
    if (new_hash.empty()) {
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
    } else {
      Stmt stmt(conn,
                "UPDATE app_user SET username = ?, display_name = ?, role_id = ?, status = ?,"
                " password_hash = ?, must_change_password = 1, failed_count = 0, locked_until = '',"
                " updated_at = ?, updated_by = ?"
                " WHERE id = ? AND is_deleted = 0;");
      stmt.bind(1, input.username)
          .bind(2, input.display_name)
          .bind(3, role_id)
          .bind(4, static_cast<std::int64_t>(input.active ? 1 : 0))
          .bind(5, new_hash)
          .bind(6, stamp.at)
          .bind(7, stamp.by)
          .bind(8, id)
          .run();
    }
    // 口令变更或停用后，旧会话不能再继续使用。
    if (!new_hash.empty() || !input.active) {
      revoke_sessions(conn, id);
    }
    audit_write(conn, "update", "user", id, input.username,
                "{\"role\":\"" + input.role + "\",\"active\":" +
                    std::string(input.active ? "true" : "false") + "}");
    if (!previous_role.empty() && previous_role != input.role) {
      audit_write(conn, "role_change", "user", id, input.username,
                  "{\"from\":\"" + previous_role + "\",\"to\":\"" + input.role + "\"}");
    }
  });
  return get(id);
}

UserRow UserService::authenticate(std::string_view username, std::string_view password,
                                  std::string_view source) {
  const std::string actor =
      username.empty() ? std::string("（空用户名）") : std::string(username.substr(0, 64));
  const std::string source_note =
      source.empty() ? std::string{} : "（来源 " + std::string(source) + "）";
  UserRow row;
  // 事务内不能靠抛异常中断：失败计数与审计必须在返回错误前提交，否则回滚会抹掉留痕。
  enum class Outcome { ok, invalid, locked };
  Outcome outcome = Outcome::ok;
  std::int64_t locked_minutes = 0;

  db_.write([&](sqlite3* conn) {
    std::string hash;
    std::int64_t failed = 0;
    bool found = false;
    Stmt stmt(conn,
              "SELECT u.id, u.username, u.display_name, r.code, u.status, u.must_change_password,"
              " u.created_at, u.updated_at, u.password_hash, u.failed_count"
              " FROM app_user u JOIN app_role r ON r.id = u.role_id"
              " WHERE u.username = ? AND u.is_deleted = 0 LIMIT 1;");
    stmt.bind(1, username);
    if (stmt.step()) {
      row = read_row(stmt);
      hash = stmt.text(8);
      failed = stmt.i64(9);
      found = true;
    }
    if (!found || !row.active) {
      audit_write_as(conn, actor, "login_failed", "auth", 0, actor, "口令错误" + source_note);
      outcome = Outcome::invalid;
      return;
    }
    // 锁定优先于口令校验：锁定期内即使口令正确也不放行。
    Stmt lock(conn,
              "SELECT CAST((julianday(locked_until) - julianday('now', 'localtime')) * 1440 + 0.999"
              " AS INTEGER) FROM app_user WHERE id = ?;");
    lock.bind(1, row.id);
    if (lock.step()) {
      locked_minutes = lock.i64(0);
    }
    if (locked_minutes > 0) {
      audit_write_as(conn, actor, "login_failed", "auth", row.id, row.username,
                     "账号锁定，剩余 " + std::to_string(locked_minutes) + " 分钟");
      outcome = Outcome::locked;
      return;
    }
    if (verify_password(hash, password)) {
      Stmt reset(conn, "UPDATE app_user SET failed_count = 0, locked_until = '' WHERE id = ?;");
      reset.bind(1, row.id).run();
      audit_write_as(conn, row.username, "login", "auth", row.id, row.username, source);
      outcome = Outcome::ok;
      return;
    }
    const std::int64_t next = failed + 1;
    audit_write_as(conn, actor, "login_failed", "auth", row.id, row.username,
                   "口令错误" + source_note);
    if (next >= kMaxFailures) {
      Stmt bump(conn,
                "UPDATE app_user SET failed_count = ?, locked_until ="
                " datetime('now', 'localtime', '+" +
                    std::to_string(kLockMinutes) + " minutes') WHERE id = ?;");
      bump.bind(1, next).bind(2, row.id).run();
      audit_write_as(conn, actor, "account_locked", "auth", row.id, row.username,
                     "口令错误连续 " + std::to_string(kMaxFailures) + " 次，锁定 " +
                         std::to_string(kLockMinutes) + " 分钟");
      locked_minutes = kLockMinutes;
      outcome = Outcome::locked;
      return;
    }
    Stmt bump(conn, "UPDATE app_user SET failed_count = ? WHERE id = ?;");
    bump.bind(1, next).bind(2, row.id).run();
    outcome = Outcome::invalid;
  });

  if (outcome == Outcome::locked) {
    throw AppError("ACCOUNT_LOCKED",
                   "账号已锁定，请 " + std::to_string(locked_minutes) + " 分钟后再试");
  }
  if (outcome == Outcome::invalid) {
    throw AppError("INVALID_CREDENTIALS", "用户名或口令错误");
  }
  return row;
}

bool UserService::has_password(std::int64_t id) const {
  bool found = false;
  db_.read([&](sqlite3* conn) {
    Stmt stmt(conn,
              "SELECT password_hash FROM app_user WHERE id = ? AND is_deleted = 0 LIMIT 1;");
    stmt.bind(1, id);
    if (stmt.step()) {
      found = !stmt.text(0).empty();
    }
  });
  return found;
}

void UserService::set_password(std::int64_t id, std::string_view password,
                               bool force_change_on_first_set) {
  validate_password(password);
  const std::string hash = hash_password(password);
  // 只有首次设置（原散列为空）才要求下次登录强制改密；管理员重设已有口令
  // 不再触发改密页，避免每次跑 --set-password 后登录都弹一次。
  const std::string must_change =
      force_change_on_first_set ? "CASE WHEN password_hash = '' THEN 1 ELSE 0 END" : "0";
  db_.write([&](sqlite3* conn) {
    ensure_user_exists(conn, id);
    Stmt stmt(conn,
              "UPDATE app_user SET password_hash = ?,"
              " must_change_password = " + must_change + ","
              " failed_count = 0, locked_until = '', updated_at = ?, updated_by = ?"
              " WHERE id = ? AND is_deleted = 0;");
    const AuditStamp stamp = audit_stamp();
    stmt.bind(1, hash).bind(2, stamp.at).bind(3, stamp.by).bind(4, id).run();
    revoke_sessions(conn, id);
    audit_write(conn, "set_password", "user", id, std::to_string(id), "管理员重置口令");
  });
}

void UserService::change_password(std::int64_t id, std::string_view old_password,
                                  std::string_view new_password) {
  validate_password(new_password);
  const std::string hash = hash_password(new_password);
  db_.write([&](sqlite3* conn) {
    ensure_user_exists(conn, id);
    std::string username;
    std::string stored;
    {
      Stmt stmt(conn,
                "SELECT username, password_hash FROM app_user WHERE id = ? AND is_deleted = 0"
                " LIMIT 1;");
      stmt.bind(1, id);
      if (stmt.step()) {
        username = stmt.text(0);
        stored = stmt.text(1);
      }
    }
    if (!verify_password(stored, old_password)) {
      throw AppError("INVALID_CREDENTIALS", "原口令不正确");
    }
    Stmt stmt(conn,
              "UPDATE app_user SET password_hash = ?, must_change_password = 0, failed_count = 0,"
              " locked_until = '', updated_at = ?, updated_by = ?"
              " WHERE id = ? AND is_deleted = 0;");
    const AuditStamp stamp = audit_stamp();
    stmt.bind(1, hash).bind(2, stamp.at).bind(3, stamp.by).bind(4, id).run();
    revoke_sessions(conn, id);
    audit_write(conn, "change_password", "user", id, username, "口令已修改");
  });
}

void UserService::remove(std::int64_t id) {
  if (id == kBuiltinAdminId) {
    throw AppError("USER_PROTECTED", "内置管理员不能删除");
  }
  db_.write([&](sqlite3* conn) {
    std::string username;
    {
      Stmt stmt(conn, "SELECT username FROM app_user WHERE id = ?;");
      stmt.bind(1, id);
      if (stmt.step()) {
        username = stmt.text(0);
      }
    }
    soft_delete(conn, "app_user", id);
    revoke_sessions(conn, id);
    audit_write(conn, "delete", "user", id, username);
  });
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
