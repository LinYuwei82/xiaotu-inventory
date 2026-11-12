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

// 入库/退货：按录入单价入账（采购入库与销售退货的成本口径）。
std::int64_t amount_by_unit_price(std::int64_t qty, std::int64_t price,
                                  std::int64_t /*balance_qty*/,
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
          .cost_hook = amount_by_unit_price,
          .on_approve = nullptr,
          .on_reverse = nullptr,
      },
      // 采购退货：出库方向，按录入退货单价冲减库存金额（供应商退款口径）。
      DocumentDefinition{
          .doc_type = "PUR_RET",
          .doc_no_prefix = "CGTH",
          .stock_direction = StockDirection::Out,
          .affects_stock = true,
          .price_source = PriceSource::Manual,
          .cost_hook = amount_by_unit_price,
          .on_approve = nullptr,
          .on_reverse = nullptr,
      },
      // 销售退货：入库方向，按录入成本单价增加库存金额。
      DocumentDefinition{
          .doc_type = "SAL_RET",
          .doc_no_prefix = "XSTH",
          .stock_direction = StockDirection::In,
          .affects_stock = true,
          .price_source = PriceSource::Manual,
          .cost_hook = amount_by_unit_price,
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

void ensure_warehouse(sqlite3* conn, std::int64_t warehouse_id) {
  if (warehouse_id == 0) {
    throw AppError("INVALID_INPUT", "请选择仓库");
  }
  Stmt warehouse(conn, "SELECT 1 FROM warehouse WHERE id = ? AND status = 1 AND is_deleted = 0;");
  warehouse.bind(1, warehouse_id);
  if (!warehouse.step()) {
    throw AppError("NOT_FOUND", "仓库不存在或已停用");
  }
}

struct ValidatedDoc {
  std::string doc_date;
  std::string biz_time;
  std::string partner;
  std::int64_t total_qty = 0;
  std::int64_t total_amount = 0;
};

ValidatedDoc validate_document(sqlite3* conn, const DocInput& input, bool require_lines) {
  ensure_warehouse(conn, input.warehouse_id);
  if (require_lines && input.lines.empty()) {
    throw AppError("INVALID_INPUT", "请至少录入一行明细");
  }

  ValidatedDoc validated;
  validated.doc_date = input.doc_date.empty() ? today_text() : input.doc_date;
  if (validated.doc_date.size() != 10) {
    throw AppError("INVALID_INPUT", "单据日期格式应为 YYYY-MM-DD");
  }
  validated.biz_time = validated.doc_date + " " + now_text().substr(11);
  validated.partner = input.partner;

  // 引用了供应商档案时以档案名称为准，并校验其可用性。
  if (input.partner_id != 0) {
    Stmt supplier(conn, "SELECT name FROM supplier WHERE id = ? AND status = 1 AND is_deleted = 0;");
    supplier.bind(1, input.partner_id);
    if (!supplier.step()) {
      throw AppError("NOT_FOUND", "供应商不存在或已停用");
    }
    validated.partner = supplier.text(0);
  }

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
    validated.total_qty += line.qty;
    validated.total_amount += line.qty * line.price_cents;
  }
  return validated;
}

std::vector<std::int64_t> insert_lines(sqlite3* conn, std::int64_t doc_id,
                                       const std::vector<DocLineInput>& lines) {
  std::vector<std::int64_t> line_ids;
  line_ids.reserve(lines.size());
  std::int64_t line_no = 1;
  for (const DocLineInput& line : lines) {
    const std::int64_t line_id = next_id();
    Stmt insert_line(conn,
                     "INSERT INTO doc_line (id, doc_id, line_no, item_id, qty, price_cents,"
                     " amount_cents) VALUES (?, ?, ?, ?, ?, ?, ?);");
    insert_line.bind(1, line_id)
        .bind(2, doc_id)
        .bind(3, line_no)
        .bind(4, line.item_id)
        .bind(5, line.qty)
        .bind(6, line.price_cents)
        .bind(7, line.qty * line.price_cents)
        .run();
    line_ids.push_back(line_id);
    ++line_no;
  }
  return line_ids;
}

