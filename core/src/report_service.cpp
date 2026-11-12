#include "xiaotu/report_service.h"

#include <sqlite3.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "xiaotu/db.h"

namespace xiaotu {
namespace {

// 往来档案名称：优先按 partner_key 前缀查档案，查不到再回落到台账上的名称快照。
std::string archive_name(sqlite3* conn, const std::string& partner_key) {
  const char* table = nullptr;
  if (partner_key.rfind("C:", 0) == 0) {
    table = "customer";
  } else if (partner_key.rfind("S:", 0) == 0) {
    table = "supplier";
  }
  if (table != nullptr && partner_key.size() > 2) {
    const std::string id = partner_key.substr(2);
    const bool numeric =
        !id.empty() && std::all_of(id.begin(), id.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
    if (numeric) {
      Stmt stmt(conn, std::string("SELECT name FROM ") + table + " WHERE id = ?;");
      stmt.bind(1, std::stoll(id));
      if (stmt.step()) return stmt.text(0);
    }
  }
  return {};
}

std::string ledger_name(sqlite3* conn, const std::string& partner_key) {
  Stmt entry(conn,
             "SELECT partner_name FROM ar_ap_entry WHERE partner_key = ?"
             " ORDER BY id DESC LIMIT 1;");
  entry.bind(1, partner_key);
  if (entry.step()) return entry.text(0);
  Stmt settlement(conn,
                  "SELECT partner_name FROM settlement WHERE partner_key = ?"
                  " ORDER BY id DESC LIMIT 1;");
  settlement.bind(1, partner_key);
  if (settlement.step()) return settlement.text(0);
  return {};
}

struct StatementRaw {
  std::string date;
  std::int64_t id = 0;
  int rank = 0;  // 同一天先列单据，再列核销
  StatementLine line;
};

}  // namespace

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

std::vector<InventorySummaryRow> ReportService::inventory_summary(
    const ReportFilter& filter) const {
  const std::string from_start = filter.from.empty() ? std::string{} : filter.from + " 00:00:00";
  std::vector<InventorySummaryRow> rows;
  db_.read([&](sqlite3* conn) {
    Stmt stmt(conn,
              "SELECT item_id, item_code, item_name, unit,"
              " opening_qty, in_qty, out_qty, opening_qty + in_qty - out_qty AS closing_qty,"
              " opening_amount, in_amount, out_amount,"
              " opening_amount + in_amount - out_amount AS closing_amount"
              " FROM ("
              "   SELECT i.id AS item_id, i.code AS item_code, i.name AS item_name, i.unit,"
              "     SUM(CASE WHEN l.biz_time < ?1 THEN l.qty ELSE 0 END) AS opening_qty,"
              "     SUM(CASE WHEN l.biz_time < ?1 THEN l.amount_cents ELSE 0 END) AS opening_amount,"
              "     SUM(CASE WHEN l.biz_time >= ?1"
              "              AND (?2 = '' OR substr(l.biz_time, 1, 10) <= ?2)"
              "              AND l.qty > 0 THEN l.qty ELSE 0 END) AS in_qty,"
              "     SUM(CASE WHEN l.biz_time >= ?1"
              "              AND (?2 = '' OR substr(l.biz_time, 1, 10) <= ?2)"
              "              AND (l.qty > 0 OR (l.qty = 0 AND l.amount_cents > 0))"
              "              THEN l.amount_cents ELSE 0 END) AS in_amount,"
              "     SUM(CASE WHEN l.biz_time >= ?1"
              "              AND (?2 = '' OR substr(l.biz_time, 1, 10) <= ?2)"
              "              AND l.qty < 0 THEN -l.qty ELSE 0 END) AS out_qty,"
              "     SUM(CASE WHEN l.biz_time >= ?1"
              "              AND (?2 = '' OR substr(l.biz_time, 1, 10) <= ?2)"
              "              AND (l.qty < 0 OR (l.qty = 0 AND l.amount_cents < 0))"
              "              THEN -l.amount_cents ELSE 0 END) AS out_amount"
              "   FROM stock_ledger l"
              "   JOIN item i ON i.id = l.item_id"
              "   WHERE (?3 = 0 OR l.warehouse_id = ?3)"
              "     AND (?4 = 0 OR l.item_id = ?4)"
              "   GROUP BY i.id"
              " ) t"
              " WHERE opening_qty <> 0 OR in_qty <> 0 OR out_qty <> 0"
              "    OR opening_amount <> 0 OR in_amount <> 0 OR out_amount <> 0"
              " ORDER BY item_code;");
    stmt.bind(1, from_start)
        .bind(2, filter.to)
        .bind(3, filter.warehouse_id)
        .bind(4, filter.item_id);
    while (stmt.step()) {
      InventorySummaryRow row;
      row.item_id = stmt.i64(0);
      row.item_code = stmt.text(1);
      row.item_name = stmt.text(2);
      row.unit = stmt.text(3);
      row.opening_qty = stmt.i64(4);
      row.in_qty = stmt.i64(5);
      row.out_qty = stmt.i64(6);
      row.closing_qty = stmt.i64(7);
      row.opening_amount_cents = stmt.i64(8);
      row.in_amount_cents = stmt.i64(9);
      row.out_amount_cents = stmt.i64(10);
      row.closing_amount_cents = stmt.i64(11);
      rows.push_back(std::move(row));
    }
  });
  return rows;
}

std::vector<SalesProfitRow> ReportService::sales_profit(const ReportFilter& filter) const {
  std::vector<SalesProfitRow> rows;
  db_.read([&](sqlite3* conn) {
    Stmt stmt(conn,
              " SELECT i.id, i.code, i.name, i.unit,"
              " SUM(CASE WHEN h.doc_type = 'SAL_RET' THEN -l.qty ELSE l.qty END) AS qty,"
              " SUM(CASE WHEN h.doc_type = 'SAL_RET' THEN -l.amount_cents"
              "          ELSE l.amount_cents END) AS revenue_cents,"
              " -SUM(COALESCE(s.amount_cents, 0)) AS cost_cents"
              " FROM doc_header h"
              " JOIN doc_line l ON l.doc_id = h.id"
              " JOIN item i ON i.id = l.item_id"
              " LEFT JOIN stock_ledger s ON s.doc_id = h.id AND s.doc_line_id = l.id"
              "   AND s.biz_type = h.doc_type"
              " WHERE h.doc_type IN ('SAL_OUT', 'SAL_RET') AND h.status = 'APPROVED'"
              "   AND (? = '' OR h.doc_date >= ?)"
              "   AND (? = '' OR h.doc_date <= ?)"
              "   AND (? = 0 OR l.item_id = ?)"
              "   AND (? = 0 OR h.customer_id = ?)"
              " GROUP BY i.id"
              " ORDER BY i.code;");
    stmt.bind(1, filter.from)
        .bind(2, filter.from)
        .bind(3, filter.to)
        .bind(4, filter.to)
        .bind(5, filter.item_id)
        .bind(6, filter.item_id)
        .bind(7, filter.customer_id)
        .bind(8, filter.customer_id);
    while (stmt.step()) {
      SalesProfitRow row;
      row.item_id = stmt.i64(0);
      row.item_code = stmt.text(1);
      row.item_name = stmt.text(2);
      row.unit = stmt.text(3);
      row.qty = stmt.i64(4);
      row.revenue_cents = stmt.i64(5);
      row.cost_cents = stmt.i64(6);
      row.profit_cents = row.revenue_cents - row.cost_cents;
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

StatementResult ReportService::partner_statement(std::string_view partner_key, int direction,
                                                 std::string_view from,
                                                 std::string_view to) const {
  StatementResult result;
  result.partner_key = std::string(partner_key);
  result.direction = direction == -1 ? -1 : 1;
  result.from = std::string(from);
  result.to = std::string(to);

  const std::string from_text = result.from;
  const std::string to_text = result.to;
  db_.read([&](sqlite3* conn) {
    result.partner_name = archive_name(conn, result.partner_key);
    if (result.partner_name.empty()) {
      result.partner_name = ledger_name(conn, result.partner_key);
    }

    // 期初：期间开始前的单据金额减去已核销金额（含撤销产生的负数记录）。
    Stmt opening(conn,
                 "SELECT COALESCE((SELECT SUM(amount_cents) FROM ar_ap_entry"
                 "                 WHERE partner_key = ? AND direction = ?"
                 "                   AND status <> 'VOIDED' AND biz_date < ?), 0)"
                 "     - COALESCE((SELECT SUM(amount_cents) FROM settlement"
                 "                 WHERE partner_key = ? AND direction = ?"
                 "                   AND settled_on < ?), 0);");
    opening.bind(1, result.partner_key)
        .bind(2, static_cast<std::int64_t>(result.direction))
        .bind(3, from_text)
        .bind(4, result.partner_key)
        .bind(5, static_cast<std::int64_t>(result.direction))
        .bind(6, from_text);
    if (opening.step()) result.opening_cents = opening.i64(0);

    std::vector<StatementRaw> raw;
    {
      Stmt stmt(conn,
                "SELECT id, biz_date, doc_no, doc_type, amount_cents FROM ar_ap_entry"
                " WHERE partner_key = ? AND direction = ? AND status <> 'VOIDED'"
                "   AND (? = '' OR biz_date >= ?)"
                "   AND (? = '' OR biz_date <= ?)"
                " ORDER BY biz_date, id;");
      stmt.bind(1, result.partner_key)
          .bind(2, static_cast<std::int64_t>(result.direction))
          .bind(3, from_text)
          .bind(4, from_text)
          .bind(5, to_text)
          .bind(6, to_text);
      while (stmt.step()) {
        StatementRaw row;
        row.id = stmt.i64(0);
        row.date = stmt.text(1);
        row.rank = 0;
        row.line.date = row.date;
        row.line.kind = "DOC";
        row.line.doc_no = stmt.text(2);
        row.line.doc_type = stmt.text(3);
        const std::int64_t delta = stmt.i64(4);
        if (delta >= 0) row.line.increase_cents = delta;
        else row.line.decrease_cents = -delta;
        raw.push_back(std::move(row));
      }
    }
    {
      Stmt stmt(conn,
                "SELECT id, settled_on, settle_no, amount_cents, reverses_id, note FROM settlement"
                " WHERE partner_key = ? AND direction = ?"
                "   AND (? = '' OR settled_on >= ?)"
                "   AND (? = '' OR settled_on <= ?)"
                " ORDER BY settled_on, id;");
      stmt.bind(1, result.partner_key)
          .bind(2, static_cast<std::int64_t>(result.direction))
          .bind(3, from_text)
          .bind(4, from_text)
          .bind(5, to_text)
          .bind(6, to_text);
      while (stmt.step()) {
        StatementRaw row;
        row.id = stmt.i64(0);
        row.date = stmt.text(1);
        row.rank = 1;
        row.line.date = row.date;
        row.line.kind = "SETTLE";
        row.line.doc_no = stmt.text(2);
        row.line.note = stmt.text(5);
        const std::int64_t delta = -stmt.i64(3);
        if (delta >= 0) row.line.increase_cents = delta;
        else row.line.decrease_cents = -delta;
        raw.push_back(std::move(row));
      }
    }

    std::stable_sort(raw.begin(), raw.end(), [](const StatementRaw& a, const StatementRaw& b) {
      if (a.date != b.date) return a.date < b.date;
      if (a.rank != b.rank) return a.rank < b.rank;
      return a.id < b.id;
    });

    std::int64_t balance = result.opening_cents;
    for (auto& row : raw) {
      balance += row.line.increase_cents - row.line.decrease_cents;
      row.line.balance_cents = balance;
      result.increase_cents += row.line.increase_cents;
      result.decrease_cents += row.line.decrease_cents;
      result.lines.push_back(std::move(row.line));
    }
    result.closing_cents = balance;
  });
  return result;
}

}  // namespace xiaotu
