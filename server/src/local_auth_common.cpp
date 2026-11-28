#include "local_auth_common.h"

#include <string>

#include "xiaotu/audit.h"
#include "xiaotu/db.h"
#include "xiaotu/ids.h"
#include "xiaotu/token_hash.h"

namespace xiaotu {
namespace {

constexpr int kLocalRememberDays = 30;

}  // namespace

std::string read_setting_value(Conn& conn, const char* key) {
  Stmt stmt(conn, "SELECT value FROM app_setting WHERE key = ?;");
  stmt.bind(1, key);
  return stmt.step() ? stmt.text(0) : std::string{};
}

void write_setting_value(Conn& conn, const char* key, const std::string& value) {
  Stmt stmt(conn,
            "INSERT INTO app_setting (key, value, updated_at, updated_by)"
            " VALUES (?, ?, ?, 'system')"
            " ON CONFLICT(key) DO UPDATE SET value = excluded.value,"
            " updated_at = excluded.updated_at;");
  stmt.bind(1, key).bind(2, value).bind(3, now_text()).run();
}

bool local_remember_enabled(Db& db) {
  bool enabled = true;
  db.read([&](Conn& conn) {
    enabled = read_setting_value(conn, kLocalRememberSettingKey) != "0";
  });
  return enabled;
}

bool has_local_remember_token(Db& db) {
  bool exists = false;
  db.read([&](Conn& conn) {
    Stmt stmt(conn, "SELECT EXISTS(SELECT 1 FROM local_remember_token);");
    exists = stmt.step() && stmt.i64(0) != 0;
  });
  return exists;
}

IssuedRememberToken issue_local_remember_token(Db& db, std::int64_t user_id,
                                               const std::string& username) {
  IssuedRememberToken issued;
  issued.token = random_hex(32);
  const std::string digest = hash_token(issued.token);
  db.write([&](Conn& conn) {
    Stmt clear(conn, "DELETE FROM local_remember_token;");
    clear.run();
    Stmt insert(conn,
                "INSERT INTO local_remember_token"
                " (token_hash, issued_at, expires_at, last_used_at)"
                " VALUES (?, datetime('now'), datetime('now', ?), datetime('now'));");
    insert.bind(1, digest)
        .bind(2, std::string("+") + std::to_string(kLocalRememberDays) + " days")
        .run();
    Stmt select(conn, "SELECT expires_at FROM local_remember_token WHERE token_hash = ?;");
    select.bind(1, digest);
    if (select.step()) {
      issued.expires_at = select.text(0);
    }
    audit_write_as(conn, username, "remember_issue", "auth", user_id, username,
                   "签发本机免输凭据");
  });
  return issued;
}

void clear_local_remember_tokens(Db& db, std::int64_t user_id, const std::string& username,
                                 const char* detail) {
  db.write([&](Conn& conn) {
    Stmt clear(conn, "DELETE FROM local_remember_token;");
    clear.run();
    audit_write_as(conn, username, "remember_clear", "auth", user_id, username, detail);
  });
}

// 到期与时钟回拨都在服务端判断：
// - now >= expires_at 视为过期；
// - now < issued_at 或 now < last_used_at 说明系统时间被回拨，一律作废，
//   不顺着回拨延长有效期。
bool validate_local_remember_token(Db& db, const std::string& token, std::string* failure) {
  if (token.empty()) {
    *failure = "缺少本机免输凭据";
    return false;
  }
  const std::string digest = hash_token(token);
  bool found = false;
  bool expired = false;
  bool rolled_back = false;
  db.read([&](Conn& conn) {
    Stmt stmt(conn,
              "SELECT expires_at <= datetime('now'),"
              " issued_at > datetime('now'),"
              " last_used_at > datetime('now')"
              " FROM local_remember_token WHERE token_hash = ? LIMIT 1;");
    stmt.bind(1, digest);
    if (stmt.step()) {
      found = true;
      expired = stmt.i64(0) != 0;
      rolled_back = stmt.i64(1) != 0 || stmt.i64(2) != 0;
    }
  });
  if (!found) {
    *failure = "本机免输凭据无效，请重新输入口令";
    return false;
  }
  if (expired || rolled_back) {
    db.write([&](Conn& conn) {
      Stmt stmt(conn, "DELETE FROM local_remember_token WHERE token_hash = ?;");
      stmt.bind(1, digest).run();
    });
    *failure = rolled_back ? "系统时间早于凭据签发时间，本机免输已失效"
                           : "本机免输已过期，请重新输入口令";
    return false;
  }
  db.write([&](Conn& conn) {
    Stmt stmt(conn,
              "UPDATE local_remember_token SET last_used_at = datetime('now')"
              " WHERE token_hash = ?;");
    stmt.bind(1, digest).run();
  });
  return true;
}

}  // namespace xiaotu