void insert_header(sqlite3* conn, std::int64_t id, const std::string& doc_no,
                   std::string_view doc_type, const DocInput& input,
                   const ValidatedDoc& validated, bool approved) {
  const AuditStamp stamp = audit_stamp();
  if (approved) {
    Stmt header(conn,
                "INSERT INTO doc_header (id, doc_no, doc_type, doc_date, warehouse_id, partner_id,"
                " customer, status, total_qty, total_amount_cents, created_at, created_by,"
                " approved_at, approved_by)"
                " VALUES (?, ?, ?, ?, ?, ?, ?, 'APPROVED', ?, ?, ?, ?, ?, ?);");
    header.bind(1, id)
        .bind(2, doc_no)
        .bind(3, doc_type)
        .bind(4, validated.doc_date)
        .bind(5, input.warehouse_id)
        .bind(6, input.partner_id)
        .bind(7, validated.partner)
        .bind(8, validated.total_qty)
        .bind(9, validated.total_amount)
        .bind(10, stamp.at)
        .bind(11, stamp.by)
        .bind(12, stamp.at)
        .bind(13, stamp.by)
        .run();
    return;
  }

  Stmt header(conn,
              "INSERT INTO doc_header (id, doc_no, doc_type, doc_date, warehouse_id, partner_id,"
              " customer, status, total_qty, total_amount_cents, created_at, created_by,"
              " approved_at, approved_by)"
              " VALUES (?, ?, ?, ?, ?, ?, ?, 'DRAFT', ?, ?, ?, ?, NULL, NULL);");
  header.bind(1, id)
      .bind(2, doc_no)
      .bind(3, doc_type)
      .bind(4, validated.doc_date)
      .bind(5, input.warehouse_id)
      .bind(6, input.partner_id)
      .bind(7, validated.partner)
      .bind(8, validated.total_qty)
      .bind(9, validated.total_amount)
      .bind(10, stamp.at)
      .bind(11, stamp.by)
      .run();
}

void apply_stock_moves(sqlite3* conn, const DocumentDefinition& definition, std::int64_t doc_id,
                       std::int64_t warehouse_id, const std::string& biz_time,
                       const std::vector<DocLineInput>& lines,
                       const std::vector<std::int64_t>& line_ids) {
  for (std::size_t i = 0; i < lines.size(); ++i) {
    apply_stock_move(conn, definition, doc_id, line_ids.at(i), warehouse_id, biz_time, lines.at(i));
  }
}

}  // namespace

DocumentEngine::Created DocumentEngine::save_and_approve(const DocInput& input) {
  const DocumentDefinition* definition = find_document_definition(input.doc_type);
  if (definition == nullptr) {
    throw AppError("UNKNOWN_DOC_TYPE", "未知单据类型: " + input.doc_type);
  }

  Created created;
  db_.write([&](sqlite3* conn) {
    const ValidatedDoc validated = validate_document(conn, input, true);
    created.id = next_id();
    created.doc_no = allocate_doc_no(conn, definition->doc_no_prefix, validated.doc_date);
    insert_header(conn, created.id, created.doc_no, definition->doc_type, input, validated, true);
    const std::vector<std::int64_t> line_ids = insert_lines(conn, created.id, input.lines);
    if (definition->affects_stock) {
      apply_stock_moves(conn, *definition, created.id, input.warehouse_id, validated.biz_time,
                        input.lines, line_ids);
    }
    if (definition->on_approve) {
      definition->on_approve(conn, created.id);
    }
  });
  return created;
}

DocumentEngine::Created DocumentEngine::save_draft(const DocInput& input) {
  const DocumentDefinition* definition = find_document_definition(input.doc_type);
  if (definition == nullptr) {
    throw AppError("UNKNOWN_DOC_TYPE", "未知单据类型: " + input.doc_type);
  }

  Created created;
  db_.write([&](sqlite3* conn) {
    const ValidatedDoc validated = validate_document(conn, input, false);
    created.id = next_id();
    created.doc_no = allocate_doc_no(conn, definition->doc_no_prefix, validated.doc_date);
    insert_header(conn, created.id, created.doc_no, definition->doc_type, input, validated, false);
    insert_lines(conn, created.id, input.lines);
  });
  return created;
}

void DocumentEngine::update_draft(std::int64_t doc_id, const DocInput& input) {
  const DocumentDefinition* definition = find_document_definition(input.doc_type);
  if (definition == nullptr) {
    throw AppError("UNKNOWN_DOC_TYPE", "未知单据类型: " + input.doc_type);
  }

  db_.write([&](sqlite3* conn) {
    std::string status;
    std::string doc_type;
    {
      Stmt header(conn, "SELECT status, doc_type FROM doc_header WHERE id = ?;");
      header.bind(1, doc_id);
      if (!header.step()) {
        throw AppError("NOT_FOUND", "单据不存在");
      }
      status = header.text(0);
      doc_type = header.text(1);
    }
    if (status != "DRAFT") {
      throw AppError("INVALID_STATUS", "只有草稿可以编辑");
    }
    if (doc_type != definition->doc_type) {
      throw AppError("INVALID_INPUT", "不能修改单据类型");
    }

    const ValidatedDoc validated = validate_document(conn, input, false);
    Stmt update(conn,
                "UPDATE doc_header SET doc_date = ?, warehouse_id = ?, partner_id = ?,"
                " customer = ?, total_qty = ?, total_amount_cents = ? WHERE id = ?;");
    update.bind(1, validated.doc_date)
        .bind(2, input.warehouse_id)
        .bind(3, input.partner_id)
        .bind(4, validated.partner)
        .bind(5, validated.total_qty)
        .bind(6, validated.total_amount)
        .bind(7, doc_id)
        .run();

    Stmt clear(conn, "DELETE FROM doc_line WHERE doc_id = ?;");
    clear.bind(1, doc_id).run();
    insert_lines(conn, doc_id, input.lines);
  });
}

