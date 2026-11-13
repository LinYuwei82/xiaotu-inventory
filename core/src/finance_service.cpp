#include "xiaotu/finance_service.h"

#include <sqlite3.h>

#include <string>
#include <vector>

#include "xiaotu/partner_resolve.h"

namespace xiaotu {
namespace {

std::string mark_list(std::size_t count) {
  std::string marks;
  for (std::size_t i = 0; i < count; ++i) {
    if (i != 0) marks += ", ";
    marks += "?";
  }
  return marks;
}

}  // namespace

std::vector<ArApRow> FinanceService::entries(int direction, std::string_view partner_key,
                                             std::string_view status) const {
  std::vector<ArApRow> rows;
  db_.read([&](sqlite3* conn) {
    // 按单位筛选时展开历史键：C:/S: 与新 P: 属于同一 partner 维度。
    const std::vector<std::string> aliases =
        partner_key.empty() ? std::vector<std::string>{}
                            : partner_key_aliases(conn, partner_key);
    std::string sql =
        "SELECT id, doc_id, doc_no, doc_type, direction, partner_id, partner_name,"
        " partner_key, biz_date, due_date, amount_cents, settled_cents, status"
        " FROM ar_ap_entry"
        " WHERE (? = 0 OR direction = ?)";
    if (!aliases.empty()) sql += " AND partner_key IN (" + mark_list(aliases.size()) + ")";
    sql +=
        // 未显式指定状态时排除作废台账，与账龄报表口径一致。
        " AND (CASE WHEN ? = '' THEN status <> 'VOIDED' ELSE status = ? END)"
        " ORDER BY biz_date DESC, id DESC;";
    Stmt stmt(conn, sql);
    int index = 1;
    stmt.bind(index++, static_cast<std::int64_t>(direction))
        .bind(index++, static_cast<std::int64_t>(direction));
    for (const auto& alias : aliases) stmt.bind(index++, alias);
    stmt.bind(index++, status.empty() ? std::string_view{""} : status)
        .bind(index++, status.empty() ? std::string_view{""} : status);
    while (stmt.step()) {
      ArApRow row;
      row.id = stmt.i64(0);
      row.doc_id = stmt.i64(1);
      row.doc_no = stmt.text(2);
      row.doc_type = stmt.text(3);
      row.direction = static_cast<int>(stmt.i64(4));
      row.partner_id = stmt.i64(5);
      row.partner_name = stmt.text(6);
      row.partner_key = stmt.text(7);
      row.biz_date = stmt.text(8);
      row.due_date = stmt.text(9);
      row.amount_cents = stmt.i64(10);
      row.settled_cents = stmt.i64(11);
      row.outstanding_cents = row.amount_cents - row.settled_cents;
      row.status = stmt.text(12);
      rows.push_back(std::move(row));
    }
  });
  return rows;
}

}  // namespace xiaotu
