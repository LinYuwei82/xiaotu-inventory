#include "xiaotu/session_store.h"

#include <sqlite3.h>

#include <string>

#include "xiaotu/ids.h"
#include "xiaotu/token_hash.h"

namespace xiaotu {
namespace {

// 与 Cookie 的 Max-Age 保持一致；两处都改才算改动有效期。
constexpr const char* kLifetimeSql = "+8 hours";
// 滑动续期的写库间隔，避免每个请求都产生一次写事务。
constexpr const char* kRenewAfterSql = "-60 seconds";

constexpr const char* kSelect =
    "SELECT s.user_id, u.username, r.code, s.csrf,"
    " s.expires_at <= datetime('now', 'localtime') AS expired,"
    " s.last_seen_at <= datetime('now', 'localtime', ?) AS needs_renew,"
    " u.is_deleted, u.status"
    " FROM app_session s"
    " JOIN app_user u ON u.id = s.user_id"
    " JOIN app_role r ON r.id = u.role_id"
    " WHERE s.token_hash = ?"
    " LIMIT 1;";

}  // namespace

SessionStore::SessionStore(Db& db) : db_(db) { purge_expired(); }

CreatedSession SessionStore::create(std::int64_t user_id) {
  CreatedSession created{.id = random_hex(32), .csrf = random_hex(32)};
  db_.write([&](sqlite3* conn) {
    Stmt stmt(conn,
              "INSERT INTO app_session (token_hash, user_id, csrf, created_at, last_seen_at,"
              " expires_at)"
              " VALUES (?, ?, ?, datetime('now', 'localtime'), datetime('now', 'localtime'),"
              " datetime('now', 'localtime', ?));");
    stmt.bind(1, hash_token(created.id))
        .bind(2, user_id)
        .bind(3, created.csrf)
        .bind(4, kLifetimeSql)
        .run();
  });
  return created;
}

std::optional<Session> SessionStore::get(const std::string& id, bool* renewed) {
  if (renewed != nullptr) {
    *renewed = false;
  }
  if (id.empty()) {
    return std::nullopt;
  }

  const std::string digest = hash_token(id);
  std::optional<Session> session;
  bool expired = false;
  bool needs_renew = false;
  bool usable = false;
  db_.read([&](sqlite3* conn) {
    Stmt stmt(conn, kSelect);
    stmt.bind(1, kRenewAfterSql).bind(2, digest);
    if (stmt.step()) {
      session = Session{.user_id = stmt.i64(0),
                        .user = stmt.text(1),
                        .role = stmt.text(2),
                        .csrf = stmt.text(3)};
      expired = stmt.i64(4) != 0;
      needs_renew = stmt.i64(5) != 0;
      usable = stmt.i64(6) == 0 && stmt.i64(7) != 0;
    }
  });

  if (!session.has_value()) {
    return std::nullopt;
  }
  // 过期、用户被停用或删除：立即拒绝并清掉这条记录。
  if (expired || !usable) {
    erase(id);
    return std::nullopt;
  }

  if (needs_renew) {
    // 条件里再判一次时间：并发请求同时认为「该续期」时只有一次真正落库。
    db_.write([&](sqlite3* conn) {
      Stmt stmt(conn,
                "UPDATE app_session SET last_seen_at = datetime('now', 'localtime'),"
                " expires_at = datetime('now', 'localtime', ?)"
                " WHERE token_hash = ? AND last_seen_at <= datetime('now', 'localtime', ?);");
      stmt.bind(1, kLifetimeSql).bind(2, digest).bind(3, kRenewAfterSql).run();
      if (renewed != nullptr) {
        *renewed = sqlite3_changes(conn) > 0;
      }
    });
  }
  return session;
}

void SessionStore::erase(const std::string& id) {
  if (id.empty()) {
    return;
  }
  db_.write([&](sqlite3* conn) {
    Stmt stmt(conn, "DELETE FROM app_session WHERE token_hash = ?;");
    stmt.bind(1, hash_token(id)).run();
  });
}

void SessionStore::purge_expired() {
  db_.write([](sqlite3* conn) {
    Stmt stmt(conn,
              "DELETE FROM app_session WHERE expires_at <= datetime('now', 'localtime');");
    stmt.run();
  });
}

}  // namespace xiaotu