void DocumentEngine::delete_draft(std::int64_t doc_id) {
  db_.write([&](sqlite3* conn) {
    std::string status;
    {
      Stmt header(conn, "SELECT status FROM doc_header WHERE id = ?;");
      header.bind(1, doc_id);
      if (!header.step()) {
        throw AppError("NOT_FOUND", "单据不存在");
      }
      status = header.text(0);
    }
    if (status != "DRAFT") {
      throw AppError("INVALID_STATUS", "只有草稿可以删除");
    }
    Stmt lines(conn, "DELETE FROM doc_line WHERE doc_id = ?;");
    lines.bind(1, doc_id).run();
    Stmt header(conn, "DELETE FROM doc_header WHERE id = ?;");
    header.bind(1, doc_id).run();
  });
}

void DocumentEngine::approve(std::int64_t doc_id) {
  db_.write([&](sqlite3* conn) {
    DocInput input;
    std::string status;
    {
      Stmt header(conn,
                  "SELECT doc_type, status, warehouse_id, doc_date, customer, partner_id"
                  " FROM doc_header WHERE id = ?;");
      header.bind(1, doc_id);
      if (!header.step()) {
        throw AppError("NOT_FOUND", "单据不存在");
      }
      input.doc_type = header.text(0);
      status = header.text(1);
      input.warehouse_id = header.i64(2);
      input.doc_date = header.text(3);
      input.partner = header.text(4);
      input.partner_id = header.i64(5);
    }
    if (status != "DRAFT") {
      throw AppError("INVALID_STATUS", "只有草稿可以审核");
    }
    const DocumentDefinition* definition = find_document_definition(input.doc_type);
    if (definition == nullptr) {
      throw AppError("UNKNOWN_DOC_TYPE", "未知单据类型: " + input.doc_type);
    }

    std::vector<std::int64_t> line_ids;
    {
      Stmt lines(conn,
                 "SELECT id, item_id, qty, price_cents FROM doc_line WHERE doc_id = ?"
                 " ORDER BY line_no;");
      lines.bind(1, doc_id);
      while (lines.step()) {
        line_ids.push_back(lines.i64(0));
        input.lines.push_back(DocLineInput{.item_id = lines.i64(1),
                                           .qty = lines.i64(2),
                                           .price_cents = lines.i64(3)});
      }
    }

    const ValidatedDoc validated = validate_document(conn, input, true);
    if (definition->affects_stock) {
      apply_stock_moves(conn, *definition, doc_id, input.warehouse_id, validated.biz_time,
                        input.lines, line_ids);
    }
    const AuditStamp stamp = audit_stamp();
    Stmt update(conn,
                "UPDATE doc_header SET status = 'APPROVED', total_qty = ?, total_amount_cents = ?,"
                " approved_at = ?, approved_by = ? WHERE id = ?;");
    update.bind(1, validated.total_qty)
        .bind(2, validated.total_amount)
        .bind(3, stamp.at)
        .bind(4, stamp.by)
        .bind(5, doc_id)
        .run();
    if (definition->on_approve) {
      definition->on_approve(conn, doc_id);
    }
  });
}

DocDetail DocumentEngine::detail(std::int64_t doc_id) {
  DocDetail detail;
  db_.read([&](sqlite3* conn) {
    {
      Stmt header(conn,
                  "SELECT id, doc_no, doc_type, doc_date, warehouse_id, partner_id, customer,"
                  " status, total_qty, total_amount_cents FROM doc_header WHERE id = ?;");
      header.bind(1, doc_id);
      if (!header.step()) {
        throw AppError("NOT_FOUND", "单据不存在");
      }
      detail.id = header.i64(0);
      detail.doc_no = header.text(1);
      detail.doc_type = header.text(2);
      detail.doc_date = header.text(3);
      detail.warehouse_id = header.i64(4);
      detail.partner_id = header.i64(5);
      detail.partner = header.text(6);
      detail.status = header.text(7);
      detail.total_qty = header.i64(8);
      detail.total_amount_cents = header.i64(9);
    }
    Stmt lines(conn,
               "SELECT item_id, qty, price_cents FROM doc_line WHERE doc_id = ? ORDER BY line_no;");
    lines.bind(1, doc_id);
    while (lines.step()) {
      detail.lines.push_back(DocLineInput{.item_id = lines.i64(0),
                                          .qty = lines.i64(1),
                                          .price_cents = lines.i64(2)});
    }
  });
  return detail;
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
