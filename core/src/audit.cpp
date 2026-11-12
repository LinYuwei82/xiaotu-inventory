#include "xiaotu/audit.h"

#include <sqlite3.h>

#include <string>

#include "xiaotu/context.h"

namespace xiaotu {

void audit_write(sqlite3* conn, std::string_view action, std::string_view entity,
                 std::int64_t entity_id, std::string_view entity_no, std::string_view detail) {
  Stmt stmt(conn,
            "INSERT INTO audit_log (at, actor, action, entity, entity_id, entity_no, detail)"
            " VALUES (?, ?, ?, ?, ?, ?, ?);");
  stmt.bind(1, now_text())
      .bind(2, current_user())
      .bind(3, action)
      .bind(4, entity)
      .bind(5, entity_id)
      .bind(6, entity_no)
      // 默认参数是空 string_view（data() 为空），直接绑定会被当成 NULL。
      .bind(7, detail.empty() ? std::string_view{""} : detail);
  stmt.run();
}

std::vector<AuditRow> audit_recent(Db& db, int limit) {
  std::vector<AuditRow> rows;
  db.read([&](sqlite3* conn) {
    Stmt stmt(conn,
              "SELECT id, at, actor, action, entity, entity_id, entity_no, detail FROM audit_log"
              " ORDER BY id DESC LIMIT ?;");
    stmt.bind(1, static_cast<std::int64_t>(limit));
    while (stmt.step()) {
      rows.push_back(AuditRow{.id = stmt.i64(0),
                              .at = stmt.text(1),
                              .actor = stmt.text(2),
                              .action = stmt.text(3),
                              .entity = stmt.text(4),
                              .entity_id = stmt.i64(5),
                              .entity_no = stmt.text(6),
                              .detail = stmt.text(7)});
    }
  });
  return rows;
}

}  // namespace xiaotu
