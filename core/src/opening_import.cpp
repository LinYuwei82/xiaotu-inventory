#include "xiaotu/opening_import.h"

#include <sqlite3.h>

#include <cctype>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "xiaotu/audit.h"
#include "xiaotu/backup.h"
#include "xiaotu/crud.h"
#include "xiaotu/document.h"
#include "xiaotu/error.h"
#include "xiaotu/ids.h"
#include "xiaotu/masterdata_io.h"
#include "xiaotu/partner_resolve.h"
#include "xiaotu/qty.h"
#include "xiaotu/xlsx_reader.h"

namespace xiaotu {
namespace {

constexpr const char* kStockColumns[] = {"仓库编码", "商品编码", "数量", "成本单价（元）"};
constexpr const char* kReceivableColumns[] = {"客户编码", "金额（元）", "业务日期", "到期日期"};
constexpr const char* kPayableColumns[] = {"供应商编码", "金额（元）", "业务日期", "到期日期"};
constexpr const char* kFundColumns[] = {"账户编码", "期初余额（元）"};

struct WarehouseRef {
  std::int64_t id = 0;
  std::string name;
};

struct ItemRef {
  std::int64_t id = 0;
  int decimals = 4;
};

struct PartnerRef {
  std::int64_t id = 0;
  std::string name;
};

struct AccountRef {
  std::int64_t id = 0;
};

void reject(std::vector<OpeningIssue>& issues, int row, std::string column,
            std::string reason) {
  issues.push_back(OpeningIssue{row, std::move(column), std::move(reason)});
}

std::string trimmed(const std::string& value) { return trim_text(value); }

// 允许负数的十进制金额（资金账户期初可能为负），最多两位小数，不经过浮点。
bool parse_signed_money(const std::string& text, std::int64_t* cents) {
  const std::string value = trimmed(text);
  if (value.empty()) return false;
  std::size_t index = 0;
  bool negative = false;
  if (value[index] == '-') {
    negative = true;
    ++index;
  } else if (value[index] == '+') {
    ++index;
  }
  std::string whole;
  std::string fraction;
  bool dot = false;
  for (; index < value.size(); ++index) {
    const char ch = value[index];
    if (ch == '.') {
      if (dot) return false;
      dot = true;
      continue;
    }
    if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
    (dot ? fraction : whole).push_back(ch);
  }
  if (whole.empty() && fraction.empty()) return false;
  if (fraction.size() > 2) return false;
  while (fraction.size() < 2) fraction.push_back('0');
  try {
    const std::int64_t amount = (whole.empty() ? 0 : std::stoll(whole)) * 100 +
                                std::stoll(fraction);
    *cents = negative ? -amount : amount;
    return true;
  } catch (...) {
    return false;
  }
}

bool valid_date(const std::string& text) {
  if (text.size() != 10 || text[4] != '-' || text[7] != '-') return false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (i == 4 || i == 7) continue;
    if (!std::isdigit(static_cast<unsigned char>(text[i]))) return false;
  }
  const int month = std::stoi(text.substr(5, 2));
  const int day = std::stoi(text.substr(8, 2));
  return month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

// 单据日期取导入当天；时间统一走 audit_stamp 的本地时间。
std::string today_text() { return audit_stamp().at.substr(0, 10); }

std::unordered_map<std::string, WarehouseRef> load_warehouses(sqlite3* conn) {
  std::unordered_map<std::string, WarehouseRef> result;
  Stmt stmt(conn, "SELECT id, code, name FROM warehouse WHERE is_deleted = 0;");
  while (stmt.step()) result.emplace(stmt.text(1), WarehouseRef{stmt.i64(0), stmt.text(2)});
  return result;
}

std::unordered_map<std::string, ItemRef> load_items(sqlite3* conn) {
  std::unordered_map<std::string, ItemRef> result;
  Stmt stmt(conn,
            "SELECT i.id, i.code, COALESCE(u.qty_decimals, 4) FROM item i"
            " LEFT JOIN unit u ON u.id = i.unit_id WHERE i.is_deleted = 0;");
  while (stmt.step()) {
    result.emplace(stmt.text(1), ItemRef{stmt.i64(0), static_cast<int>(stmt.i64(2))});
  }
  return result;
}

std::unordered_map<std::string, PartnerRef> load_partners(sqlite3* conn, const char* table) {
  std::unordered_map<std::string, PartnerRef> result;
  Stmt stmt(conn, "SELECT id, code, name FROM " + std::string(table) + " WHERE is_deleted = 0;");
  while (stmt.step()) result.emplace(stmt.text(1), PartnerRef{stmt.i64(0), stmt.text(2)});
  return result;
}

std::unordered_map<std::string, AccountRef> load_accounts(sqlite3* conn) {
  std::unordered_map<std::string, AccountRef> result;
  Stmt stmt(conn, "SELECT id, code FROM fund_account WHERE is_deleted = 0;");
  while (stmt.step()) result.emplace(stmt.text(1), AccountRef{stmt.i64(0)});
  return result;
}

std::int64_t scalar_count(sqlite3* conn, const std::string& sql) {
  Stmt stmt(conn, sql);
  return stmt.step() ? stmt.i64(0) : 0;
}

// 有业务流水之后拒绝导入期初：返回空串表示可导入，否则返回判定依据。
// 期初自身生成的单据/台账不算业务流水，否则四类期初无法依次导入。
std::string business_blocker(sqlite3* conn, OpeningKind kind) {
  constexpr const char* kBusinessDocs =
      "SELECT COUNT(*) FROM doc_header WHERE status = 'APPROVED'"
      " AND NOT (doc_type = 'OTH_IN' AND biz_category = 'OPENING');";
  if (kind == OpeningKind::Stock) {
    if (scalar_count(conn, "SELECT COUNT(*) FROM stock_ledger;") > 0 ||
        scalar_count(conn, kBusinessDocs) > 0) {
      return "已有库存流水或已审核单据，不能导入库存期初";
    }
    return {};
  }
  if (kind == OpeningKind::Fund) {
    if (scalar_count(conn, "SELECT COUNT(*) FROM settlement;") > 0 ||
        scalar_count(conn,
                     "SELECT COUNT(*) FROM audit_log WHERE action = 'import'"
                     " AND entity = 'opening_fund';") > 0) {
      return "已有收付款记录或已导入过资金账户期初，不能重复导入";
    }
    return {};
  }
  const std::int64_t direction = kind == OpeningKind::Receivable ? 1 : -1;
  if (scalar_count(conn, "SELECT COUNT(*) FROM ar_ap_entry WHERE opening = 1 AND direction = " +
                             std::to_string(direction) + ";") > 0 ||
      scalar_count(conn, "SELECT COUNT(*) FROM ar_ap_entry WHERE opening = 0;") > 0 ||
      scalar_count(conn, kBusinessDocs) > 0) {
    return "已有应收应付台账或已审核单据，不能导入应收应付期初";
  }
  return {};
}

std::string backup_before_import(Db& db, OpeningKind kind,
                                 const std::filesystem::path& backup_dir) {
  try {
    const std::filesystem::path directory =
        backup_dir.empty() ? db.file().parent_path() / "backups" : backup_dir;
    return backup_database(db, directory, "pre-opening-" + opening_key(kind))
        .path.filename()
        .string();
  } catch (const AppError& error) {
    if (error.code() == "BACKUP_DIR_UNWRITABLE") {
      throw AppError("BACKUP_DIR_UNWRITABLE",
                     "导入前自动备份失败，已中止：" + std::string(error.what()));
    }
    throw AppError("BACKUP_FAILED", "导入前自动备份失败，已中止：" + std::string(error.what()));
  }
}

OpeningReport import_stock(Db& db, const SheetTable& table, const ColumnMap& columns,
                           std::string_view filename,
                           const std::filesystem::path& backup_dir) {
  OpeningReport report;
  std::vector<OpeningIssue> issues;
  std::vector<std::pair<std::int64_t, std::vector<DocLineInput>>> batches;
  std::unordered_map<std::string, int> seen;
  db.read([&](sqlite3* conn) {
    const std::string blocker = business_blocker(conn, OpeningKind::Stock);
    if (!blocker.empty()) {
      reject(issues, 1, "库存期初", blocker + "；如需重来请清空数据目录重建后再导入");
      return;
    }
    const auto warehouses = load_warehouses(conn);
    const auto items = load_items(conn);
    for (const auto& sheet_row : table.rows) {
      const std::string warehouse_code = trimmed(columns.cell(sheet_row, "仓库编码"));
      const std::string item_code = trimmed(columns.cell(sheet_row, "商品编码"));
      const std::string qty_text = trimmed(columns.cell(sheet_row, "数量"));
      const std::string price_text = trimmed(columns.cell(sheet_row, "成本单价（元）"));
      const auto warehouse = warehouses.find(warehouse_code);
      const auto item = items.find(item_code);
      if (warehouse == warehouses.end()) {
        reject(issues, sheet_row.number, "仓库编码", "仓库不存在或已删除：" + warehouse_code);
        continue;
      }
      if (item == items.end()) {
        reject(issues, sheet_row.number, "商品编码", "商品不存在或已删除：" + item_code);
        continue;
      }
      const auto qty = parse_qty(qty_text);
      if (!qty.has_value() || *qty <= 0) {
        reject(issues, sheet_row.number, "数量", "应为大于 0 的数量，最多 4 位小数");
        continue;
      }
      if (!qty_fits_decimals(*qty, item->second.decimals)) {
        reject(issues, sheet_row.number, "数量", "数量精度超出该商品计量单位的小数位");
        continue;
      }
      if (price_text.empty()) {
        reject(issues, sheet_row.number, "成本单价（元）", "成本单价必填，不能用 0 代替");
        continue;
      }
      std::int64_t price = 0;
      if (!parse_signed_money(price_text, &price) || price < 0) {
        reject(issues, sheet_row.number, "成本单价（元）", "应为不小于 0 的金额，最多两位小数");
        continue;
      }
      const std::string key = std::to_string(warehouse->second.id) + ":" + item_code;
      if (const auto duplicate = seen.find(key); duplicate != seen.end()) {
        reject(issues, sheet_row.number, "商品编码",
               "同一仓库同一商品与第 " + std::to_string(duplicate->second) + " 行重复");
        continue;
      }
      seen.emplace(key, sheet_row.number);
      bool placed = false;
      for (auto& batch : batches) {
        if (batch.first == warehouse->second.id) {
          batch.second.push_back(
              DocLineInput{.item_id = item->second.id, .qty = *qty, .price_cents = price});
          placed = true;
          break;
        }
      }
      if (!placed) {
        batches.push_back({warehouse->second.id,
                           {DocLineInput{.item_id = item->second.id,
                                         .qty = *qty,
                                         .price_cents = price}}});
      }
    }
  });
  if (!issues.empty()) {
    report.issues = std::move(issues);
    return report;
  }

  report.backup = backup_before_import(db, OpeningKind::Stock, backup_dir);
  const std::string doc_date = today_text();
  DocumentEngine engine(db);
  for (const auto& batch : batches) {
    DocInput input;
    input.doc_type = "OTH_IN";
    input.warehouse_id = batch.first;
    input.doc_date = doc_date;
    input.partner = "期初导入";
    input.biz_category = "OPENING";
    input.lines = batch.second;
    const auto created = engine.save_and_approve(input);
    report.doc_nos.push_back(created.doc_no);
    report.inserted += static_cast<int>(batch.second.size());
  }
  {
    const std::string name = filename.empty() ? "库存期初.xlsx" : std::string(filename);
    db.write([&](sqlite3* conn) {
      audit_write(conn, "import", "opening_stock", 0, name,
                  "库存期初导入 " + std::to_string(report.inserted) + " 行，生成 " +
                      std::to_string(report.doc_nos.size()) + " 张期初入库单");
    });
  }
  return report;
}

OpeningReport import_receivable_payable(Db& db, OpeningKind kind, const SheetTable& table,
                                        const ColumnMap& columns, std::string_view filename,
                                        const std::filesystem::path& backup_dir) {
  OpeningReport report;
  const bool receivable = kind == OpeningKind::Receivable;
  const char* code_column = receivable ? "客户编码" : "供应商编码";
  std::vector<OpeningIssue> issues;
  struct Entry {
    std::int64_t partner_id = 0;
    std::string partner_name;
    std::int64_t amount_cents = 0;
    std::string biz_date;
    std::string due_date;
  };
  std::vector<Entry> entries;
  db.read([&](sqlite3* conn) {
    const std::string blocker = business_blocker(conn, kind);
    if (!blocker.empty()) {
      reject(issues, 1, "期初", blocker + "；如需重来请清空数据目录重建后再导入");
      return;
    }
    const auto partners = load_partners(conn, receivable ? "customer" : "supplier");
    for (const auto& sheet_row : table.rows) {
      const std::string code = trimmed(columns.cell(sheet_row, code_column));
      const std::string amount_text = trimmed(columns.cell(sheet_row, "金额（元）"));
      const std::string biz_date = trimmed(columns.cell(sheet_row, "业务日期"));
      const std::string due_date = trimmed(columns.cell(sheet_row, "到期日期"));
      const auto partner = partners.find(code);
      if (partner == partners.end()) {
        reject(issues, sheet_row.number, code_column,
               std::string(receivable ? "客户" : "供应商") + "不存在或已删除：" + code);
        continue;
      }
      std::int64_t amount = 0;
      if (!parse_signed_money(amount_text, &amount) || amount <= 0) {
        reject(issues, sheet_row.number, "金额（元）", "应为大于 0 的金额，最多两位小数");
        continue;
      }
      if (!valid_date(biz_date)) {
        reject(issues, sheet_row.number, "业务日期", "应为 YYYY-MM-DD");
        continue;
      }
      if (!due_date.empty() && !valid_date(due_date)) {
        reject(issues, sheet_row.number, "到期日期", "应为 YYYY-MM-DD，可留空");
        continue;
      }
      entries.push_back(Entry{partner->second.id, partner->second.name, amount, biz_date,
                              due_date});
    }
  });
  if (!issues.empty()) {
    report.issues = std::move(issues);
    return report;
  }

  report.backup = backup_before_import(db, kind, backup_dir);
  const std::string name = filename.empty() ? "期初.xlsx" : std::string(filename);
  db.write([&](sqlite3* conn) {
    const AuditStamp stamp = audit_stamp();
    for (const auto& entry : entries) {
      const std::int64_t id = next_id();
      const std::string doc_no = std::string(receivable ? "期初应收-" : "期初应付-") +
                                 std::to_string(id);
      Stmt stmt(conn,
                "INSERT INTO ar_ap_entry (id, doc_id, doc_no, doc_type, direction, partner_id,"
                " partner_name, partner_key, biz_date, due_date, amount_cents, settled_cents,"
                " status, created_at, created_by, opening)"
                " VALUES (?, 0, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0, 'OPEN', ?, ?, 1);");
      stmt.bind(1, id)
          .bind(2, doc_no)
          .bind(3, receivable ? "OPENING_AR" : "OPENING_AP")
          .bind(4, static_cast<std::int64_t>(receivable ? 1 : -1))
          .bind(5, entry.partner_id)
          .bind(6, entry.partner_name)
          .bind(7, partner_key_for_archive(
                       conn, receivable ? PartnerSide::Customer : PartnerSide::Supplier,
                       entry.partner_id, entry.partner_name, true))
          .bind(8, entry.biz_date)
          .bind(9, entry.due_date)
          .bind(10, entry.amount_cents)
          .bind(11, stamp.at)
          .bind(12, stamp.by)
          .run();
      ++report.inserted;
    }
    audit_write(conn, "import", receivable ? "opening_receivable" : "opening_payable", 0, name,
                (receivable ? "应收" : "应付") + std::string("期初导入 ") +
                    std::to_string(report.inserted) + " 条");
  });
  return report;
}

OpeningReport import_fund(Db& db, const SheetTable& table, const ColumnMap& columns,
                          std::string_view filename,
                          const std::filesystem::path& backup_dir) {
  OpeningReport report;
  std::vector<OpeningIssue> issues;
  std::vector<std::pair<std::int64_t, std::int64_t>> updates;  // id -> cents
  std::unordered_map<std::string, int> seen;
  db.read([&](sqlite3* conn) {
    const std::string blocker = business_blocker(conn, OpeningKind::Fund);
    if (!blocker.empty()) {
      reject(issues, 1, "资金账户期初", blocker + "；如需重来请清空数据目录重建后再导入");
      return;
    }
    const auto accounts = load_accounts(conn);
    for (const auto& sheet_row : table.rows) {
      const std::string code = trimmed(columns.cell(sheet_row, "账户编码"));
      const std::string amount_text = trimmed(columns.cell(sheet_row, "期初余额（元）"));
      const auto account = accounts.find(code);
      if (account == accounts.end()) {
        reject(issues, sheet_row.number, "账户编码", "资金账户不存在或已删除：" + code);
        continue;
      }
      std::int64_t amount = 0;
      if (!parse_signed_money(amount_text, &amount)) {
        reject(issues, sheet_row.number, "期初余额（元）", "应为金额，最多两位小数，可为负");
        continue;
      }
      if (const auto duplicate = seen.find(code); duplicate != seen.end()) {
        reject(issues, sheet_row.number, "账户编码",
               "与第 " + std::to_string(duplicate->second) + " 行重复");
        continue;
      }
      seen.emplace(code, sheet_row.number);
      updates.push_back({account->second.id, amount});
    }
  });
  if (!issues.empty()) {
    report.issues = std::move(issues);
    return report;
  }

  report.backup = backup_before_import(db, OpeningKind::Fund, backup_dir);
  const std::string name = filename.empty() ? "资金账户期初.xlsx" : std::string(filename);
  db.write([&](sqlite3* conn) {
    const AuditStamp stamp = audit_stamp();
    for (const auto& update : updates) {
      Stmt stmt(conn,
                "UPDATE fund_account SET opening_cents = ?, updated_at = ?, updated_by = ?"
                " WHERE id = ? AND is_deleted = 0;");
      stmt.bind(1, update.second)
          .bind(2, stamp.at)
          .bind(3, stamp.by)
          .bind(4, update.first)
          .run();
      ++report.inserted;
    }
    audit_write(conn, "import", "opening_fund", 0, name,
                "资金账户期初导入 " + std::to_string(report.inserted) + " 个账户");
  });
  return report;
}

}  // namespace

std::string opening_key(OpeningKind kind) {
  switch (kind) {
    case OpeningKind::Stock:
      return "stock";
    case OpeningKind::Receivable:
      return "receivable";
    case OpeningKind::Payable:
      return "payable";
    case OpeningKind::Fund:
      return "fund";
  }
  return "stock";
}

std::string opening_label(OpeningKind kind) {
  switch (kind) {
    case OpeningKind::Stock:
      return "库存期初";
    case OpeningKind::Receivable:
      return "应收期初";
    case OpeningKind::Payable:
      return "应付期初";
    case OpeningKind::Fund:
      return "资金账户期初";
  }
  return "期初";
}

std::vector<std::string> opening_columns(OpeningKind kind) {
  switch (kind) {
    case OpeningKind::Stock:
      return std::vector<std::string>(std::begin(kStockColumns), std::end(kStockColumns));
    case OpeningKind::Receivable:
      return std::vector<std::string>(std::begin(kReceivableColumns),
                                     std::end(kReceivableColumns));
    case OpeningKind::Payable:
      return std::vector<std::string>(std::begin(kPayableColumns), std::end(kPayableColumns));
    case OpeningKind::Fund:
      return std::vector<std::string>(std::begin(kFundColumns), std::end(kFundColumns));
  }
  return {};
}

std::string opening_blocker(Db& db, OpeningKind kind) {
  std::string reason;
  db.read([&](sqlite3* conn) { reason = business_blocker(conn, kind); });
  return reason;
}

OpeningReport import_opening(Db& db, OpeningKind kind, std::string_view bytes,
                             std::string_view filename,
                             const std::filesystem::path& backup_dir) {
  if (bytes.empty()) throw AppError("IMPORT_INVALID", "请选择要导入的 .xlsx 文件");
  if (bytes.size() > kImportMaxBytes) {
    throw AppError("IMPORT_TOO_LARGE", "文件超过 5MB 上限，请分批导入");
  }
  const SheetTable table = read_first_sheet(bytes);
  if (static_cast<int>(table.rows.size()) > kImportMaxRows) {
    throw AppError("IMPORT_TOO_LARGE", "单次最多导入 5000 行，请分批导入");
  }
  const std::vector<std::string> known = opening_columns(kind);
  std::vector<std::string> required = known;
  if (kind == OpeningKind::Receivable || kind == OpeningKind::Payable) {
    required = {known[0], known[1], known[2]};  // 到期日期可留空
  }
  const ColumnMap columns = map_columns(table.header, known, required);
  switch (kind) {
    case OpeningKind::Stock:
      return import_stock(db, table, columns, filename, backup_dir);
    case OpeningKind::Receivable:
    case OpeningKind::Payable:
      return import_receivable_payable(db, kind, table, columns, filename, backup_dir);
    case OpeningKind::Fund:
      return import_fund(db, table, columns, filename, backup_dir);
  }
  return {};
}

}  // namespace xiaotu
