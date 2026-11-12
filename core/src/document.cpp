#include "xiaotu/document.h"

#include <sqlite3.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "xiaotu/crud.h"
#include "xiaotu/doc_number.h"
#include "xiaotu/error.h"
#include "xiaotu/ids.h"

namespace xiaotu {
namespace {

// 移动加权平均：按结存金额比例结转，保证清零时金额精确归零。
std::int64_t moving_average_cost(std::int64_t qty, std::int64_t /*price*/, std::int64_t balance_qty,
                                 std::int64_t balance_amount) {
  if (balance_qty <= 0 || balance_amount <= 0) {
    return 0;
  }
  const std::int64_t cost = (balance_amount * qty + balance_qty / 2) / balance_qty;
  return std::clamp<std::int64_t>(cost, 0, balance_amount);
}

// 入库：按单据金额入账。
std::int64_t purchase_cost(std::int64_t qty, std::int64_t price, std::int64_t /*balance_qty*/,
                           std::int64_t /*balance_amount*/) {
  return qty * price;
}

}  // namespace

const std::vector<DocumentDefinition>& document_definitions() {
  static const std::vector<DocumentDefinition> definitions = {
      // 销售出库：保存即审核，出库方向，成本走移动加权平均，取价方式为手工录入。
      DocumentDefinition{
          .doc_type = "SAL_OUT",
          .doc_no_prefix = "XSCK",
          .stock_direction = StockDirection::Out,
          .affects_stock = true,
          .price_source = PriceSource::Manual,
          .cost_hook = moving_average_cost,
          .on_approve = nullptr,
          .on_reverse = nullptr,
      },
      // 采购入库的定义先落位（M1 接入，不改流程代码）。
      DocumentDefinition{
          .doc_type = "PUR_IN",
          .doc_no_prefix = "CGRK",
          .stock_direction = StockDirection::In,
          .affects_stock = true,
          .price_source = PriceSource::Manual,
          .cost_hook = purchase_cost,
          .on_approve = nullptr,
          .on_reverse = nullptr,
      },
  };
  return definitions;
}

const DocumentDefinition* find_document_definition(std::string_view doc_type) {
  for (const auto& definition : document_definitions()) {
    if (definition.doc_type == doc_type) {
      return &definition;
    }
  }
  return nullptr;
}

namespace {

void apply_stock_move(sqlite3* conn, const DocumentDefinition& definition, std::int64_t doc_id,
                      std::int64_t line_id, std::int64_t warehouse_id, const std::string& biz_time,
                      const DocLineInput& line) {
  std::int64_t balance_qty = 0;
  std::int64_t balance_amount = 0;
  {
    Stmt balance(conn,
                 "SELECT qty, amount_cents FROM stock_balance WHERE warehouse_id = ? AND item_id = ?;");
    balance.bind(1, warehouse_id).bind(2, line.item_id);
    if (balance.step()) {
      balance_qty = balance.i64(0);
      balance_amount = balance.i64(1);
    }
  }

  const std::int64_t cost =
      definition.cost_hook ? definition.cost_hook(line.qty, line.price_cents, balance_qty,
                                                  balance_amount)
                           : 0;
  const bool outgoing = definition.stock_direction == StockDirection::Out;
  const std::int64_t delta_qty = outgoing ? -line.qty : line.qty;
  const std::int64_t delta_amount = outgoing ? -cost : cost;

  {
    Stmt ledger(conn,
                "INSERT INTO stock_ledger (id, doc_id, doc_line_id, biz_type, biz_time,"
                " warehouse_id, item_id, qty, amount_cents, created_at)"
                " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
    ledger.bind(1, next_id())
        .bind(2, doc_id)
        .bind(3, line_id)
        .bind(4, definition.doc_type)
        .bind(5, biz_time)
        .bind(6, warehouse_id)
        .bind(7, line.item_id)
        .bind(8, delta_qty)
        .bind(9, delta_amount)
        .bind(10, now_text())
        .run();
  }

  Stmt upsert(conn,
              "INSERT INTO stock_balance (warehouse_id, item_id, qty, amount_cents, updated_at)"
              " VALUES (?, ?, ?, ?, ?)"
              " ON CONFLICT (warehouse_id, item_id) DO UPDATE SET"
              "   qty = qty + excluded.qty,"
              "   amount_cents = amount_cents + excluded.amount_cents,"
              "   updated_at = excluded.updated_at;");
  upsert.bind(1, warehouse_id)
      .bind(2, line.item_id)
      .bind(3, delta_qty)
      .bind(4, delta_amount)
      .bind(5, now_text())
      .run();
}

}  // namespace

DocumentEngine::Created DocumentEngine::save_and_approve(const DocInput& input) {
  const DocumentDefinition* definition = find_document_definition(input.doc_type);
  if (definition == nullptr) {
    throw AppError("UNKNOWN_DOC_TYPE", "未知单据类型: " + input.doc_type);
  }
  if (input.warehouse_id == 0) {
    throw AppError("INVALID_INPUT", "请选择仓库");
  }
  if (input.lines.empty()) {
    throw AppError("INVALID_INPUT", "请至少录入一行明细");
  }

  const std::string doc_date = input.doc_date.empty() ? today_text() : input.doc_date;
  if (doc_date.size() != 10) {
    throw AppError("INVALID_INPUT", "单据日期格式应为 YYYY-MM-DD");
  }
  const std::string biz_time = doc_date + " " + now_text().substr(11);

  Created created;
  created.id = next_id();

  db_.write([&](sqlite3* conn) {
    {
      Stmt warehouse(conn,
                     "SELECT 1 FROM warehouse WHERE id = ? AND status = 1 AND is_deleted = 0;");
      warehouse.bind(1, input.warehouse_id);
      if (!warehouse.step()) {
        throw AppError("NOT_FOUND", "仓库不存在或已停用");
      }
    }

    std::int64_t total_qty = 0;
    std::int64_t total_amount = 0;
    for (const DocLineInput& line : input.lines) {
      if (line.item_id == 0) {
        throw AppError("INVALID_INPUT", "明细行未选择商品");
      }
      if (line.qty <= 0) {
        throw AppError("INVALID_INPUT", "数量必须大于 0");
      }
      if (line.price_cents < 0) {
        throw AppError("INVALID_INPUT", "单价不能为负数");
      }
      Stmt item(conn, "SELECT 1 FROM item WHERE id = ? AND status = 1 AND is_deleted = 0;");
      item.bind(1, line.item_id);
      if (!item.step()) {
        throw AppError("NOT_FOUND", "商品不存在或已停用");
      }
      total_qty += line.qty;
      total_amount += line.qty * line.price_cents;
    }

    created.doc_no = allocate_doc_no(conn, definition->doc_no_prefix, doc_date);
    const AuditStamp stamp = audit_stamp();

    {
      Stmt header(conn,
                  "INSERT INTO doc_header (id, doc_no, doc_type, doc_date, warehouse_id, customer,"
                  " status, total_qty, total_amount_cents, created_at, created_by, approved_at,"
                  " approved_by)"
                  " VALUES (?, ?, ?, ?, ?, ?, 'APPROVED', ?, ?, ?, ?, ?, ?);");
      header.bind(1, created.id)
          .bind(2, created.doc_no)
          .bind(3, definition->doc_type)
          .bind(4, doc_date)
          .bind(5, input.warehouse_id)
          .bind(6, input.partner)
          .bind(7, total_qty)
          .bind(8, total_amount)
          .bind(9, stamp.at)
          .bind(10, stamp.by)
          .bind(11, stamp.at)
          .bind(12, stamp.by)
          .run();
    }

    std::int64_t line_no = 1;
    for (const DocLineInput& line : input.lines) {
      const std::int64_t line_id = next_id();
      Stmt insert_line(conn,
                       "INSERT INTO doc_line (id, doc_id, line_no, item_id, qty, price_cents,"
                       " amount_cents) VALUES (?, ?, ?, ?, ?, ?, ?);");
      insert_line.bind(1, line_id)
          .bind(2, created.id)
          .bind(3, line_no)
          .bind(4, line.item_id)
          .bind(5, line.qty)
          .bind(6, line.price_cents)
          .bind(7, line.qty * line.price_cents)
          .run();

      if (definition->affects_stock) {
        apply_stock_move(conn, *definition, created.id, line_id, input.warehouse_id, biz_time, line);
      }
      ++line_no;
    }

    if (definition->on_approve) {
      definition->on_approve(conn, created.id);
    }
  });

  return created;
}

std::vector<DocSummary> DocumentEngine::list(std::string_view doc_type, int limit) {
  if (find_document_definition(doc_type) == nullptr) {
    throw AppError("UNKNOWN_DOC_TYPE", "未知单据类型: " + std::string(doc_type));
  }

  std::vector<DocSummary> documents;
  db_.read([&](sqlite3* conn) {
    Stmt stmt(conn,
              "SELECT h.id, h.doc_no, h.doc_date, w.name, h.customer, h.total_qty,"
              " h.total_amount_cents, h.status, h.created_at"
              " FROM doc_header h JOIN warehouse w ON w.id = h.warehouse_id"
              " WHERE h.doc_type = ? ORDER BY h.id DESC LIMIT ?;");
    stmt.bind(1, doc_type).bind(2, static_cast<std::int64_t>(limit));
    while (stmt.step()) {
      DocSummary summary;
      summary.id = stmt.i64(0);
      summary.doc_no = stmt.text(1);
      summary.doc_date = stmt.text(2);
      summary.warehouse_name = stmt.text(3);
      summary.customer = stmt.text(4);
      summary.total_qty = stmt.i64(5);
      summary.total_amount_cents = stmt.i64(6);
      summary.status = stmt.text(7);
      summary.created_at = stmt.text(8);
      documents.push_back(std::move(summary));
    }
  });
  return documents;
}

void DocumentEngine::reverse(std::int64_t doc_id) {
  db_.write([&](sqlite3* conn) {
    std::string doc_type;
    std::string status;
    std::int64_t warehouse_id = 0;
    {
      Stmt header(conn, "SELECT doc_type, status, warehouse_id FROM doc_header WHERE id = ?;");
      header.bind(1, doc_id);
      if (!header.step()) {
        throw AppError("NOT_FOUND", "单据不存在");
      }
      doc_type = header.text(0);
      status = header.text(1);
      warehouse_id = header.i64(2);
    }
    if (status != "APPROVED") {
      throw AppError("INVALID_STATUS", "只有已审核的单据可以反审核");
    }

    struct LedgerRow {
      std::int64_t line_id;
      std::int64_t item_id;
      std::int64_t qty;
      std::int64_t amount;
    };
    std::vector<LedgerRow> rows;
    {
      Stmt ledger(conn,
                  "SELECT doc_line_id, item_id, qty, amount_cents FROM stock_ledger"
                  " WHERE doc_id = ? AND biz_type = ?;");
      ledger.bind(1, doc_id).bind(2, doc_type);
      while (ledger.step()) {
        rows.push_back(LedgerRow{ledger.i64(0), ledger.i64(1), ledger.i64(2), ledger.i64(3)});
      }
    }

    for (const LedgerRow& row : rows) {
      Stmt ledger(conn,
                  "INSERT INTO stock_ledger (id, doc_id, doc_line_id, biz_type, biz_time,"
                  " warehouse_id, item_id, qty, amount_cents, created_at)"
                  " VALUES (?, ?, ?, 'REVERSE', ?, ?, ?, ?, ?, ?);");
      ledger.bind(1, next_id())
          .bind(2, doc_id)
          .bind(3, row.line_id)
          .bind(4, now_text())
          .bind(5, warehouse_id)
          .bind(6, row.item_id)
          .bind(7, -row.qty)
          .bind(8, -row.amount)
          .bind(9, now_text())
          .run();

      Stmt upsert(conn,
                  "INSERT INTO stock_balance (warehouse_id, item_id, qty, amount_cents, updated_at)"
                  " VALUES (?, ?, ?, ?, ?)"
                  " ON CONFLICT (warehouse_id, item_id) DO UPDATE SET"
                  "   qty = qty + excluded.qty,"
                  "   amount_cents = amount_cents + excluded.amount_cents,"
                  "   updated_at = excluded.updated_at;");
      upsert.bind(1, warehouse_id)
          .bind(2, row.item_id)
          .bind(3, -row.qty)
          .bind(4, -row.amount)
          .bind(5, now_text())
          .run();
    }

    Stmt update(conn, "UPDATE doc_header SET status = 'VOIDED' WHERE id = ?;");
    update.bind(1, doc_id).run();

    if (const DocumentDefinition* definition = find_document_definition(doc_type)) {
      if (definition->on_reverse) {
        definition->on_reverse(conn, doc_id);
      }
    }
  });
}

}  // namespace xiaotu
