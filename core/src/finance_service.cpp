#include "xiaotu/finance_service.h"

#include <sqlite3.h>

#include <string>

namespace xiaotu {

std::vector<ArApRow> FinanceService::entries(int direction, std::string_view partner_key,
                                             std::string_view status) const {
  std::vector<ArApRow> rows;
  db_.read([&](sqlite3* conn) {
    Stmt stmt(conn,
              "SELECT id, doc_id, doc_no, doc_type, direction, partner_id, partner_name,"
              " partner_key, biz_date, due_date, amount_cents, settled_cents, status"
              " FROM ar_ap_entry"
              " WHERE (? = 0 OR direction = ?)"
              "   AND (? = '' OR partner_key = ?)"
              "   AND (? = '' OR status = ?)"
              " ORDER BY biz_date DESC, id DESC;");
    stmt.bind(1, static_cast<std::int64_t>(direction))
        .bind(2, static_cast<std::int64_t>(direction))
        // 空 string_view 的 data() 为空，直接绑定会变成 NULL，比较恒为假。
        .bind(3, partner_key.empty() ? std::string_view{""} : partner_key)
        .bind(4, partner_key.empty() ? std::string_view{""} : partner_key)
        .bind(5, status.empty() ? std::string_view{""} : status)
        .bind(6, status.empty() ? std::string_view{""} : status);
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
