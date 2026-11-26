#include "xiaotu/cost_recalc.h"

#include <sqlite3.h>

#include <map>
#include <string>
#include <string_view>
#include <utility>

#include "xiaotu/document.h"
#include "xiaotu/qty.h"

namespace xiaotu {
namespace {

struct BalanceState {
  std::int64_t qty = 0;
  std::int64_t amount = 0;
};

struct BookBalance {
  std::int64_t qty = 0;
  std::int64_t amount = 0;
  bool exists = false;
};

struct LineAmounts {
  std::int64_t regular = 0;  // 正方向（调拨为出库腿）的重算金额
  std::int64_t in_leg = 0;   // 调拨入库腿的重算金额
  bool has_regular = false;
  bool has_in_leg = false;
};

bool ends_with(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string where_clause(std::int64_t item_id, std::int64_t warehouse_id,
                         const char* alias) {
  std::string sql;
  if (item_id > 0) {
    sql += " AND " + std::string(alias) + "item_id = " + std::to_string(item_id);
  }
  if (warehouse_id > 0) {
    sql += " AND " + std::string(alias) + "warehouse_id = " + std::to_string(warehouse_id);
  }
  return sql;
}

}  // namespace

CostRecalcResult CostRecalcService::recalculate(std::int64_t item_id,
                                                std::int64_t warehouse_id) const {
  CostRecalcResult result;
  std::map<std::pair<std::int64_t, std::int64_t>, BalanceState> replay;
  std::map<std::pair<std::int64_t, std::int64_t>, BookBalance> book;
  std::map<std::int64_t, LineAmounts> line_amounts;
  std::map<std::int64_t, std::pair<std::string, std::string>> item_names;
  std::map<std::int64_t, std::string> warehouse_names;

  db_.read([&](sqlite3* conn) {
    {
      Stmt stmt(conn, "SELECT id, code, name FROM item;");
      while (stmt.step()) {
        item_names[stmt.i64(0)] = {stmt.text(1), stmt.text(2)};
      }
    }
    {
      Stmt stmt(conn, "SELECT id, name FROM warehouse;");
      while (stmt.step()) {
        warehouse_names[stmt.i64(0)] = stmt.text(1);
      }
    }
    {
      const std::string sql =
          "SELECT warehouse_id, item_id, qty, amount_cents FROM stock_balance WHERE 1 = 1" +
          where_clause(item_id, warehouse_id, " ") + ";";
      Stmt stmt(conn, sql);
      while (stmt.step()) {
        const auto key = std::make_pair(stmt.i64(0), stmt.i64(1));
        book[key] = BookBalance{stmt.i64(2), stmt.i64(3), true};
      }
    }
    {
      const std::string sql =
          "SELECT l.warehouse_id, l.item_id, l.qty, l.amount_cents, l.biz_type,"
          " l.doc_line_id, COALESCE(d.price_cents, 0)"
          " FROM stock_ledger l LEFT JOIN doc_line d ON d.id = l.doc_line_id"
          " WHERE 1 = 1" +
          where_clause(item_id, warehouse_id, "l.") + " ORDER BY l.id;";
      Stmt stmt(conn, sql);
      while (stmt.step()) {
        ++result.ledger_rows;
        const std::int64_t wh = stmt.i64(0);
        const std::int64_t item = stmt.i64(1);
        const std::int64_t qty = stmt.i64(2);
        const std::int64_t stored_amount = stmt.i64(3);
        const std::string biz_type = stmt.text(4);
        const std::int64_t line_id = stmt.i64(5);
        const std::int64_t price = stmt.i64(6);

        BalanceState& state = replay[std::make_pair(wh, item)];
        LineAmounts& line = line_amounts[line_id];
        std::int64_t amount = stored_amount;

        if (qty > 0) {
          if (biz_type == "STK_CNT_IN") {
            // 盘盈：按当时结存的移动加权平均计价，不能走调拨入库腿分支。
            amount = state.qty > 0 ? moving_average_cost(qty, price, state.qty, state.amount)
                                   : mul_qty_price(qty, price);
            line.regular = amount;
            line.has_regular = true;
          } else if (biz_type == "PUR_IN") {
            amount = mul_qty_price(qty, price);
            line.regular = amount;
            line.has_regular = true;
          } else if (biz_type == "SAL_RET") {
            // 销售退货按退货时的移动加权成本入库，与 document.cpp 的成本钩子一致；
            // 单据上的销售单价只影响应收红字，不参与库存金额。
            amount = state.qty > 0 ? moving_average_cost(qty, price, state.qty, state.amount)
                                   : 0;
            line.regular = amount;
            line.has_regular = true;
          } else if (ends_with(biz_type, "_IN")) {
            // 调拨入库腿：金额与出库腿一致。
            amount = line.has_regular ? -line.regular : stored_amount;
            line.in_leg = amount;
            line.has_in_leg = true;
          }
        } else if (qty < 0) {
          if (biz_type == "STK_CNT_IN_REVERSE") {
            // 盘盈回冲：金额取盘盈行的负值，而不是误判成调拨入库腿回冲。
            amount = line.has_regular ? -line.regular : stored_amount;
          } else if (ends_with(biz_type, "_IN_REVERSE")) {
            amount = line.has_in_leg ? -line.in_leg : stored_amount;
          } else if (ends_with(biz_type, "REVERSE")) {
            amount = line.has_regular ? -line.regular : stored_amount;
          } else if (biz_type == "PUR_RET") {
            amount = mul_qty_price(qty, price);
            line.regular = amount;
            line.has_regular = true;
          } else {
            amount = -moving_average_cost(-qty, price, state.qty, state.amount);
            line.regular = amount;
            line.has_regular = true;
          }
        }

        state.qty += qty;
        state.amount += amount;
      }
    }
  });

  std::map<std::pair<std::int64_t, std::int64_t>, bool> keys;
  for (const auto& [key, state] : replay) {
    (void)state;
    keys[key] = true;
  }
  for (const auto& [key, value] : book) {
    (void)value;
    keys[key] = true;
  }

  result.item_count = static_cast<std::int64_t>(keys.size());
  for (const auto& [key, present] : keys) {
    (void)present;
    const auto replay_it = replay.find(key);
    const BalanceState state = replay_it == replay.end() ? BalanceState{} : replay_it->second;
    const auto book_it = book.find(key);
    const BookBalance current = book_it == book.end() ? BookBalance{} : book_it->second;
    const std::int64_t qty_diff = state.qty - current.qty;
    const std::int64_t amount_diff = state.amount - current.amount;
    if (qty_diff == 0 && amount_diff == 0) {
      continue;
    }
    CostRecalcRow row;
    row.item_id = key.second;
    row.warehouse_id = key.first;
    const auto item_it = item_names.find(key.second);
    if (item_it != item_names.end()) {
      row.item_code = item_it->second.first;
      row.item_name = item_it->second.second;
    }
    const auto warehouse_it = warehouse_names.find(key.first);
    if (warehouse_it != warehouse_names.end()) {
      row.warehouse_name = warehouse_it->second;
    }
    row.book_qty = current.qty;
    row.book_amount_cents = current.amount;
    row.replay_qty = state.qty;
    row.replay_amount_cents = state.amount;
    row.qty_diff = qty_diff;
    row.amount_diff_cents = amount_diff;
    if (qty_diff != 0) ++result.qty_difference_count;
    result.differences.push_back(std::move(row));
  }
  result.difference_count = static_cast<std::int64_t>(result.differences.size());
  return result;
}

}  // namespace xiaotu
