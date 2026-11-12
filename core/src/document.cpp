#include "xiaotu/document.h"

#include <sqlite3.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "xiaotu/crud.h"
#include "xiaotu/audit.h"
#include "xiaotu/doc_number.h"
#include "xiaotu/error.h"
#include "xiaotu/ids.h"
#include "xiaotu/qty.h"

namespace xiaotu {
namespace {

// 入库/退货：按录入单价入账（采购入库与销售退货的成本口径）。
std::int64_t amount_by_unit_price(std::int64_t qty, std::int64_t price,
                                  std::int64_t /*balance_qty*/,
                                  std::int64_t /*balance_amount*/) {
  return mul_qty_price(qty, price);
}

}  // namespace

std::int64_t moving_average_cost(std::int64_t qty, std::int64_t /*price_cents*/,
                                 std::int64_t balance_qty, std::int64_t balance_amount) {
  if (balance_qty <= 0 || balance_amount <= 0) {
    return 0;
  }
  const std::int64_t cost = (balance_amount * qty + balance_qty / 2) / balance_qty;
  return std::clamp<std::int64_t>(cost, 0, balance_amount);
}

const std::vector<DocumentDefinition>& document_definitions() {
  static const std::vector<DocumentDefinition> definitions = {
      // 销售出库：保存即审核，出库方向，成本走移动加权平均，取价方式为手工录入。
      DocumentDefinition{
          .doc_type = "SAL_OUT",
          .doc_no_prefix = "XSCK",
          .stock_mode = StockMode::Out,
          .affects_stock = true,
          .partner_kind = PartnerKind::Customer,
          .finance_direction = 1,
          .finance_red = false,
          .price_source = PriceSource::Manual,
          .cost_hook = moving_average_cost,
          .on_approve = nullptr,
          .on_reverse = nullptr,
      },
      // 采购入库的定义先落位（M1 接入，不改流程代码）。
      DocumentDefinition{
          .doc_type = "PUR_IN",
          .doc_no_prefix = "CGRK",
          .stock_mode = StockMode::In,
          .affects_stock = true,
          .partner_kind = PartnerKind::Supplier,
          .finance_direction = -1,
          .finance_red = false,
          .price_source = PriceSource::Manual,
          .cost_hook = amount_by_unit_price,
          .on_approve = nullptr,
          .on_reverse = nullptr,
      },
      // 采购退货：出库方向，按录入退货单价冲减库存金额（供应商退款口径）。
      DocumentDefinition{
          .doc_type = "PUR_RET",
          .doc_no_prefix = "CGTH",
          .stock_mode = StockMode::Out,
          .affects_stock = true,
          .partner_kind = PartnerKind::Supplier,
          .finance_direction = -1,
          .finance_red = true,
          .price_source = PriceSource::Manual,
          .cost_hook = amount_by_unit_price,
          .on_approve = nullptr,
          .on_reverse = nullptr,
      },
      // 销售退货：入库方向。本版未做「退货关联原销售单」，成本按退货时的移动
      // 加权成本冲回（与出库同一套成本口径），不使用单据上的销售单价。
      DocumentDefinition{
          .doc_type = "SAL_RET",
          .doc_no_prefix = "XSTH",
          .stock_mode = StockMode::In,
          .affects_stock = true,
          .partner_kind = PartnerKind::Customer,
          .finance_direction = 1,
          .finance_red = true,
          .price_source = PriceSource::Manual,
          .cost_hook = moving_average_cost,
          .on_approve = nullptr,
          .on_reverse = nullptr,
      },
      // 调拨：源仓按移动加权平均出库，目标仓按同一成本入库。
      DocumentDefinition{
          .doc_type = "STK_TRF",
          .doc_no_prefix = "DBD",
          .stock_mode = StockMode::Transfer,
          .affects_stock = true,
          .price_source = PriceSource::Manual,
          .cost_hook = nullptr,
          .on_approve = nullptr,
          .on_reverse = nullptr,
      },
      // 盘点：明细数量为实盘数，审核时按差额生成调整流水。
      DocumentDefinition{
          .doc_type = "STK_CNT",
          .doc_no_prefix = "PDD",
          .stock_mode = StockMode::Stocktake,
          .affects_stock = true,
          .price_source = PriceSource::Manual,
          .cost_hook = nullptr,
          .on_approve = nullptr,
          .on_reverse = nullptr,
      },
      // 成本调整：不改数量，只按明细行仓库调整结存金额；审核权限仅 cost.adjust。
      DocumentDefinition{
          .doc_type = "CST_ADJ",
          .doc_no_prefix = "CBTZ",
          .stock_mode = StockMode::CostAdjust,
          .affects_stock = true,
          .price_source = PriceSource::Manual,
          .cost_hook = nullptr,
          .on_approve = nullptr,
          .on_reverse = nullptr,
          .write_permission = "cost.adjust",
      },
      // 销售订单：只做需求确认与转出库来源，不占库存、不写流水、不产生往来。
      DocumentDefinition{
          .doc_type = "SAL_ORDER",
          .doc_no_prefix = "XSDD",
          .stock_mode = StockMode::None,
          .affects_stock = false,
          .role = DocumentRole::Order,
          .partner_kind = PartnerKind::Customer,
          .finance_direction = 0,
          .finance_red = false,
          .price_source = PriceSource::Manual,
          .cost_hook = nullptr,
          .on_approve = nullptr,
          .on_reverse = nullptr,
          .order_converts_to = "SAL_OUT",
      },
      // 采购订单：只做需求确认与转入库来源，不占库存、不写流水、不产生往来。
      DocumentDefinition{
          .doc_type = "PUR_ORDER",
          .doc_no_prefix = "CGDD",
          .stock_mode = StockMode::None,
          .affects_stock = false,
          .role = DocumentRole::Order,
          .partner_kind = PartnerKind::Supplier,
          .finance_direction = 0,
          .finance_red = false,
          .price_source = PriceSource::Manual,
          .cost_hook = nullptr,
          .on_approve = nullptr,
          .on_reverse = nullptr,
          .order_converts_to = "PUR_IN",
      },
      // 其他入库：按录入单价增加库存金额，不绑定往来单位、不产生应收应付。
      DocumentDefinition{
          .doc_type = "OTH_IN",
          .doc_no_prefix = "QTRK",
          .stock_mode = StockMode::In,
          .affects_stock = true,
          .partner_kind = PartnerKind::None,
          .finance_direction = 0,
          .finance_red = false,
          .price_source = PriceSource::Manual,
          .cost_hook = amount_by_unit_price,
          .on_approve = nullptr,
          .on_reverse = nullptr,
      },
      // 其他出库：按当前移动加权平均结转成本，出库前校验可用库存，不产生往来。
      DocumentDefinition{
          .doc_type = "OTH_OUT",
          .doc_no_prefix = "QTCK",
          .stock_mode = StockMode::Out,
          .affects_stock = true,
          .partner_kind = PartnerKind::None,
          .finance_direction = 0,
          .finance_red = false,
          .price_source = PriceSource::Manual,
          .cost_hook = moving_average_cost,
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

struct BalanceState {
  std::int64_t qty = 0;
  std::int64_t amount = 0;
};

BalanceState read_balance(sqlite3* conn, std::int64_t warehouse_id, std::int64_t item_id) {
  BalanceState state;
  Stmt balance(conn,
               "SELECT qty, amount_cents FROM stock_balance WHERE warehouse_id = ? AND item_id = ?;");
  balance.bind(1, warehouse_id).bind(2, item_id);
  if (balance.step()) {
    state.qty = balance.i64(0);
    state.amount = balance.i64(1);
  }
  return state;
}

// 所有结存写入的唯一出口：同一事务内先算后写，数量或金额为负直接拒绝。
// 正常出库/反审核仍由 ensure_available / REVERSE_WOULD_NEGATIVE 给出更具体的
// 提示；这里是最后的兜底，覆盖盘点、成本调整和将来新增的写入路径。
// 负库存开关：按 app_setting 的显式 '1' 放行数量为负；任何其他值都按拒绝处理。
bool allow_negative_stock(sqlite3* conn) {
  Stmt stmt(conn, "SELECT value FROM app_setting WHERE key = 'allow_negative_stock';");
  return stmt.step() && stmt.text(0) == "1";
}

void apply_balance_delta(sqlite3* conn, std::int64_t warehouse_id, std::int64_t item_id,
                         std::int64_t delta_qty, std::int64_t delta_amount) {
  const BalanceState current = read_balance(conn, warehouse_id, item_id);
  const std::int64_t next_qty = current.qty + delta_qty;
  const std::int64_t next_amount = current.amount + delta_amount;
  // 数量为负是否允许由系统参数决定；金额为负始终拒绝（负库存只放行数量）。
  if ((next_qty < 0 && !allow_negative_stock(conn)) || next_amount < 0) {
    std::string code;
    std::string name;
    Stmt item(conn, "SELECT code, name FROM item WHERE id = ?;");
    item.bind(1, item_id);
    if (item.step()) {
      code = item.text(0);
      name = item.text(1);
    }
    const std::string label = code + " " + name;
    if (next_qty < 0) {
      throw AppError("STOCK_WOULD_NEGATIVE",
                     "结存数量不能为负：" + label + " 当前结存 " + qty_text(current.qty) +
                         "，本次变动 " + qty_text(delta_qty));
    }
    throw AppError("STOCK_WOULD_NEGATIVE",
                   "结存金额不能为负：" + label + " 当前结存 " +
                       std::to_string(current.amount) + " 分，本次变动 " +
                       std::to_string(delta_amount) + " 分");
  }

  // 写最终值而不是 `qty = qty + ?`：BEFORE INSERT 触发器在 UPSERT 冲突判定前
  // 就会看到待插入的负数增量，增量写法会被误伤。这里已持有写事务，先算后写无竞态。
  Stmt update(conn,
              "UPDATE stock_balance SET qty = ?, amount_cents = ?, updated_at = ?"
              " WHERE warehouse_id = ? AND item_id = ?;");
  update.bind(1, next_qty)
      .bind(2, next_amount)
      .bind(3, now_text())
      .bind(4, warehouse_id)
      .bind(5, item_id)
      .run();
  if (sqlite3_changes(conn) == 0) {
    Stmt insert(conn,
                "INSERT INTO stock_balance (warehouse_id, item_id, qty, amount_cents, updated_at)"
                " VALUES (?, ?, ?, ?, ?);");
    insert.bind(1, warehouse_id)
        .bind(2, item_id)
        .bind(3, next_qty)
        .bind(4, next_amount)
        .bind(5, now_text())
        .run();
  }
}

// 追加流水并同步结存；调用方必须处于同一个写事务内。
void write_move(sqlite3* conn, const std::string& biz_type, std::int64_t doc_id,
                std::int64_t line_id, std::int64_t warehouse_id, std::int64_t item_id,
                const std::string& biz_time, std::int64_t delta_qty, std::int64_t delta_amount) {
  Stmt ledger(conn,
              "INSERT INTO stock_ledger (id, doc_id, doc_line_id, biz_type, biz_time,"
              " warehouse_id, item_id, qty, amount_cents, created_at)"
              " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
  ledger.bind(1, next_id())
      .bind(2, doc_id)
      .bind(3, line_id)
      .bind(4, biz_type)
      .bind(5, biz_time)
      .bind(6, warehouse_id)
      .bind(7, item_id)
      .bind(8, delta_qty)
      .bind(9, delta_amount)
      .bind(10, now_text())
      .run();

  apply_balance_delta(conn, warehouse_id, item_id, delta_qty, delta_amount);
}

// 出库/调拨前校验可用数量：不足则整单事务回滚，不会出现半途扣减或负数结存。
void ensure_available(sqlite3* conn, std::int64_t warehouse_id, std::int64_t item_id,
                      std::int64_t need) {
  const BalanceState balance = read_balance(conn, warehouse_id, item_id);
  if (balance.qty >= need) {
    return;
  }
  std::string code;
  std::string name;
  Stmt item(conn, "SELECT code, name FROM item WHERE id = ?;");
  item.bind(1, item_id);
  if (item.step()) {
    code = item.text(0);
    name = item.text(1);
  }
  throw AppError("STOCK_INSUFFICIENT", "库存不足：" + code + " " + name + " 可用 " +
                                          qty_text(balance.qty) + "，需出库 " +
                                          qty_text(need));
}

// 负库存开关：按 app_setting 的显式 '1' 放行；任何其他值都按拒绝处理。
void apply_in_or_out(sqlite3* conn, const DocumentDefinition& definition, std::int64_t doc_id,
                     std::int64_t line_id, std::int64_t warehouse_id, const std::string& biz_time,
                     const DocLineInput& line) {
  const BalanceState balance = read_balance(conn, warehouse_id, line.item_id);
  const bool outgoing = definition.stock_mode == StockMode::Out;
  if (outgoing && !allow_negative_stock(conn)) {
    ensure_available(conn, warehouse_id, line.item_id, line.qty);
  }

  const std::int64_t cost =
      definition.cost_hook ? definition.cost_hook(line.qty, line.price_cents, balance.qty,
                                                  balance.amount)
                           : 0;
  write_move(conn, definition.doc_type, doc_id, line_id, warehouse_id, line.item_id, biz_time,
             outgoing ? -line.qty : line.qty, outgoing ? -cost : cost);
}

// 调拨：源仓按移动加权平均出库，目标仓按同一成本入库，两侧同一事务。
void apply_transfer(sqlite3* conn, const DocumentDefinition& definition, std::int64_t doc_id,
                    std::int64_t line_id, std::int64_t warehouse_id,
                    std::int64_t target_warehouse_id, const std::string& biz_time,
                    const DocLineInput& line) {
  ensure_available(conn, warehouse_id, line.item_id, line.qty);
  const BalanceState source = read_balance(conn, warehouse_id, line.item_id);
  const std::int64_t cost =
      moving_average_cost(line.qty, line.price_cents, source.qty, source.amount);
  write_move(conn, definition.doc_type, doc_id, line_id, warehouse_id, line.item_id, biz_time,
             -line.qty, -cost);
  // 入库腿使用独立 biz_type：同一明细行有两条流水，唯一索引按 (doc_line_id, biz_type) 约束。
  write_move(conn, definition.doc_type + "_IN", doc_id, line_id, target_warehouse_id, line.item_id,
             biz_time, line.qty, cost);
}

// 盘点：明细数量为实盘数，按差额写调整流水；差额为 0 不写。
void apply_stocktake(sqlite3* conn, const DocumentDefinition& definition, std::int64_t doc_id,
                     std::int64_t line_id, std::int64_t warehouse_id, const std::string& biz_time,
                     const DocLineInput& line) {
  const BalanceState balance = read_balance(conn, warehouse_id, line.item_id);
  const std::int64_t diff = line.qty - balance.qty;
  if (diff == 0) {
    return;
  }
  std::int64_t amount = 0;
  if (diff > 0) {
    amount = balance.qty > 0 ? moving_average_cost(diff, line.price_cents, balance.qty,
                                                   balance.amount)
                             : mul_qty_price(diff, line.price_cents);
  } else {
    amount = -moving_average_cost(-diff, line.price_cents, balance.qty, balance.amount);
  }
  write_move(conn, definition.doc_type + (diff > 0 ? "_IN" : "_OUT"), doc_id, line_id,
             warehouse_id, line.item_id, biz_time, diff, amount);
}

// 成本调整：数量不变，只改结存金额；零库存不允许留下金额，调整后金额不能为负。
void ensure_warehouse(sqlite3* conn, std::int64_t warehouse_id);

void apply_cost_adjust(sqlite3* conn, const DocumentDefinition& definition, std::int64_t doc_id,
                       std::int64_t line_id, const std::string& biz_time,
                       const DocLineInput& line) {
  ensure_warehouse(conn, line.warehouse_id);
  const BalanceState balance = read_balance(conn, line.warehouse_id, line.item_id);
  const std::int64_t next_amount = balance.amount + line.adjust_cents;
  if (balance.qty == 0 && next_amount != 0) {
    throw AppError("INVALID_INPUT", "零库存不能有结存金额，请用盘点单处理");
  }
  if (next_amount < 0) {
    throw AppError("INVALID_INPUT", "调整后结存金额不能为负");
  }
  write_move(conn, definition.doc_type, doc_id, line_id, line.warehouse_id, line.item_id, biz_time,
             0, line.adjust_cents);
}

void apply_stock_move(sqlite3* conn, const DocumentDefinition& definition, std::int64_t doc_id,
                      std::int64_t line_id, std::int64_t warehouse_id,
                      std::int64_t target_warehouse_id, const std::string& biz_time,
                      const DocLineInput& line) {
  switch (definition.stock_mode) {
    case StockMode::In:
    case StockMode::Out:
      apply_in_or_out(conn, definition, doc_id, line_id, warehouse_id, biz_time, line);
      break;
    case StockMode::Transfer:
      apply_transfer(conn, definition, doc_id, line_id, warehouse_id, target_warehouse_id, biz_time,
                     line);
      break;
    case StockMode::Stocktake:
      apply_stocktake(conn, definition, doc_id, line_id, warehouse_id, biz_time, line);
      break;
    case StockMode::CostAdjust:
      apply_cost_adjust(conn, definition, doc_id, line_id, biz_time, line);
      break;
    case StockMode::None:
      break;
  }
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
  std::string biz_category;
  std::int64_t salesperson_id = 0;
  std::string salesperson_name;
  std::int64_t total_qty = 0;
  std::int64_t total_amount = 0;
};

bool is_sales_document(const std::string& doc_type) {
  return doc_type == "SAL_ORDER" || doc_type == "SAL_OUT" || doc_type == "SAL_RET";
}

std::string cents_text(std::int64_t cents) {
  char buffer[48];
  std::snprintf(buffer, sizeof(buffer), "%lld.%02lld", static_cast<long long>(cents / 100),
                static_cast<long long>(cents % 100));
  return std::string(buffer);
}

// 信用额度卡在销售出库审核（产生应收的那一刻），只做客户侧。
// 额度 0 视为不限制；mode=warn 时首次返回 CREDIT_LIMIT_WARNING 由界面二次确认，
// 确认后放行并写审计；mode=reject 直接拒绝。
void enforce_credit_limit(sqlite3* conn, const DocumentDefinition& definition,
                          const DocInput& input, const ValidatedDoc& validated,
                          std::int64_t doc_id, bool confirm_credit, const std::string& mode) {
  if (definition.doc_type != "SAL_OUT" || input.partner_id <= 0) return;
  std::int64_t limit = 0;
  {
    Stmt stmt(conn, "SELECT credit_limit_cents FROM customer WHERE id = ? AND is_deleted = 0;");
    stmt.bind(1, input.partner_id);
    if (!stmt.step()) return;
    limit = stmt.i64(0);
  }
  if (limit <= 0) return;
  std::int64_t used = 0;
  {
    Stmt stmt(conn,
              "SELECT COALESCE(SUM(amount_cents - settled_cents), 0) FROM ar_ap_entry"
              " WHERE direction = 1 AND status IN ('OPEN', 'PARTIAL') AND partner_id = ?;");
    stmt.bind(1, input.partner_id);
    if (stmt.step()) used = stmt.i64(0);
  }
  const std::int64_t after = used + validated.total_amount;
  if (after <= limit) return;
  const std::string detail = "客户 " + validated.partner + " 信用额度 " + cents_text(limit) +
                             "，已占用 " + cents_text(used) + "，本次 " +
                             cents_text(validated.total_amount) + "，超出 " +
                             cents_text(after - limit);
  if (mode == "reject") {
    throw AppError("CREDIT_LIMIT_EXCEEDED", "超出客户信用额度：" + detail);
  }
  if (!confirm_credit) {
    throw AppError("CREDIT_LIMIT_WARNING", "超出客户信用额度：" + detail);
  }
  audit_write(conn, "credit_over_limit", "doc_header", doc_id, "", detail);
}

// 其他出入库的业务类型白名单：赠品两方向通用，期初只入库，领用与报损只出库；
// 其他单据一律清空，避免旧客户端误传脏值。
std::string normalize_biz_category(const DocumentDefinition& definition,
                                   const std::string& value) {
  const bool inbound = definition.doc_type == "OTH_IN";
  const bool outbound = definition.doc_type == "OTH_OUT";
  if (!inbound && !outbound) return {};
  const std::string category = value.empty() ? "OTHER" : value;
  const bool allowed = category == "GIFT" || category == "OTHER" ||
                       (inbound && category == "OPENING") ||
                       (outbound && (category == "USAGE" || category == "LOSS"));
  if (!allowed) {
    throw AppError("INVALID_INPUT",
                   inbound ? "其他入库的业务类型只能是赠品、期初或其他"
                           : "其他出库的业务类型只能是赠品、领用、报损或其他");
  }
  return category;
}

ValidatedDoc validate_document(sqlite3* conn, const DocInput& input,
                               const DocumentDefinition& definition, bool require_lines) {
  const bool cost_adjust = definition.stock_mode == StockMode::CostAdjust;
  if (!cost_adjust) {
    ensure_warehouse(conn, input.warehouse_id);
  }
  if (definition.stock_mode == StockMode::Transfer) {
    if (input.target_warehouse_id == 0) {
      throw AppError("INVALID_INPUT", "请选择调入仓库");
    }
    if (input.target_warehouse_id == input.warehouse_id) {
      throw AppError("INVALID_INPUT", "调出仓库与调入仓库不能相同");
    }
    ensure_warehouse(conn, input.target_warehouse_id);
  }
  if ((require_lines || cost_adjust) && input.lines.empty()) {
    throw AppError("INVALID_INPUT", "请至少录入一行明细");
  }
  // 采购入库/退货必须填供应商：草稿可暂不填，审核时卡住。
  // 档案 ID 与名称快照都为空才算「没填」；历史文本往来单位仍可正常审核。
  if (require_lines && (definition.doc_type == "PUR_IN" || definition.doc_type == "PUR_RET") &&
      input.partner_id == 0 && input.partner.empty()) {
    throw AppError("INVALID_INPUT", "采购单据必须选择供应商");
  }

  const bool allow_zero_qty = definition.stock_mode == StockMode::Stocktake;

  ValidatedDoc validated;
  validated.doc_date = input.doc_date.empty() ? today_text() : input.doc_date;
  if (validated.doc_date.size() != 10) {
    throw AppError("INVALID_INPUT", "单据日期格式应为 YYYY-MM-DD");
  }
  validated.biz_time = validated.doc_date + " " + now_text().substr(11);
  validated.partner = input.partner;
  validated.biz_category = normalize_biz_category(definition, input.biz_category);
  // 业务员只落在销售类单据；未指定保持 0 与空名称，停用职员仍允许历史使用。
  if (is_sales_document(definition.doc_type) && input.salesperson_id != 0) {
    Stmt person(conn, "SELECT name FROM employee WHERE id = ? AND is_deleted = 0;");
    person.bind(1, input.salesperson_id);
    if (!person.step()) {
      throw AppError("NOT_FOUND", "业务员不存在或已删除");
    }
    validated.salesperson_id = input.salesperson_id;
    validated.salesperson_name = person.text(0);
  }

  // 引用了档案时以档案名称为准，并校验其可用性。
  if (input.partner_id != 0) {
    const char* table =
        definition.partner_kind == PartnerKind::Customer ? "customer" : "supplier";
    const char* label =
        definition.partner_kind == PartnerKind::Customer ? "客户不存在或已停用" : "供应商不存在或已停用";
    Stmt partner(conn, std::string("SELECT name FROM ") + table +
                           " WHERE id = ? AND status = 1 AND is_deleted = 0;");
    partner.bind(1, input.partner_id);
    if (!partner.step()) {
      throw AppError("NOT_FOUND", label);
    }
    validated.partner = partner.text(0);
  }

  for (const DocLineInput& line : input.lines) {
    if (line.item_id == 0) {
      throw AppError("INVALID_INPUT", "明细行未选择商品");
    }
    if (cost_adjust) {
      if (line.warehouse_id == 0) {
        throw AppError("INVALID_INPUT", "成本调整单需逐行指定仓库");
      }
      ensure_warehouse(conn, line.warehouse_id);
      if (line.qty != 0 || line.price_cents != 0) {
        throw AppError("INVALID_INPUT", "成本调整单不改数量与单价");
      }
      if (line.adjust_cents == 0) {
        throw AppError("INVALID_INPUT", "调整金额不能为 0");
      }
      if (line.adjust_reason.empty()) {
        throw AppError("INVALID_INPUT", "请填写调整原因");
      }
      if (line.adjust_reason.size() > 100) {
        throw AppError("INVALID_INPUT", "调整原因最多 100 个字符");
      }
      Stmt item(conn, "SELECT 1 FROM item WHERE id = ? AND status = 1 AND is_deleted = 0;");
      item.bind(1, line.item_id);
      if (!item.step()) {
        throw AppError("NOT_FOUND", "商品不存在或已停用");
      }
      validated.total_amount += line.adjust_cents;
      continue;
    }
    if (allow_zero_qty ? line.qty < 0 : line.qty <= 0) {
      throw AppError("INVALID_INPUT", allow_zero_qty ? "实盘数量不能为负数" : "数量必须大于 0");
    }
    if (line.price_cents < 0) {
      throw AppError("INVALID_INPUT", "单价不能为负数");
    }
    Stmt item(conn,
              "SELECT i.unit, COALESCE(u.qty_decimals, 0) FROM item i"
              " LEFT JOIN unit u ON u.id = i.unit_id"
              " WHERE i.id = ? AND i.status = 1 AND i.is_deleted = 0;");
    item.bind(1, line.item_id);
    if (!item.step()) {
      throw AppError("NOT_FOUND", "商品不存在或已停用");
    }
    const std::string unit = item.text(0);
    const int decimals = static_cast<int>(item.i64(1));
    if (!qty_fits_decimals(line.qty, decimals)) {
      throw AppError("INVALID_INPUT", "数量小数位超过单位「" + unit + "」的精度（" +
                                          std::to_string(decimals) + " 位）");
    }
    validated.total_qty += line.qty;
    validated.total_amount += mul_qty_price(line.qty, line.price_cents);
  }
  return validated;
}

std::vector<std::int64_t> insert_lines(sqlite3* conn, std::int64_t doc_id,
                                       const std::vector<DocLineInput>& lines, bool cost_adjust) {
  std::vector<std::int64_t> line_ids;
  line_ids.reserve(lines.size());
  std::int64_t line_no = 1;
  for (const DocLineInput& line : lines) {
    const std::int64_t line_id = next_id();
    Stmt insert_line(conn,
                     "INSERT INTO doc_line (id, doc_id, line_no, item_id, qty, price_cents,"
                     " amount_cents, source_order_line_id, warehouse_id, adjust_cents,"
                     " adjust_reason)"
                     " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
    insert_line.bind(1, line_id)
        .bind(2, doc_id)
        .bind(3, line_no)
        .bind(4, line.item_id)
        .bind(5, line.qty)
        .bind(6, line.price_cents)
        .bind(7, cost_adjust ? 0 : mul_qty_price(line.qty, line.price_cents))
        .bind(8, line.source_order_line_id)
        .bind(9, cost_adjust ? line.warehouse_id : 0)
        .bind(10, cost_adjust ? line.adjust_cents : 0)
        .bind(11, cost_adjust ? line.adjust_reason : std::string{})
        .run();
    line_ids.push_back(line_id);
    ++line_no;
  }
  return line_ids;
}

std::int64_t header_warehouse_id(const DocumentDefinition& definition, const DocInput& input) {
  // 成本调整单的仓库在明细行；表头取第一行仓库，仅用于满足历史表结构。
  if (definition.stock_mode == StockMode::CostAdjust && input.warehouse_id == 0 &&
      !input.lines.empty()) {
    return input.lines.front().warehouse_id;
  }
  return input.warehouse_id;
}

void insert_header(sqlite3* conn, std::int64_t id, const std::string& doc_no,
                   std::string_view doc_type, PartnerKind partner_kind, const DocInput& input,
                   std::int64_t warehouse_id, const ValidatedDoc& validated, bool approved,
                   std::string_view order_status) {
  const AuditStamp stamp = audit_stamp();
  // 供应商写 partner_id，客户写 customer_id；两者互斥，partner 文本都保留快照。
  const std::int64_t supplier_id = partner_kind == PartnerKind::Supplier ? input.partner_id : 0;
  const std::int64_t customer_id = partner_kind == PartnerKind::Customer ? input.partner_id : 0;
  if (approved) {
    Stmt header(conn,
                "INSERT INTO doc_header (id, doc_no, doc_type, doc_date, warehouse_id,"
                " target_warehouse_id, partner_id, customer_id, customer, biz_category,"
                " salesperson_id, salesperson_name,"
                " status, order_status, total_qty, total_amount_cents, created_at, created_by,"
                " approved_at, approved_by)"
                " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 'APPROVED', ?, ?, ?, ?, ?, ?, ?);");
    header.bind(1, id)
        .bind(2, doc_no)
        .bind(3, doc_type)
        .bind(4, validated.doc_date)
        .bind(5, warehouse_id)
        .bind(6, input.target_warehouse_id)
        .bind(7, supplier_id)
        .bind(8, customer_id)
        .bind(9, validated.partner)
        .bind(10, validated.biz_category)
        .bind(11, validated.salesperson_id)
        .bind(12, validated.salesperson_name)
        .bind(13, order_status)
        .bind(14, validated.total_qty)
        .bind(15, validated.total_amount)
        .bind(16, stamp.at)
        .bind(17, stamp.by)
        .bind(18, stamp.at)
        .bind(19, stamp.by)
        .run();
    return;
  }

  Stmt header(conn,
              "INSERT INTO doc_header (id, doc_no, doc_type, doc_date, warehouse_id,"
              " target_warehouse_id, partner_id, customer_id, customer, biz_category,"
              " salesperson_id, salesperson_name,"
              " status, order_status, total_qty, total_amount_cents, created_at, created_by,"
              " approved_at, approved_by)"
              " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 'DRAFT', ?, ?, ?, ?, ?, NULL, NULL);");
  header.bind(1, id)
      .bind(2, doc_no)
      .bind(3, doc_type)
      .bind(4, validated.doc_date)
      .bind(5, warehouse_id)
      .bind(6, input.target_warehouse_id)
      .bind(7, supplier_id)
      .bind(8, customer_id)
      .bind(9, validated.partner)
      .bind(10, validated.biz_category)
      .bind(11, validated.salesperson_id)
      .bind(12, validated.salesperson_name)
      .bind(13, order_status)
      .bind(14, validated.total_qty)
      .bind(15, validated.total_amount)
      .bind(16, stamp.at)
      .bind(17, stamp.by)
      .run();
}

void apply_stock_moves(sqlite3* conn, const DocumentDefinition& definition, std::int64_t doc_id,
                       std::int64_t warehouse_id, std::int64_t target_warehouse_id,
                       const std::string& biz_time, const std::vector<DocLineInput>& lines,
                       const std::vector<std::int64_t>& line_ids) {
  for (std::size_t i = 0; i < lines.size(); ++i) {
    apply_stock_move(conn, definition, doc_id, line_ids.at(i), warehouse_id, target_warehouse_id,
                     biz_time, lines.at(i));
  }
}

std::string doc_detail(const std::string& doc_type, const ValidatedDoc& validated) {
  return "{\"doc_type\":\"" + doc_type + "\",\"qty\":\"" + qty_text(validated.total_qty) +
         "\",\"amount_cents\":" + std::to_string(validated.total_amount) + "}";
}

std::string partner_key_of(PartnerKind kind, std::int64_t partner_id, const std::string& name) {
  if (partner_id > 0) {
    return (kind == PartnerKind::Customer ? "C:" : "S:") + std::to_string(partner_id);
  }
  if (!name.empty()) {
    return "T:" + name;
  }
  return "T:（未指定）";
}

// 审核时生成往来台账：销售应收、采购应付，退货为负数（红字）。
void create_ar_ap_entry(sqlite3* conn, const DocumentDefinition& definition,
                        const DocInput& input, const ValidatedDoc& validated,
                        std::int64_t doc_id, const std::string& doc_no) {
  if (definition.finance_direction == 0) {
    return;
  }
  const std::int64_t amount =
      definition.finance_red ? -validated.total_amount : validated.total_amount;
  const std::string partner_key =
      partner_key_of(definition.partner_kind, input.partner_id, validated.partner);
  Stmt stmt(conn,
            "INSERT INTO ar_ap_entry (id, doc_id, doc_no, doc_type, direction, partner_id,"
            " partner_name, partner_key, biz_date, due_date, amount_cents, settled_cents,"
            " status, created_at, created_by)"
            " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0, 'OPEN', ?, ?);");
  stmt.bind(1, next_id())
      .bind(2, doc_id)
      .bind(3, doc_no)
      .bind(4, definition.doc_type)
      .bind(5, static_cast<std::int64_t>(definition.finance_direction))
      .bind(6, input.partner_id)
      .bind(7, validated.partner)
      .bind(8, partner_key)
      .bind(9, validated.doc_date)
      .bind(10, validated.doc_date)
      .bind(11, amount)
      .bind(12, audit_stamp().at)
      .bind(13, audit_stamp().by)
      .run();
}

// 反审核：已核销必须拦截；未核销则作废台账，不物理删除。
void void_ar_ap_entry(sqlite3* conn, std::int64_t doc_id) {
  std::int64_t entry_id = 0;
  std::int64_t settled = 0;
  {
    Stmt query(conn,
               "SELECT id, settled_cents FROM ar_ap_entry"
               " WHERE doc_id = ? AND status <> 'VOIDED' LIMIT 1;");
    query.bind(1, doc_id);
    if (query.step()) {
      entry_id = query.i64(0);
      settled = query.i64(1);
    }
  }
  if (entry_id == 0) {
    return;
  }
  if (settled != 0) {
    throw AppError("ENTRY_SETTLED", "该单据已有核销记录，请先撤销核销再反审核");
  }
  Stmt update(conn, "UPDATE ar_ap_entry SET status = 'VOIDED' WHERE id = ?;");
  update.bind(1, entry_id).run();
}

// 反审核销售出库单时，把来源订单行的已转数量退回去；订单状态按剩余量重算。
void release_order_conversions(sqlite3* conn, std::int64_t doc_id) {
  struct Link {
    std::int64_t order_line_id;
    std::int64_t qty;
  };
  std::vector<Link> links;
  {
    Stmt stmt(conn,
              "SELECT source_order_line_id, qty FROM doc_line"
              " WHERE doc_id = ? AND source_order_line_id > 0;");
    stmt.bind(1, doc_id);
    while (stmt.step()) {
      links.push_back(Link{stmt.i64(0), stmt.i64(1)});
    }
  }
  std::vector<std::int64_t> order_ids;
  for (const Link& link : links) {
    {
      Stmt dec(conn,
               "UPDATE doc_line SET converted_qty = MAX(converted_qty - ?, 0) WHERE id = ?;");
      dec.bind(1, link.qty).bind(2, link.order_line_id).run();
    }
    {
      Stmt order(conn, "SELECT doc_id FROM doc_line WHERE id = ?;");
      order.bind(1, link.order_line_id);
      if (order.step()) {
        const std::int64_t order_id = order.i64(0);
        if (std::find(order_ids.begin(), order_ids.end(), order_id) == order_ids.end()) {
          order_ids.push_back(order_id);
        }
      }
    }
  }
  for (const std::int64_t order_id : order_ids) {
    std::string order_status;
    {
      Stmt status(conn, "SELECT order_status FROM doc_header WHERE id = ?;");
      status.bind(1, order_id);
      if (!status.step()) continue;
      order_status = status.text(0);
    }
    // 已关闭的订单保持关闭，只允许释放数量，不自动恢复可转状态。
    if (order_status == "CLOSED") continue;
    std::int64_t remaining = 0;
    {
      Stmt stmt(conn,
                "SELECT COUNT(*) FROM doc_line WHERE doc_id = ? AND converted_qty < qty;");
      stmt.bind(1, order_id);
      if (stmt.step()) remaining = stmt.i64(0);
    }
    Stmt update(conn, "UPDATE doc_header SET order_status = ? WHERE id = ?;");
    update.bind(1, remaining == 0 ? "CONVERTED" : "CONFIRMED").bind(2, order_id).run();
  }
}

}  // namespace

DocumentEngine::Created DocumentEngine::save_and_approve(const DocInput& input,
                                                         bool confirm_credit) {
  const DocumentDefinition* definition = find_document_definition(input.doc_type);
  if (definition == nullptr) {
    throw AppError("UNKNOWN_DOC_TYPE", "未知单据类型: " + input.doc_type);
  }

  Created created;
  db_.write([&](sqlite3* conn) {
    const ValidatedDoc validated = validate_document(conn, input, *definition, true);
    created.id = next_id();
    enforce_credit_limit(conn, *definition, input, validated, created.id, confirm_credit,
                         credit_limit_mode_);
    created.doc_no = allocate_doc_no(conn, definition->doc_no_prefix, validated.doc_date);
    insert_header(conn, created.id, created.doc_no, definition->doc_type,
                  definition->partner_kind, input, header_warehouse_id(*definition, input),
                  validated, true,
                  definition->role == DocumentRole::Order ? "CONFIRMED" : "");
    const std::vector<std::int64_t> line_ids =
        insert_lines(conn, created.id, input.lines,
                     definition->stock_mode == StockMode::CostAdjust);
    if (definition->affects_stock) {
      apply_stock_moves(conn, *definition, created.id, input.warehouse_id,
                        input.target_warehouse_id, validated.biz_time, input.lines, line_ids);
    }
    if (definition->on_approve) {
      definition->on_approve(conn, created.id);
    }
    create_ar_ap_entry(conn, *definition, input, validated, created.id, created.doc_no);
    audit_write(conn, "approve", "document", created.id, created.doc_no,
                doc_detail(definition->doc_type, validated));
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
    const ValidatedDoc validated = validate_document(conn, input, *definition, false);
    created.id = next_id();
    created.doc_no = allocate_doc_no(conn, definition->doc_no_prefix, validated.doc_date);
    insert_header(conn, created.id, created.doc_no, definition->doc_type,
                  definition->partner_kind, input, header_warehouse_id(*definition, input),
                  validated, false, "");
    insert_lines(conn, created.id, input.lines,
                 definition->stock_mode == StockMode::CostAdjust);
    audit_write(conn, "save_draft", "document", created.id, created.doc_no,
                doc_detail(definition->doc_type, validated));
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
    std::string doc_no;
    {
      Stmt header(conn, "SELECT status, doc_type, doc_no FROM doc_header WHERE id = ?;");
      header.bind(1, doc_id);
      if (!header.step()) {
        throw AppError("NOT_FOUND", "单据不存在");
      }
      status = header.text(0);
      doc_type = header.text(1);
      doc_no = header.text(2);
    }
    if (status != "DRAFT") {
      throw AppError("INVALID_STATUS", "只有草稿可以编辑");
    }
    if (doc_type != definition->doc_type) {
      throw AppError("INVALID_INPUT", "不能修改单据类型");
    }

    const ValidatedDoc validated = validate_document(conn, input, *definition, false);
    Stmt update(conn,
                "UPDATE doc_header SET doc_date = ?, warehouse_id = ?, target_warehouse_id = ?,"
                " partner_id = ?, customer_id = ?, customer = ?, biz_category = ?,"
                " salesperson_id = ?, salesperson_name = ?, total_qty = ?,"
                " total_amount_cents = ? WHERE id = ?;");
    const std::int64_t supplier_id =
        definition->partner_kind == PartnerKind::Supplier ? input.partner_id : 0;
    const std::int64_t customer_id =
        definition->partner_kind == PartnerKind::Customer ? input.partner_id : 0;
    update.bind(1, validated.doc_date)
        .bind(2, input.warehouse_id)
        .bind(3, input.target_warehouse_id)
        .bind(4, supplier_id)
        .bind(5, customer_id)
        .bind(6, validated.partner)
        .bind(7, validated.biz_category)
        .bind(8, validated.salesperson_id)
        .bind(9, validated.salesperson_name)
        .bind(10, validated.total_qty)
        .bind(11, validated.total_amount)
        .bind(12, doc_id)
        .run();

    Stmt clear(conn, "DELETE FROM doc_line WHERE doc_id = ?;");
    clear.bind(1, doc_id).run();
    insert_lines(conn, doc_id, input.lines,
                 definition->stock_mode == StockMode::CostAdjust);
    audit_write(conn, "update_draft", "document", doc_id, doc_no,
                doc_detail(doc_type, validated));
  });
}

void DocumentEngine::delete_draft(std::int64_t doc_id) {
  db_.write([&](sqlite3* conn) {
    std::string status;
    std::string doc_no;
    {
      Stmt header(conn, "SELECT status, doc_no FROM doc_header WHERE id = ?;");
      header.bind(1, doc_id);
      if (!header.step()) {
        throw AppError("NOT_FOUND", "单据不存在");
      }
      status = header.text(0);
      doc_no = header.text(1);
    }
    if (status != "DRAFT") {
      throw AppError("INVALID_STATUS", "只有草稿可以删除");
    }
    Stmt lines(conn, "DELETE FROM doc_line WHERE doc_id = ?;");
    lines.bind(1, doc_id).run();
    Stmt header(conn, "DELETE FROM doc_header WHERE id = ?;");
    header.bind(1, doc_id).run();
    audit_write(conn, "delete_draft", "document", doc_id, doc_no);
  });
}

void DocumentEngine::approve(std::int64_t doc_id, bool confirm_credit) {
  db_.write([&](sqlite3* conn) {
    DocInput input;
    std::string status;
    std::string doc_no;
    std::int64_t supplier_id = 0;
    std::int64_t customer_id = 0;
    {
      Stmt header(conn,
                  "SELECT doc_type, status, warehouse_id, doc_date, customer, partner_id, doc_no,"
                  " target_warehouse_id, customer_id, biz_category, salesperson_id"
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
      supplier_id = header.i64(5);
      doc_no = header.text(6);
      input.target_warehouse_id = header.i64(7);
      customer_id = header.i64(8);
      input.biz_category = header.text(9);
      input.salesperson_id = header.i64(10);
    }
    if (status != "DRAFT") {
      throw AppError("INVALID_STATUS", "只有草稿可以审核");
    }
    const DocumentDefinition* definition = find_document_definition(input.doc_type);
    if (definition == nullptr) {
      throw AppError("UNKNOWN_DOC_TYPE", "未知单据类型: " + input.doc_type);
    }
    input.partner_id =
        definition->partner_kind == PartnerKind::Customer ? customer_id : supplier_id;

    std::vector<std::int64_t> line_ids;
    {
      Stmt lines(conn,
                 "SELECT id, item_id, qty, price_cents, warehouse_id, adjust_cents,"
                 " adjust_reason FROM doc_line WHERE doc_id = ? ORDER BY line_no;");
      lines.bind(1, doc_id);
      while (lines.step()) {
        line_ids.push_back(lines.i64(0));
        input.lines.push_back(DocLineInput{.item_id = lines.i64(1),
                                           .qty = lines.i64(2),
                                           .price_cents = lines.i64(3),
                                           .warehouse_id = lines.i64(4),
                                           .adjust_cents = lines.i64(5),
                                           .adjust_reason = lines.text(6)});
      }
    }

    const ValidatedDoc validated = validate_document(conn, input, *definition, true);
    enforce_credit_limit(conn, *definition, input, validated, doc_id, confirm_credit,
                         credit_limit_mode_);
    if (definition->affects_stock) {
      apply_stock_moves(conn, *definition, doc_id, input.warehouse_id,
                        input.target_warehouse_id, validated.biz_time, input.lines, line_ids);
    }
    const AuditStamp stamp = audit_stamp();
    Stmt update(conn,
                "UPDATE doc_header SET status = 'APPROVED', total_qty = ?, total_amount_cents = ?,"
                " salesperson_id = ?, salesperson_name = ?, approved_at = ?, approved_by = ?"
                " WHERE id = ?;");
    update.bind(1, validated.total_qty)
        .bind(2, validated.total_amount)
        .bind(3, validated.salesperson_id)
        .bind(4, validated.salesperson_name)
        .bind(5, stamp.at)
        .bind(6, stamp.by)
        .bind(7, doc_id)
        .run();
    if (definition->role == DocumentRole::Order) {
      // 订单的「审核」即确认；确认后不可再改单，只能转出库或关闭。
      Stmt confirm(conn, "UPDATE doc_header SET order_status = 'CONFIRMED' WHERE id = ?;");
      confirm.bind(1, doc_id).run();
    }
    if (definition->on_approve) {
      definition->on_approve(conn, doc_id);
    }
    create_ar_ap_entry(conn, *definition, input, validated, doc_id, doc_no);
    audit_write(conn, "approve", "document", doc_id, doc_no,
                doc_detail(definition->doc_type, validated));
  });
}

DocDetail DocumentEngine::detail(std::int64_t doc_id) {
  DocDetail detail;
  db_.read([&](sqlite3* conn) {
    {
      Stmt header(conn,
                  "SELECT id, doc_no, doc_type, doc_date, warehouse_id, target_warehouse_id,"
                  " partner_id, customer_id, customer, status, order_status, total_qty,"
                  " total_amount_cents, biz_category, salesperson_id, salesperson_name"
                  " FROM doc_header WHERE id = ?;");
      header.bind(1, doc_id);
      if (!header.step()) {
        throw AppError("NOT_FOUND", "单据不存在");
      }
      detail.id = header.i64(0);
      detail.doc_no = header.text(1);
      detail.doc_type = header.text(2);
      detail.doc_date = header.text(3);
      detail.warehouse_id = header.i64(4);
      detail.target_warehouse_id = header.i64(5);
      const std::int64_t supplier_id = header.i64(6);
      const std::int64_t customer_id = header.i64(7);
      detail.partner = header.text(8);
      detail.status = header.text(9);
      detail.order_status = header.text(10);
      detail.total_qty = header.i64(11);
      detail.total_amount_cents = header.i64(12);
      detail.biz_category = header.text(13);
      detail.salesperson_id = header.i64(14);
      detail.salesperson_name = header.text(15);
      const DocumentDefinition* definition = find_document_definition(detail.doc_type);
      detail.partner_id = definition != nullptr && definition->partner_kind == PartnerKind::Customer
                              ? customer_id
                              : supplier_id;
    }
    Stmt lines(conn,
               "SELECT id, item_id, qty, price_cents, converted_qty, source_order_line_id,"
               " warehouse_id, adjust_cents, adjust_reason"
               " FROM doc_line WHERE doc_id = ? ORDER BY line_no;");
    lines.bind(1, doc_id);
    while (lines.step()) {
      detail.lines.push_back(DocLineInput{.item_id = lines.i64(1),
                                          .qty = lines.i64(2),
                                          .price_cents = lines.i64(3),
                                          .id = lines.i64(0),
                                          .converted_qty = lines.i64(4),
                                          .warehouse_id = lines.i64(6),
                                          .adjust_cents = lines.i64(7),
                                          .adjust_reason = lines.text(8),
                                          .source_order_line_id = lines.i64(5)});
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
              " h.total_amount_cents, h.status, h.order_status, h.created_at,"
              " COALESCE((SELECT SUM(converted_qty) FROM doc_line WHERE doc_id = h.id), 0),"
              " h.biz_category, h.salesperson_id, h.salesperson_name"
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
      summary.order_status = stmt.text(8);
      summary.created_at = stmt.text(9);
      summary.converted_qty = stmt.i64(10);
      summary.biz_category = stmt.text(11);
      summary.salesperson_id = stmt.i64(12);
      summary.salesperson_name = stmt.text(13);
      documents.push_back(std::move(summary));
    }
  });
  return documents;
}

void DocumentEngine::reverse(std::int64_t doc_id) {
  db_.write([&](sqlite3* conn) {
    std::string doc_type;
    std::string status;
    std::string doc_no;
    {
      Stmt header(conn,
                  "SELECT doc_type, status, doc_no FROM doc_header WHERE id = ?;");
      header.bind(1, doc_id);
      if (!header.step()) {
        throw AppError("NOT_FOUND", "单据不存在");
      }
      doc_type = header.text(0);
      status = header.text(1);
      doc_no = header.text(2);
    }
    if (status != "APPROVED") {
      throw AppError("INVALID_STATUS", "只有已审核的单据可以反审核");
    }
    const DocumentDefinition* reversed_definition = find_document_definition(doc_type);
    if (reversed_definition != nullptr && reversed_definition->role == DocumentRole::Order) {
      throw AppError("INVALID_STATUS", "订单不支持反审核，请使用关闭");
    }
    void_ar_ap_entry(conn, doc_id);

    struct LedgerRow {
      std::int64_t line_id;
      std::int64_t item_id;
      std::int64_t qty;
      std::int64_t amount;
      std::int64_t warehouse_id;
      std::string biz_type;
    };
    std::vector<LedgerRow> rows;
    {
      Stmt ledger(conn,
                  "SELECT doc_line_id, item_id, qty, amount_cents, warehouse_id, biz_type"
                  " FROM stock_ledger WHERE doc_id = ? AND biz_type IN (?, ? || '_IN', ? || '_OUT');");
      ledger.bind(1, doc_id).bind(2, doc_type).bind(3, doc_type).bind(4, doc_type);
      while (ledger.step()) {
        rows.push_back(LedgerRow{ledger.i64(0), ledger.i64(1), ledger.i64(2), ledger.i64(3),
                                 ledger.i64(4), ledger.text(5)});
      }
    }

    if (doc_type == "CST_ADJ") {
      // 反审核会把调整金额反向扣除；若后续业务已把这份金额结转出去，
      // 直接回退会让结存金额变负，必须拒绝并提示先处理后续业务。
      std::map<std::pair<std::int64_t, std::int64_t>, std::int64_t> required;
      for (const LedgerRow& row : rows) {
        if (row.amount > 0) {
          required[{row.warehouse_id, row.item_id}] += row.amount;
        }
      }
      for (const auto& [key, amount] : required) {
        const BalanceState balance = read_balance(conn, key.first, key.second);
        if (balance.amount - amount < 0) {
          throw AppError("REVERSE_WOULD_NEGATIVE",
                         "反审核会使结存金额为负，请先处理后续出库或成本调整");
        }
      }
    }

    // 入库类单据（采购入库、销售退货、其他入库、调拨入库腿、盘盈）反审核会扣回库存。
    // 如果这批货已被后续出库消耗，直接回退会把结存扣成负数，必须在写流水前拒绝。
    {
      std::map<std::pair<std::int64_t, std::int64_t>, std::int64_t> inbound;
      for (const LedgerRow& row : rows) {
        if (row.qty > 0) inbound[{row.warehouse_id, row.item_id}] += row.qty;
      }
      for (const auto& [key, qty] : inbound) {
        const BalanceState balance = read_balance(conn, key.first, key.second);
        if (balance.qty - qty < 0) {
          std::string code;
          std::string name;
          Stmt item(conn, "SELECT code, name FROM item WHERE id = ?;");
          item.bind(1, key.second);
          if (item.step()) {
            code = item.text(0);
            name = item.text(1);
          }
          throw AppError("REVERSE_WOULD_NEGATIVE",
                         "反审核会使结存数量为负：" + code + " " + name + " 当前结存 " +
                             qty_text(balance.qty) + "，需撤回 " + qty_text(qty) +
                             "；请先处理后续出库");
        }
      }
    }

    for (const LedgerRow& row : rows) {
      // 单腿单据沿用 REVERSE；调拨的入库腿追加独立类型，避免同一明细行的唯一索引冲突。
      const std::string reverse_type =
          row.biz_type == doc_type ? "REVERSE" : row.biz_type + "_REVERSE";
      Stmt ledger(conn,
                  "INSERT INTO stock_ledger (id, doc_id, doc_line_id, biz_type, biz_time,"
                  " warehouse_id, item_id, qty, amount_cents, created_at)"
                  " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
      ledger.bind(1, next_id())
          .bind(2, doc_id)
          .bind(3, row.line_id)
          .bind(4, reverse_type)
          .bind(5, now_text())
          .bind(6, row.warehouse_id)
          .bind(7, row.item_id)
          .bind(8, -row.qty)
          .bind(9, -row.amount)
          .bind(10, now_text())
          .run();

      apply_balance_delta(conn, row.warehouse_id, row.item_id, -row.qty, -row.amount);
    }

    Stmt update(conn, "UPDATE doc_header SET status = 'VOIDED' WHERE id = ?;");
    update.bind(1, doc_id).run();
    release_order_conversions(conn, doc_id);

    if (const DocumentDefinition* definition = find_document_definition(doc_type)) {
      if (definition->on_reverse) {
        definition->on_reverse(conn, doc_id);
      }
    }
    audit_write(conn, "reverse", "document", doc_id, doc_no);
  });
}

OrderConversion DocumentEngine::convert_order(std::int64_t order_id,
                                              const OrderConvertInput& input) {
  if (input.lines.empty()) {
    throw AppError("INVALID_INPUT", "请选择要下推的订单行");
  }

  OrderConversion result;
  db_.write([&](sqlite3* conn) {
    std::string doc_type;
    std::string status;
    std::string order_status;
    std::string order_no;
    std::string partner_name;
    std::int64_t warehouse_id = 0;
    std::int64_t customer_id = 0;
    std::int64_t supplier_id = 0;
    std::int64_t salesperson_id = 0;
    {
      Stmt header(conn,
                  "SELECT doc_type, status, order_status, doc_no, warehouse_id, customer_id,"
                  " partner_id, customer, salesperson_id FROM doc_header WHERE id = ?;");
      header.bind(1, order_id);
      if (!header.step()) {
        throw AppError("NOT_FOUND", "订单不存在");
      }
      doc_type = header.text(0);
      status = header.text(1);
      order_status = header.text(2);
      order_no = header.text(3);
      warehouse_id = header.i64(4);
      customer_id = header.i64(5);
      supplier_id = header.i64(6);
      partner_name = header.text(7);
      salesperson_id = header.i64(8);
    }
    const DocumentDefinition* order_definition = find_document_definition(doc_type);
    if (order_definition == nullptr || order_definition->role != DocumentRole::Order) {
      throw AppError("INVALID_INPUT", "该单据不是订单");
    }
    if (order_definition->order_converts_to.empty()) {
      throw AppError("INVALID_INPUT", "该订单未配置下推目标");
    }
    if (status != "APPROVED") {
      throw AppError("INVALID_STATUS", "只有已确认的订单可以下推");
    }
    if (order_status == "CONVERTED") {
      throw AppError("INVALID_STATUS", "订单已全部下推");
    }
    if (order_status == "CLOSED") {
      throw AppError("INVALID_STATUS", "订单已关闭，不能再下推");
    }
    if (order_status != "CONFIRMED") {
      throw AppError("INVALID_STATUS", "请先确认订单");
    }

    struct OrderLine {
      std::int64_t id;
      std::int64_t item_id;
      std::int64_t qty;
      std::int64_t price_cents;
      std::int64_t converted_qty;
    };
    std::vector<OrderLine> order_lines;
    {
      Stmt lines(conn,
                 "SELECT id, item_id, qty, price_cents, converted_qty FROM doc_line"
                 " WHERE doc_id = ? ORDER BY line_no;");
      lines.bind(1, order_id);
      while (lines.step()) {
        order_lines.push_back(OrderLine{lines.i64(0), lines.i64(1), lines.i64(2), lines.i64(3),
                                        lines.i64(4)});
      }
    }

    const DocumentDefinition* downstream_definition =
        find_document_definition(order_definition->order_converts_to);
    if (downstream_definition == nullptr) {
      throw AppError("UNKNOWN_DOC_TYPE",
                     "下推目标单据定义缺失: " + order_definition->order_converts_to);
    }

    DocInput downstream;
    downstream.doc_type = downstream_definition->doc_type;
    downstream.warehouse_id = input.warehouse_id > 0 ? input.warehouse_id : warehouse_id;
    downstream.doc_date = input.doc_date;
    // 销售订单的往来方在 customer_id，采购订单在 partner_id；`customer` 列统一存名称快照。
    downstream.partner_id = order_definition->partner_kind == PartnerKind::Customer ? customer_id
                                                                                   : supplier_id;
    downstream.partner = partner_name;
    // 下推出的销售出库沿用订单上的业务员，避免业绩归属丢失。
    downstream.salesperson_id = salesperson_id;

    std::vector<std::pair<std::int64_t, std::int64_t>> allocations;
    std::int64_t converted_total = 0;
    for (const OrderConvertLine& allocation : input.lines) {
      const auto found = std::find_if(order_lines.begin(), order_lines.end(),
                                      [&](const OrderLine& line) {
                                        return line.id == allocation.order_line_id;
                                      });
      if (found == order_lines.end()) {
        throw AppError("INVALID_INPUT", "订单行不存在：" +
                                            std::to_string(allocation.order_line_id));
      }
      if (allocation.qty <= 0) {
        throw AppError("INVALID_INPUT", "下推数量必须大于 0");
      }
      if (std::find_if(allocations.begin(), allocations.end(),
                       [&](const auto& item) {
                         return item.first == allocation.order_line_id;
                       }) != allocations.end()) {
        throw AppError("INVALID_INPUT", "同一订单行不能重复提交");
      }
      if (found->converted_qty + allocation.qty > found->qty) {
        throw AppError("ORDER_QTY_EXCEEDED",
                       "下推数量超过订单剩余数量：" + qty_text(allocation.qty) + " > " +
                           qty_text(found->qty - found->converted_qty));
      }
      downstream.lines.push_back(DocLineInput{
          .item_id = found->item_id,
          .qty = allocation.qty,
          .price_cents =
              allocation.price_cents >= 0 ? allocation.price_cents : found->price_cents,
          .source_order_line_id = found->id});
      allocations.emplace_back(allocation.order_line_id, allocation.qty);
      converted_total += allocation.qty;
    }

    const ValidatedDoc validated = validate_document(conn, downstream, *downstream_definition, true);
    result.doc_id = next_id();
    result.doc_no =
        allocate_doc_no(conn, downstream_definition->doc_no_prefix, validated.doc_date);
    insert_header(conn, result.doc_id, result.doc_no, downstream_definition->doc_type,
                  downstream_definition->partner_kind, downstream,
                  header_warehouse_id(*downstream_definition, downstream), validated, true, "");
    const std::vector<std::int64_t> line_ids =
        insert_lines(conn, result.doc_id, downstream.lines, false);
    apply_stock_moves(conn, *downstream_definition, result.doc_id, downstream.warehouse_id,
                      downstream.target_warehouse_id, validated.biz_time, downstream.lines,
                      line_ids);
    create_ar_ap_entry(conn, *downstream_definition, downstream, validated, result.doc_id,
                       result.doc_no);

    for (const auto& allocation : allocations) {
      Stmt increment(conn,
                     "UPDATE doc_line SET converted_qty = converted_qty + ? WHERE id = ?;");
      increment.bind(1, allocation.second).bind(2, allocation.first).run();
    }
    std::int64_t remaining = 0;
    {
      Stmt stmt(conn,
                "SELECT COUNT(*) FROM doc_line WHERE doc_id = ? AND converted_qty < qty;");
      stmt.bind(1, order_id);
      if (stmt.step()) remaining = stmt.i64(0);
    }
    result.order_status = remaining == 0 ? "CONVERTED" : "CONFIRMED";
    Stmt update(conn, "UPDATE doc_header SET order_status = ? WHERE id = ?;");
    update.bind(1, result.order_status).bind(2, order_id).run();

    audit_write(conn, "convert_order", "document", order_id, order_no,
                "{\"out_doc_id\":" + std::to_string(result.doc_id) + ",\"out_doc_no\":\"" +
                    result.doc_no + "\",\"qty\":\"" + qty_text(converted_total) + "\"}");
  });
  return result;
}

void DocumentEngine::close_order(std::int64_t order_id) {
  db_.write([&](sqlite3* conn) {
    std::string doc_type;
    std::string status;
    std::string order_status;
    std::string order_no;
    {
      Stmt header(conn,
                  "SELECT doc_type, status, order_status, doc_no FROM doc_header WHERE id = ?;");
      header.bind(1, order_id);
      if (!header.step()) {
        throw AppError("NOT_FOUND", "订单不存在");
      }
      doc_type = header.text(0);
      status = header.text(1);
      order_status = header.text(2);
      order_no = header.text(3);
    }
    const DocumentDefinition* definition = find_document_definition(doc_type);
    if (definition == nullptr || definition->role != DocumentRole::Order) {
      throw AppError("INVALID_INPUT", "该单据不是销售订单");
    }
    if (status != "APPROVED") {
      throw AppError("INVALID_STATUS", "只有已确认的订单可以关闭");
    }
    if (order_status == "CLOSED") {
      return;
    }
    if (order_status == "CONVERTED") {
      throw AppError("INVALID_STATUS", "订单已全部转出，无需关闭");
    }
    Stmt update(conn, "UPDATE doc_header SET order_status = 'CLOSED' WHERE id = ?;");
    update.bind(1, order_id).run();
    audit_write(conn, "close_order", "document", order_id, order_no);
  });
}

}  // namespace xiaotu
