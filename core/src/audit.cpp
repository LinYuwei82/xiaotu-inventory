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

namespace {

struct BindValue {
  bool is_integer = false;
  std::int64_t integer = 0;
  std::string text;
};

void apply_binds(Stmt& stmt, const std::vector<BindValue>& binds) {
  for (std::size_t index = 0; index < binds.size(); ++index) {
    const auto position = static_cast<std::int64_t>(index + 1);
    if (binds[index].is_integer) {
      stmt.bind(position, binds[index].integer);
    } else {
      stmt.bind(position, binds[index].text);
    }
  }
}

AuditRow read_audit_row(const Stmt& stmt) {
  return AuditRow{.id = stmt.i64(0),
                  .at = stmt.text(1),
                  .actor = stmt.text(2),
                  .action = stmt.text(3),
                  .entity = stmt.text(4),
                  .entity_id = stmt.i64(5),
                  .entity_no = stmt.text(6),
                  .detail = stmt.text(7)};
}

}  // namespace

AuditPage audit_query(Db& db, const AuditFilter& filter) {
  AuditPage result;
  result.page = filter.page < 1 ? 1 : filter.page;
  result.page_size = filter.page_size < 1 ? 50 : (filter.page_size > 200 ? 200 : filter.page_size);

  std::string where = " FROM audit_log WHERE 1 = 1";
  std::vector<BindValue> binds;
  if (!filter.from.empty()) {
    where += " AND at >= ?";
    binds.push_back(BindValue{.text = filter.from + " 00:00:00"});
  }
  if (!filter.to.empty()) {
    where += " AND at <= ?";
    binds.push_back(BindValue{.text = filter.to + " 23:59:59"});
  }
  if (!filter.entity.empty()) {
    where += " AND entity = ?";
    binds.push_back(BindValue{.text = filter.entity});
  }
  if (filter.entity_id > 0) {
    where += " AND entity_id = ?";
    binds.push_back(BindValue{.is_integer = true, .integer = filter.entity_id});
  }
  if (!filter.actor.empty()) {
    where += " AND actor = ?";
    binds.push_back(BindValue{.text = filter.actor});
  }

  db.read([&](sqlite3* conn) {
    Stmt count_stmt(conn, "SELECT COUNT(*)" + where + ";");
    apply_binds(count_stmt, binds);
    if (count_stmt.step()) {
      result.total = count_stmt.i64(0);
    }

    Stmt stmt(conn, "SELECT id, at, actor, action, entity, entity_id, entity_no, detail" + where +
                        " ORDER BY id DESC LIMIT ? OFFSET ?;");
    apply_binds(stmt, binds);
    const auto offset = static_cast<std::int64_t>(result.page - 1) * result.page_size;
    stmt.bind(static_cast<std::int64_t>(binds.size() + 1),
              static_cast<std::int64_t>(result.page_size));
    stmt.bind(static_cast<std::int64_t>(binds.size() + 2), offset);
    while (stmt.step()) {
      result.rows.push_back(read_audit_row(stmt));
    }
  });
  return result;
}

}  // namespace xiaotu
