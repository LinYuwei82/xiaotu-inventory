#include "xiaotu/report_service.h"

#include <sqlite3.h>

#include <utility>

#include "xiaotu/db.h"

namespace xiaotu {

std::vector<LedgerDetailRow> ReportService::stock_ledger(const ReportFilter& filter) const {
  std::vector<LedgerDetailRow> rows;
  db_.read([&](sqlite3* conn) {
    Stmt stmt(conn,
              "SELECT l.id, l.biz_time, l.biz_type, h.doc_no, i.code, i.name, w.name,"
              " l.qty, l.amount_cents"
              " FROM stock_ledger l"
              " JOIN doc_header h ON h.id = l.doc_id"
              " JOIN item i ON i.id = l.item_id"
              " JOIN warehouse w ON w.id = l.warehouse_id"
              " WHERE (? = '' OR substr(l.biz_time, 1, 10) >= ?)"
              "   AND (? = '' OR substr(l.biz_time, 1, 10) <= ?)"
              "   AND (? = 0 OR l.item_id = ?)"
              "   AND (? = 0 OR l.warehouse_id = ?)"
              " ORDER BY l.id DESC LIMIT ?;");
    stmt.bind(1, filter.from)
        .bind(2, filter.from)
        .bind(3, filter.to)
        .bind(4, filter.to)
        .bind(5, filter.item_id)
        .bind(6, filter.item_id)
        .bind(7, filter.warehouse_id)
        .bind(8, filter.warehouse_id)
        .bind(9, static_cast<std::int64_t>(filter.limit));
    while (stmt.step()) {
      LedgerDetailRow row;
      row.id = stmt.i64(0);
      row.biz_time = stmt.text(1);
      row.biz_type = stmt.text(2);
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

std::vector<GoodsFlowRow> ReportService::goods_flow(const ReportFilter& filter) const {
  const std::string from_start = filter.from.empty() ? std::string{} : filter.from + " 00:00:00";
  std::vector<GoodsFlowRow> rows;
  db_.read([&](sqlite3* conn) {
    Stmt stmt(conn,
              "SELECT item_id, item_code, item_name, unit, warehouse_code, warehouse_name,"
              " opening_qty, in_qty, out_qty, opening_qty + in_qty - out_qty AS closing_qty,"
              " in_amount_cents, out_amount_cents"
              " FROM ("
              "   SELECT i.id AS item_id, i.code AS item_code, i.name AS item_name,"
              "     i.unit, w.code AS warehouse_code, w.name AS warehouse_name,"
              "     SUM(CASE WHEN l.biz_time < ?1 THEN l.qty ELSE 0 END) AS opening_qty,"
              "     SUM(CASE WHEN l.biz_time >= ?1"
              "              AND (?2 = '' OR substr(l.biz_time, 1, 10) <= ?2)"
              "              AND l.qty > 0 THEN l.qty ELSE 0 END) AS in_qty,"
              "     SUM(CASE WHEN l.biz_time >= ?1"
              "              AND (?2 = '' OR substr(l.biz_time, 1, 10) <= ?2)"
              "              AND l.qty < 0 THEN -l.qty ELSE 0 END) AS out_qty,"
              "     SUM(CASE WHEN l.biz_time >= ?1"
              "              AND (?2 = '' OR substr(l.biz_time, 1, 10) <= ?2)"
              "              AND l.qty > 0 THEN l.amount_cents ELSE 0 END) AS in_amount_cents,"
              "     SUM(CASE WHEN l.biz_time >= ?1"
              "              AND (?2 = '' OR substr(l.biz_time, 1, 10) <= ?2)"
              "              AND l.qty < 0 THEN -l.amount_cents ELSE 0 END) AS out_amount_cents"
              "   FROM stock_ledger l"
              "   JOIN item i ON i.id = l.item_id"
              "   JOIN warehouse w ON w.id = l.warehouse_id"
              "   WHERE (?3 = 0 OR l.warehouse_id = ?3)"
              "     AND (?4 = 0 OR l.item_id = ?4)"
              "   GROUP BY i.id, w.id"
              " ) t"
              " WHERE opening_qty <> 0 OR in_qty <> 0 OR out_qty <> 0"
              " ORDER BY item_code, warehouse_code;");
    // 我们的 Stmt 只支持位置绑定，这里重复绑定同一参数值。
    stmt.bind(1, from_start)
        .bind(2, filter.to)
        .bind(3, filter.warehouse_id)
        .bind(4, filter.item_id);
    while (stmt.step()) {
      GoodsFlowRow row;
      row.item_id = stmt.i64(0);
      row.item_code = stmt.text(1);
      row.item_name = stmt.text(2);
      row.unit = stmt.text(3);
      row.warehouse_code = stmt.text(4);
      row.warehouse_name = stmt.text(5);
      row.opening_qty = stmt.i64(6);
      row.in_qty = stmt.i64(7);
      row.out_qty = stmt.i64(8);
      row.closing_qty = stmt.i64(9);
      row.in_amount_cents = stmt.i64(10);
      row.out_amount_cents = stmt.i64(11);
      rows.push_back(std::move(row));
    }
  });
  return rows;
}

std::vector<AgingRow> ReportService::ar_ap_aging(int direction, std::string_view as_of) const {
  std::vector<AgingRow> rows;
  db_.read([&](sqlite3* conn) {
    Stmt stmt(conn,
              "SELECT partner_key, partner_name, direction,"
              " SUM(outstanding_cents) AS total_cents,"
              " SUM(CASE WHEN due_date = '' OR due_date >= ?1 THEN outstanding_cents ELSE 0 END)"
              "   AS not_due_cents,"
              " SUM(CASE WHEN due_date <> '' AND due_date < ?1"
              "          AND CAST(julianday(?1) - julianday(due_date) AS INTEGER) BETWEEN 1 AND 30"
              "          THEN outstanding_cents ELSE 0 END) AS d1_30_cents,"
              " SUM(CASE WHEN due_date <> '' AND due_date < ?1"
              "          AND CAST(julianday(?1) - julianday(due_date) AS INTEGER) BETWEEN 31 AND 60"
              "          THEN outstanding_cents ELSE 0 END) AS d31_60_cents,"
              " SUM(CASE WHEN due_date <> '' AND due_date < ?1"
              "          AND CAST(julianday(?1) - julianday(due_date) AS INTEGER) BETWEEN 61 AND 90"
              "          THEN outstanding_cents ELSE 0 END) AS d61_90_cents,"
              " SUM(CASE WHEN due_date <> '' AND due_date < ?1"
              "          AND CAST(julianday(?1) - julianday(due_date) AS INTEGER) > 90"
              "          THEN outstanding_cents ELSE 0 END) AS d90_plus_cents"
              " FROM (SELECT partner_key, partner_name, direction, due_date,"
              "         amount_cents - settled_cents AS outstanding_cents"
              "       FROM ar_ap_entry"
              "       WHERE status <> 'VOIDED' AND amount_cents - settled_cents <> 0"
              "         AND (?2 = 0 OR direction = ?2)) t"
              " GROUP BY partner_key, partner_name, direction"
              " ORDER BY total_cents DESC;");
    stmt.bind(1, std::string(as_of)).bind(2, static_cast<std::int64_t>(direction));
    while (stmt.step()) {
      AgingRow row;
      row.partner_key = stmt.text(0);
      row.partner_name = stmt.text(1);
      row.direction = static_cast<int>(stmt.i64(2));
      row.total_cents = stmt.i64(3);
      row.not_due_cents = stmt.i64(4);
      row.d1_30_cents = stmt.i64(5);
      row.d31_60_cents = stmt.i64(6);
      row.d61_90_cents = stmt.i64(7);
      row.d90_plus_cents = stmt.i64(8);
      rows.push_back(std::move(row));
    }
  });
  return rows;
}

}  // namespace xiaotu
