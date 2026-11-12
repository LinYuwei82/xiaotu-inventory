// 会话持久化的冒烟测试：重启后仍有效、登出/改密/停用/删除即失效、过期拒绝。
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include <filesystem>
#include <iostream>
#include <string>

#include <sqlite3.h>

#include "xiaotu/db.h"
#include "xiaotu/migrations.h"
#include "xiaotu/permissions.h"
#include "xiaotu/session_store.h"
#include "xiaotu/token_hash.h"
#include "xiaotu/user_service.h"

namespace {

int g_failures = 0;

void check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << std::endl;
    ++g_failures;
  }
}

// 直接按令牌散列写一条会话，用来构造过期、旧时间等状态。
void insert_session(xiaotu::Db& db, const std::string& token, std::int64_t user_id,
                    const std::string& expires_modifier) {
  db.write([&](sqlite3* conn) {
    xiaotu::Stmt stmt(
        conn,
        "INSERT INTO app_session (token_hash, user_id, csrf, created_at, last_seen_at,"
        " expires_at)"
        " VALUES (?, ?, 'csrf-test', datetime('now', 'localtime'), datetime('now', 'localtime'),"
        " datetime('now', 'localtime', ?));");
    stmt.bind(1, xiaotu::hash_token(token)).bind(2, user_id).bind(3, expires_modifier).run();
  });
}

std::string column_text(xiaotu::Db& db, const std::string& sql, const std::string& param) {
  std::string value;
  db.read([&](sqlite3* conn) {
    xiaotu::Stmt stmt(conn, sql);
    stmt.bind(1, param);
    if (stmt.step()) {
      value = stmt.text(0);
    }
  });
  return value;
}

std::int64_t scalar_count(xiaotu::Db& db, const std::string& sql) {
  std::int64_t value = 0;
  db.read([&](sqlite3* conn) {
    xiaotu::Stmt stmt(conn, sql);
    if (stmt.step()) {
      value = stmt.i64(0);
    }
  });
  return value;
}

std::string read_text(xiaotu::Db& db, const std::string& sql) {
  std::string value;
  db.read([&](sqlite3* conn) {
    xiaotu::Stmt stmt(conn, sql);
    if (stmt.step()) {
      value = stmt.text(0);
    }
  });
  return value;
}

}  // namespace

int main() {
  namespace fs = std::filesystem;
#if defined(_WIN32)
  const long process_id = _getpid();
#else
  const long process_id = ::getpid();
#endif
  const fs::path dir =
      fs::temp_directory_path() / ("xiaotu-session-" + std::to_string(process_id));

  try {
    fs::remove_all(dir);
    const fs::path database = dir / "app.db";

    std::int64_t operator_id = 0;
    std::string token;

    // ---- 建库、登录、模拟重启 ----
    {
      xiaotu::Db db(database, 2);
      xiaotu::migrate(db);
      xiaotu::load_role_permissions(db);
      xiaotu::UserService users(db);
      const auto operator_user = users.create(xiaotu::UserInput{.username = "op1",
                                                                .display_name = "操作员一",
                                                                .role = "operator",
                                                                .password = "Passw0rd!"});
      operator_id = operator_user.id;

      xiaotu::SessionStore sessions(db);
      token = sessions.create(operator_id).id;
      const auto session = sessions.get(token);
      check(session.has_value(), "新建会话应立即可用");
      check(session && session->user == "op1", "会话应带出用户名");
      check(session && session->role == "operator", "会话应带出角色");

      const std::string stored =
          column_text(db, "SELECT token_hash FROM app_session WHERE token_hash = ?;",
                      xiaotu::hash_token(token));
      check(stored == xiaotu::hash_token(token), "表里应存令牌散列");
      check(stored != token, "表里不应出现明文令牌");
      check(scalar_count(db, "SELECT COUNT(*) FROM app_session WHERE token_hash = '" + token + "';") ==
                0,
            "明文令牌不应能匹配到记录");
    }

    {
      // 重启：换一个新的连接与会话对象，旧令牌仍然可用。
      xiaotu::Db db(database, 2);
      xiaotu::SessionStore sessions(db);
      check(sessions.get(token).has_value(), "重启后旧令牌应仍然有效");

      // 登出后立即失效。
      sessions.erase(token);
      check(!sessions.get(token).has_value(), "登出后会话应失效");
    }

    {
      xiaotu::Db db(database, 2);
      xiaotu::UserService users(db);
      xiaotu::SessionStore sessions(db);

      // 过期后拒绝，并顺手清掉过期记录。
      const std::string expired_token = "expired-token-0001";
      insert_session(db, expired_token, operator_id, "-1 hours");
      check(!sessions.get(expired_token).has_value(), "过期会话应被拒绝");
      check(scalar_count(db, "SELECT COUNT(*) FROM app_session;") == 0,
            "过期的会话记录应被清除");

      // 启动时清理：写入过期记录后重建会话对象。
      insert_session(db, "expired-token-0002", operator_id, "-2 hours");
      {
        xiaotu::SessionStore restarted(db);
        check(scalar_count(db, "SELECT COUNT(*) FROM app_session;") == 0,
              "启动时应清理过期的会话");
      }

      // 改密后旧会话失效。
      const std::string password_token = sessions.create(operator_id).id;
      users.set_password(operator_id, "NewPassw0rd!");
      check(!sessions.get(password_token).has_value(), "改密后旧会话应失效");

      // 停用后旧会话失效。
      const std::string disabled_token = sessions.create(operator_id).id;
      users.update(operator_id, xiaotu::UserInput{.username = "op1",
                                                  .display_name = "操作员一",
                                                  .role = "operator",
                                                  .active = false});
      check(!sessions.get(disabled_token).has_value(), "停用用户后旧会话应失效");

      // 删除后旧会话失效。
      users.update(operator_id, xiaotu::UserInput{.username = "op1",
                                                  .display_name = "操作员一",
                                                  .role = "operator",
                                                  .active = true});
      const std::string removed_token = sessions.create(operator_id).id;
      users.remove(operator_id);
      check(!sessions.get(removed_token).has_value(), "删除用户后旧会话应失效");

      // 访问滑动续期：把最近访问时间推回 2 分钟前，再取一次应延长有效期。
      const auto admin = users.find_by_username("admin");
      const std::string sliding_token = sessions.create(admin.id).id;
      db.write([&](sqlite3* conn) {
        xiaotu::Stmt stmt(conn,
                          "UPDATE app_session SET last_seen_at = datetime('now', 'localtime',"
                          " '-2 minutes'), expires_at = datetime('now', 'localtime', '+1 hour')"
                          " WHERE token_hash = ?;");
        stmt.bind(1, xiaotu::hash_token(sliding_token)).run();
      });
      bool renewed = false;
      check(sessions.get(sliding_token, &renewed).has_value(), "续期前会话应有效");
      check(renewed, "超过续期间隔的访问应触发续期");
      const std::string expires =
          column_text(db, "SELECT expires_at FROM app_session WHERE token_hash = ?;",
                      xiaotu::hash_token(sliding_token));
      const std::string eight_hours =
          read_text(db, "SELECT datetime('now', 'localtime', '+7 hours');");
      check(expires > eight_hours, "续期后有效期应接近 8 小时");
    }

    fs::remove_all(dir);
  } catch (const std::exception& error) {
    std::cerr << "异常: " << error.what() << std::endl;
    ++g_failures;
  }

  if (g_failures != 0) {
    std::cerr << g_failures << " 项失败" << std::endl;
    return 1;
  }
  std::cout << "session_smoke 通过" << std::endl;
  return 0;
}
