#include "xiaotu/session_store.h"

#include "xiaotu/db.h"

#include <string>

#include "xiaotu/ids.h"
#include "xiaotu/token_hash.h"

namespace xiaotu {
namespace {

// 滑动续期的写库间隔，避免每个请求都产生一次写事务。
constexpr const char* kRenewAfterSql = "-60 seconds";

std::string minutes_modifier(int minutes) { return "+" + std::to_string(minutes) + " minutes"; }

std::string hours_modifier(int hours) { return "-" + std::to_string(hours) + " hours"; }

constexpr const char* kSelect =
    "SELECT s.user_id, u.username, r.code, s.csrf,"
    " s.expires_at <= datetime('now', 'localtime') AS expired,"
    " s.created_at <= datetime('now', 'localtime', ?) AS too_old,"
    " s.last_seen_at <= datetime('now', 'localtime', ?) AS needs_renew,"
    " u.is_deleted, u.status, u.must_change_password"
    " FROM app_session s"
    " JOIN app_user u ON u.id = s.user_id"
    " JOIN app_role r ON r.id = u.role_id"
    " WHERE s.token_hash = ?"
    " LIMIT 1;";

}  // namespace

SessionStore::SessionStore(Db& db) : db_(db) { purge_expired(); }

CreatedSession SessionStore::create(std::int64_t user_id, int idle_minutes) {
  CreatedSession created{.id = random_hex(32), .csrf = random_hex(32)};
  db_.write([&](Conn& conn) {
    Stmt stmt(conn,
              "INSERT INTO app_session (token_hash, user_id, csrf, created_at, last_seen_at,"
              " expires_at)"
              " VALUES (?, ?, ?, datetime('now', 'localtime'), datetime('now', 'localtime'),"
              " datetime('now', 'localtime', ?));");
    stmt.bind(1, hash_token(created.id))
        .bind(2, user_id)
        .bind(3, created.csrf)
        .bind(4, minutes_modifier(idle_minutes))
        .run();
  });
  return created;
}

std::optional<Session> SessionStore::get(const std::string& id, int idle_minutes,
                                         int absolute_hours, bool* renewed) {
  if (renewed != nullptr) {
    *renewed = false;
  }
  if (id.empty()) {
    return std::nullopt;
  }

  const std::string digest = hash_token(id);
  std::optional<Session> session;
  bool expired = false;
  bool too_old = false;
  bool needs_renew = false;
  bool usable = false;
  db_.read([&](Conn& conn) {
    Stmt stmt(conn, kSelect);
    stmt.bind(1, hours_modifier(absolute_hours))
        .bind(2, kRenewAfterSql)
        .bind(3, digest);
    if (stmt.step()) {
      session = Session{.user_id = stmt.i64(0),
                        .user = stmt.text(1),
                        .role = stmt.text(2),
                        .csrf = stmt.text(3),
                        .must_change_password = stmt.i64(9) != 0};
      expired = stmt.i64(4) != 0;
      too_old = stmt.i64(5) != 0;
      needs_renew = stmt.i64(6) != 0;
      usable = stmt.i64(7) == 0 && stmt.i64(8) != 0;
    }
  });

  if (!session.has_value()) {
    return std::nullopt;
  }
  // 空闲过期、超过签发后的绝对上限、用户被停用或删除：立即拒绝并清掉这条记录。
  if (expired || too_old || !usable) {
    erase(id);
    return std::nullopt;
  }

  if (needs_renew) {
    // 条件里再判一次时间：并发请求同时认为「该续期」时只有一次真正落库。
    db_.write([&](Conn& conn) {
      Stmt stmt(conn,
                "UPDATE app_session SET last_seen_at = datetime('now', 'localtime'),"
                " expires_at = datetime('now', 'localtime', ?)"
                " WHERE token_hash = ? AND last_seen_at <= datetime('now', 'localtime', ?);");
      stmt.bind(1, minutes_modifier(idle_minutes)).bind(2, digest).bind(3, kRenewAfterSql).run();
      if (renewed != nullptr) {
        *renewed = conn.changes() > 0;
      }
    });
  }
  return session;
}

void SessionStore::erase(const std::string& id) {
  if (id.empty()) {
    return;
  }
  db_.write([&](Conn& conn) {
    Stmt stmt(conn, "DELETE FROM app_session WHERE token_hash = ?;");
    stmt.bind(1, hash_token(id)).run();
  });
}

void SessionStore::purge_expired() {
  db_.write([](Conn& conn) {
    Stmt stmt(conn,
              "DELETE FROM app_session WHERE expires_at <= datetime('now', 'localtime');");
    stmt.run();
  });
}

}  // namespace xiaotu
