#include "xiaotu/audit.h"

#include <sqlite3.h>

#include <string>

#include "xiaotu/context.h"

namespace xiaotu {

void audit_write(sqlite3* conn, std::string_view action, std::string_view entity,
                 std::int64_t entity_id, std::string_view entity_no, std::string_view detail) {
  audit_write_as(conn, current_user(), action, entity, entity_id, entity_no, detail);
}

void audit_write_as(sqlite3* conn, std::string_view actor, std::string_view action,
                    std::string_view entity, std::int64_t entity_id,
                    std::string_view entity_no, std::string_view detail) {
  // 来源 IP 与请求号取自当前请求上下文：CLI / 无请求场景下为空串，不编造。
  const RequestContext& context = current_context();
  Stmt stmt(conn,
            "INSERT INTO audit_log"
            " (at, actor, action, entity, entity_id, entity_no, detail, client_ip, request_id)"
            " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);");
  stmt.bind(1, now_text())
      .bind(2, actor)
      .bind(3, action)
      .bind(4, entity)
      .bind(5, entity_id)
      .bind(6, entity_no)
      // 默认参数是空 string_view（data() 为空），直接绑定会被当成 NULL。
      .bind(7, detail.empty() ? std::string_view{""} : detail)
      .bind(8, context.client_ip)
      .bind(9, context.audit_request_id);
  stmt.run();
}

std::vector<AuditRow> audit_recent(Db& db, int limit) {
  std::vector<AuditRow> rows;
  db.read([&](sqlite3* conn) {
    Stmt stmt(conn,
              "SELECT id, at, actor, action, entity, entity_id, entity_no, detail,"
              " client_ip, request_id FROM audit_log"
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
                              .detail = stmt.text(7),
                              .client_ip = stmt.text(8),
                              .request_id = stmt.text(9)});
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
                  .detail = stmt.text(7),
                  .client_ip = stmt.text(8),
                  .request_id = stmt.text(9)};
}

// 列表、计数与导出共用同一套筛选条件，避免两处漂移。
struct AuditWhere {
  std::string sql;
  std::vector<BindValue> binds;
};

AuditWhere build_where(const AuditFilter& filter) {
  AuditWhere result{" FROM audit_log WHERE 1 = 1", {}};
  if (!filter.from.empty()) {
    result.sql += " AND at >= ?";
    result.binds.push_back(BindValue{.text = filter.from + " 00:00:00"});
  }
  if (!filter.to.empty()) {
    result.sql += " AND at <= ?";
    result.binds.push_back(BindValue{.text = filter.to + " 23:59:59"});
  }
  if (!filter.entity.empty()) {
    result.sql += " AND entity = ?";
    result.binds.push_back(BindValue{.text = filter.entity});
  }
  if (filter.entity_id > 0) {
    result.sql += " AND entity_id = ?";
    result.binds.push_back(BindValue{.is_integer = true, .integer = filter.entity_id});
  }
  if (!filter.actor.empty()) {
    result.sql += " AND actor = ?";
    result.binds.push_back(BindValue{.text = filter.actor});
  }
  if (!filter.client_ip.empty()) {
    result.sql += " AND client_ip = ?";
    result.binds.push_back(BindValue{.text = filter.client_ip});
  }
  if (!filter.request_id.empty()) {
    result.sql += " AND request_id = ?";
    result.binds.push_back(BindValue{.text = filter.request_id});
  }
  return result;
}

}  // namespace

AuditPage audit_query(Db& db, const AuditFilter& filter) {
  AuditPage result;
  result.page = filter.page < 1 ? 1 : filter.page;
  result.page_size = filter.page_size < 1 ? 50 : (filter.page_size > 200 ? 200 : filter.page_size);

  const AuditWhere where = build_where(filter);

  db.read([&](sqlite3* conn) {
    Stmt count_stmt(conn, "SELECT COUNT(*)" + where.sql + ";");
    apply_binds(count_stmt, where.binds);
    if (count_stmt.step()) {
      result.total = count_stmt.i64(0);
    }

    Stmt stmt(conn, "SELECT id, at, actor, action, entity, entity_id, entity_no, detail,"
                    " client_ip, request_id" +
                        where.sql + " ORDER BY id DESC LIMIT ? OFFSET ?;");
    apply_binds(stmt, where.binds);
    const auto offset = static_cast<std::int64_t>(result.page - 1) * result.page_size;
    stmt.bind(static_cast<std::int64_t>(where.binds.size() + 1),
              static_cast<std::int64_t>(result.page_size));
    stmt.bind(static_cast<std::int64_t>(where.binds.size() + 2), offset);
    while (stmt.step()) {
      result.rows.push_back(read_audit_row(stmt));
    }
  });
  return result;
}

std::vector<AuditRow> audit_all(Db& db, const AuditFilter& filter) {
  std::vector<AuditRow> rows;
  const AuditWhere where = build_where(filter);
  db.read([&](sqlite3* conn) {
    Stmt stmt(conn, "SELECT id, at, actor, action, entity, entity_id, entity_no, detail,"
                    " client_ip, request_id" +
                        where.sql + " ORDER BY id DESC;");
    apply_binds(stmt, where.binds);
    while (stmt.step()) {
      rows.push_back(read_audit_row(stmt));
    }
  });
  return rows;
}

}  // namespace xiaotu
