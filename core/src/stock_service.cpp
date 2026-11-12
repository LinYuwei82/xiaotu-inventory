#include "xiaotu/stock_service.h"

#include <sqlite3.h>

#include <utility>

namespace xiaotu {

std::vector<StockBalanceRow> StockService::balances(std::int64_t warehouse_id,
                                                    std::int64_t item_id, bool hide_zero) {
  std::vector<StockBalanceRow> rows;
  db_.read([&](sqlite3* conn) {
    Stmt stmt(conn,
              "SELECT i.id, i.code, i.name, i.unit, w.code, w.name, b.qty, b.amount_cents"
              " FROM stock_balance b"
              " JOIN item i ON i.id = b.item_id"
              " JOIN warehouse w ON w.id = b.warehouse_id"
              " WHERE i.is_deleted = 0 AND w.is_deleted = 0"
              "   AND (? = 0 OR b.warehouse_id = ?)"
              "   AND (? = 0 OR b.item_id = ?)"
              "   AND (? = 0 OR b.qty <> 0)"
              " ORDER BY i.code, w.code;");
    stmt.bind(1, warehouse_id)
        .bind(2, warehouse_id)
        .bind(3, item_id)
        .bind(4, item_id)
        .bind(5, static_cast<std::int64_t>(hide_zero ? 1 : 0));
    while (stmt.step()) {
      StockBalanceRow row;
      row.item_id = stmt.i64(0);
      row.item_code = stmt.text(1);
      row.item_name = stmt.text(2);
      row.unit = stmt.text(3);
      row.warehouse_code = stmt.text(4);
      row.warehouse_name = stmt.text(5);
      row.qty = stmt.i64(6);
      row.amount_cents = stmt.i64(7);
      rows.push_back(std::move(row));
    }
  });
  return rows;
}

std::vector<StockLedgerRow> StockService::ledger(std::int64_t item_id, int limit) {
  std::vector<StockLedgerRow> rows;
  db_.read([&](sqlite3* conn) {
    Stmt stmt(conn,
              "SELECT l.id, l.biz_type, l.biz_time, h.doc_no, i.code, i.name, w.name, l.qty,"
              " l.amount_cents"
              " FROM stock_ledger l"
              " JOIN doc_header h ON h.id = l.doc_id"
              " JOIN item i ON i.id = l.item_id"
              " JOIN warehouse w ON w.id = l.warehouse_id"
              " WHERE (? = 0 OR l.item_id = ?)"
              " ORDER BY l.id DESC LIMIT ?;");
    stmt.bind(1, item_id).bind(2, item_id).bind(3, static_cast<std::int64_t>(limit));
    while (stmt.step()) {
      StockLedgerRow row;
      row.id = stmt.i64(0);
      row.biz_type = stmt.text(1);
      row.biz_time = stmt.text(2);
      row.doc_no = stmt.text(3);
      row.item_code = stmt.text(4);
      row.item_name = stmt.text(5);
      row.warehouse_name = stmt.text(6);
      row.qty = stmt.i64(7);
      row.amount_cents = stmt.i64(8);
      rows.push_back(std::move(row));
    }
  });
  return rows;
}

}  // namespace xiaotu
