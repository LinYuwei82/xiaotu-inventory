#include "xiaotu/settlement_service.h"

#include <sqlite3.h>

#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "xiaotu/audit.h"
#include "xiaotu/context.h"
#include "xiaotu/error.h"
#include "xiaotu/ids.h"
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

std::string next_settle_no(sqlite3* conn, const std::string& date, int direction) {
  std::string compact = date;
  compact.erase(std::remove(compact.begin(), compact.end(), '-'), compact.end());
  // 收款 SK、付款 FK；老单号 HX 原样保留，互不影响。
  const std::string prefix = std::string(direction == 1 ? "SK" : "FK") + compact + "-";
  Stmt stmt(conn,
            "SELECT settle_no FROM settlement WHERE settle_no LIKE ?"
            " ORDER BY settle_no DESC LIMIT 1;");
  stmt.bind(1, prefix + "%");
  int sequence = 1;
  if (stmt.step()) {
    const std::string last = stmt.text(0);
    const std::size_t dash = last.rfind('-');
    if (dash != std::string::npos) {
      // 脏单号（后缀非数字或溢出）不能让 std::stoi 抛异常变成 500，回落从 1 重新计数。
      const std::string suffix = last.substr(dash + 1);
      bool digits = !suffix.empty() && suffix.size() <= 9;
      for (char c : suffix) {
        if (c < '0' || c > '9') {
          digits = false;
          break;
        }
      }
      if (digits) {
        sequence = std::stoi(suffix) + 1;
      }
    }
  }
  std::string number = std::to_string(sequence);
  while (number.size() < 4) {
    number.insert(number.begin(), '0');
  }
  return prefix + number;
}

bool valid_method(const std::string& method) {
  return method == "CASH" || method == "BANK" || method == "WECHAT" ||
         method == "ALIPAY" || method == "OTHER";
}

void ensure_account(sqlite3* conn, std::int64_t account_id) {
  if (account_id <= 0) {
    throw AppError("INVALID_INPUT", "请选择资金账户");
  }
  Stmt stmt(conn,
            "SELECT 1 FROM fund_account WHERE id = ? AND is_deleted = 0 AND status = 1;");
  stmt.bind(1, account_id);
  if (!stmt.step()) {
    throw AppError("INVALID_INPUT", "资金账户不存在或已停用");
  }
}

// C:<id> 客户、S:<id> 供应商、P:<id> 合表档案；T: 文本键没有档案，返回空由调用方回落到单据名称。
std::string partner_name_of(sqlite3* conn, const std::string& key) {  try {
    if (key.rfind("C:", 0) == 0) {
      Stmt stmt(conn, "SELECT name FROM customer WHERE id = ? AND is_deleted = 0;");
      stmt.bind(1, std::stoll(key.substr(2)));
      return stmt.step() ? stmt.text(0) : "";
    }
    if (key.rfind("S:", 0) == 0) {
      Stmt stmt(conn, "SELECT name FROM supplier WHERE id = ? AND is_deleted = 0;");
      stmt.bind(1, std::stoll(key.substr(2)));
      return stmt.step() ? stmt.text(0) : "";
    }
    if (key.rfind("P:", 0) == 0) {
      Stmt stmt(conn, "SELECT name FROM partner WHERE id = ? AND is_deleted = 0;");
      stmt.bind(1, std::stoll(key.substr(2)));
      return stmt.step() ? stmt.text(0) : "";
    }
  } catch (const std::exception&) {
    return "";
  }
  return "";
}

struct EntryState {
  std::int64_t id = 0;
  std::int64_t amount = 0;
  std::int64_t settled = 0;
  std::string partner_key;
  std::string partner_name;
  int direction = 0;
  std::string status;
};

EntryState load_entry(sqlite3* conn, std::int64_t entry_id) {
  Stmt stmt(conn,
            "SELECT id, amount_cents, settled_cents, partner_key, partner_name, direction, status"
            " FROM ar_ap_entry WHERE id = ?;");
  stmt.bind(1, entry_id);
  if (!stmt.step()) {
    throw AppError("NOT_FOUND", "往来记录不存在");
  }
  EntryState state;
  state.id = stmt.i64(0);
  state.amount = stmt.i64(1);
  state.settled = stmt.i64(2);
  state.partner_key = stmt.text(3);
  state.partner_name = stmt.text(4);
  state.direction = static_cast<int>(stmt.i64(5));
  state.status = stmt.text(6);
  return state;
}

void apply_allocation(sqlite3* conn, std::int64_t entry_id, std::int64_t amount) {
  Stmt update(conn, "UPDATE ar_ap_entry SET settled_cents = settled_cents + ? WHERE id = ?;");
  update.bind(1, amount).bind(2, entry_id).run();
  // 状态由缓存金额派生：0 未核销，全额已核销，其余为部分核销。
  Stmt status(conn,
              "UPDATE ar_ap_entry SET status = CASE"
              " WHEN settled_cents = 0 THEN 'OPEN'"
              " WHEN settled_cents = amount_cents THEN 'SETTLED'"
              " ELSE 'PARTIAL' END"
              " WHERE id = ? AND status <> 'VOIDED';");
  status.bind(1, entry_id).run();
}

}  // namespace

SettlementRow SettlementService::create(const SettlementCreateInput& input) {
  if (input.direction != 1 && input.direction != -1) {
    throw AppError("INVALID_INPUT", "收付款方向无效");
  }
  if (input.amount_cents <= 0) {
    throw AppError("INVALID_INPUT", "收付款金额必须大于 0");
  }
  if (!valid_method(input.method)) {
    throw AppError("INVALID_INPUT", "结算方式无效");
  }
  std::int64_t allocated = 0;
  for (const auto& item : input.allocations) {
    allocated += item.amount_cents;
  }
  if (allocated > input.amount_cents) {
    throw AppError("INVALID_INPUT", "分摊合计不能超过收付款金额");
  }

  SettlementRow created;
  db_.write([&](sqlite3* conn) {
    ensure_account(conn, input.account_id);

    std::vector<EntryState> entries;
    std::set<std::int64_t> seen;
    for (const auto& item : input.allocations) {
      if (item.entry_id == 0 || item.amount_cents == 0) {
        throw AppError("INVALID_INPUT", "核销分摊不能为空");
      }
      if (!seen.insert(item.entry_id).second) {
        throw AppError("INVALID_INPUT", "同一张单据不能重复分摊");
      }
      const EntryState entry = load_entry(conn, item.entry_id);
      if (entry.status == "VOIDED") {
        throw AppError("ENTRY_VOIDED", "往来记录已作废，不能核销");
      }
      if (entry.direction != input.direction) {
        throw AppError("INVALID_INPUT", "只能核销同一方向的单据");
      }
      const std::int64_t outstanding = entry.amount - entry.settled;
      const bool same_sign = (outstanding > 0 && item.amount_cents > 0) ||
                             (outstanding < 0 && item.amount_cents < 0);
      if (!same_sign) {
        throw AppError("SETTLE_DIRECTION", "核销金额方向与未核销余额不一致");
      }
      const std::int64_t abs_outstanding = outstanding < 0 ? -outstanding : outstanding;
      const std::int64_t abs_amount =
          item.amount_cents < 0 ? -item.amount_cents : item.amount_cents;
      if (abs_amount > abs_outstanding) {
        throw AppError("SETTLE_OVER", "核销金额超过未核销余额");
      }
      entries.push_back(entry);
    }

    std::string partner_key = input.partner_key;
    std::string partner_name = input.partner_name;
    if (!entries.empty()) {
      if (partner_key.empty()) {
        partner_key = entries.front().partner_key;
      }
      // 历史 C:/S: 与新 P: 可能同时存在，按 partner 维度判断是否同一单位。
      const std::string dimension = partner_dimension_key(conn, partner_key);
      for (const auto& entry : entries) {
        if (partner_dimension_key(conn, entry.partner_key) != dimension) {
          throw AppError("INVALID_INPUT", "只能核销同一往来单位、同一方向的单据");
        }
      }
      if (partner_name.empty()) {
        partner_name = entries.front().partner_name;
      }
    }
    if (partner_key.empty()) {
      throw AppError("INVALID_INPUT", "请选择往来单位");
    }
    if (partner_name.empty()) {
      partner_name = partner_name_of(conn, partner_key);
    }
    if (partner_name.empty()) {
      throw AppError("NOT_FOUND", "往来单位不存在");
    }

    const std::string date =
        input.settled_on.empty() ? now_text().substr(0, 10) : input.settled_on;
    created.id = next_id();
    created.settle_no = next_settle_no(conn, date, input.direction);
    created.direction = input.direction;
    created.partner_key = partner_key;
    created.partner_name = partner_name;
    created.amount_cents = input.amount_cents;
    created.settled_on = date;
    created.note = input.note;
    created.account_id = input.account_id;
    created.method = input.method;
    created.operator_name = input.operator_name.empty() ? current_user() : input.operator_name;

    Stmt header(conn,
                "INSERT INTO settlement (id, settle_no, direction, partner_key, partner_name,"
                " amount_cents, settled_on, reverses_id, note, created_at, created_by,"
                " account_id, method, operator)"
                " VALUES (?, ?, ?, ?, ?, ?, ?, 0, ?, ?, ?, ?, ?, ?);");
    header.bind(1, created.id)
        .bind(2, created.settle_no)
        .bind(3, static_cast<std::int64_t>(created.direction))
        .bind(4, created.partner_key)
        .bind(5, created.partner_name)
        .bind(6, created.amount_cents)
        .bind(7, created.settled_on)
        .bind(8, created.note)
        .bind(9, now_text())
        .bind(10, current_user())
        .bind(11, created.account_id)
        .bind(12, created.method)
        .bind(13, created.operator_name)
        .run();

    for (const auto& item : input.allocations) {
      Stmt allocation(conn,
                      "INSERT INTO settlement_allocation (id, settlement_id, entry_id,"
                      " amount_cents, created_at) VALUES (?, ?, ?, ?, ?);");
      allocation.bind(1, next_id())
          .bind(2, created.id)
          .bind(3, item.entry_id)
          .bind(4, item.amount_cents)
          .bind(5, now_text())
          .run();
      apply_allocation(conn, item.entry_id, item.amount_cents);
    }
    audit_write(conn, "create", "settlement", created.id, created.settle_no,
                "{\"direction\":" + std::to_string(created.direction) +
                    ",\"amount_cents\":" + std::to_string(created.amount_cents) +
                    ",\"account_id\":" + std::to_string(created.account_id) + "}");
  });
  return created;
}

SettlementRow SettlementService::reverse(std::int64_t settlement_id) {
  SettlementRow created;
  db_.write([&](sqlite3* conn) {
    SettlementRow original;
    {
      Stmt stmt(conn,
                "SELECT id, settle_no, direction, partner_key, partner_name, amount_cents,"
                " settled_on, reverses_id, note, account_id, method, operator"
                " FROM settlement WHERE id = ?;");
      stmt.bind(1, settlement_id);
      if (!stmt.step()) {
        throw AppError("NOT_FOUND", "收付款记录不存在");
      }
      original.id = stmt.i64(0);
      original.settle_no = stmt.text(1);
      original.direction = static_cast<int>(stmt.i64(2));
      original.partner_key = stmt.text(3);
      original.partner_name = stmt.text(4);
      original.amount_cents = stmt.i64(5);
      original.settled_on = stmt.text(6);
      original.reverses_id = stmt.i64(7);
      original.note = stmt.text(8);
      original.account_id = stmt.i64(9);
      original.method = stmt.text(10);
      original.operator_name = stmt.text(11);
    }
    if (original.reverses_id != 0 || original.amount_cents < 0) {
      throw AppError("INVALID_STATUS", "撤销记录不能再次撤销");
    }
    {
      Stmt check(conn, "SELECT 1 FROM settlement WHERE reverses_id = ? LIMIT 1;");
      check.bind(1, settlement_id);
      if (check.step()) {
        throw AppError("INVALID_STATUS", "该收付款记录已撤销");
      }
    }

    std::vector<AllocationInput> allocations;
    {
      Stmt stmt(conn,
                "SELECT entry_id, amount_cents FROM settlement_allocation"
                " WHERE settlement_id = ?;");
      stmt.bind(1, settlement_id);
      while (stmt.step()) {
        allocations.push_back(AllocationInput{stmt.i64(0), -stmt.i64(1)});
      }
    }

    const std::string date = now_text().substr(0, 10);
    created.id = next_id();
    created.settle_no = next_settle_no(conn, date, original.direction);
    created.direction = original.direction;
    created.partner_key = original.partner_key;
    created.partner_name = original.partner_name;
    created.amount_cents = -original.amount_cents;
    created.settled_on = date;
    created.reverses_id = settlement_id;
    created.note = "撤销 " + original.settle_no;
    created.account_id = original.account_id;
    created.method = original.method;
    created.operator_name = original.operator_name.empty() ? current_user()
                                                           : original.operator_name;

    Stmt header(conn,
                "INSERT INTO settlement (id, settle_no, direction, partner_key, partner_name,"
                " amount_cents, settled_on, reverses_id, note, created_at, created_by,"
                " account_id, method, operator)"
                " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
    header.bind(1, created.id)
        .bind(2, created.settle_no)
        .bind(3, static_cast<std::int64_t>(created.direction))
        .bind(4, created.partner_key)
        .bind(5, created.partner_name)
        .bind(6, created.amount_cents)
        .bind(7, created.settled_on)
        .bind(8, created.reverses_id)
        .bind(9, created.note)
        .bind(10, now_text())
        .bind(11, current_user())
        .bind(12, created.account_id)
        .bind(13, created.method)
        .bind(14, created.operator_name)
        .run();

    for (const auto& item : allocations) {
      Stmt allocation(conn,
                      "INSERT INTO settlement_allocation (id, settlement_id, entry_id,"
                      " amount_cents, created_at) VALUES (?, ?, ?, ?, ?);");
      allocation.bind(1, next_id())
          .bind(2, created.id)
          .bind(3, item.entry_id)
          .bind(4, item.amount_cents)
          .bind(5, now_text())
          .run();
      apply_allocation(conn, item.entry_id, item.amount_cents);
    }
    audit_write(conn, "reverse", "settlement", created.id, created.settle_no,
                "{\"reverses\":\"" + original.settle_no + "\"}");
  });
  return created;
}

std::vector<SettlementRow> SettlementService::list(int direction,
                                                   std::string_view partner_key) const {
  std::vector<SettlementRow> rows;
  db_.read([&](sqlite3* conn) {
    // 按单位筛选时展开历史键：C:/S: 与新 P: 属于同一 partner 维度。
    const std::vector<std::string> aliases =
        partner_key.empty() ? std::vector<std::string>{}
                            : partner_key_aliases(conn, partner_key);
    std::string sql =
        "SELECT id, settle_no, direction, partner_key, partner_name, amount_cents,"
        " settled_on, reverses_id, note, account_id, method, operator"
        " FROM settlement"
        " WHERE (? = 0 OR direction = ?)";
    if (!aliases.empty()) sql += " AND partner_key IN (" + mark_list(aliases.size()) + ")";
    sql += " ORDER BY settled_on DESC, id DESC;";
    Stmt stmt(conn, sql);
    int index = 1;
    stmt.bind(index++, static_cast<std::int64_t>(direction))
        .bind(index++, static_cast<std::int64_t>(direction));
    for (const auto& alias : aliases) stmt.bind(index++, alias);
    while (stmt.step()) {
      rows.push_back(SettlementRow{.id = stmt.i64(0),
                                   .settle_no = stmt.text(1),
                                   .direction = static_cast<int>(stmt.i64(2)),
                                   .partner_key = stmt.text(3),
                                   .partner_name = stmt.text(4),
                                   .amount_cents = stmt.i64(5),
                                   .settled_on = stmt.text(6),
                                   .reverses_id = stmt.i64(7),
                                   .note = stmt.text(8),
                                   .account_id = stmt.i64(9),
                                   .method = stmt.text(10),
                                   .operator_name = stmt.text(11)});
    }
  });
  return rows;
}

std::vector<FundLedgerRow> SettlementService::ledger(std::int64_t account_id,
                                                     std::string_view from,
                                                     std::string_view to) const {
  std::vector<FundLedgerRow> rows;
  db_.read([&](sqlite3* conn) {
    // account_id > 0：指定账户；0：历史「未指定账户」；-1：全部账户（含未指定）。
    const bool all_accounts = account_id < 0;
    std::int64_t opening = 0;
    if (all_accounts) {
      Stmt accounts(conn, "SELECT COALESCE(SUM(opening_cents), 0) FROM fund_account"
                          " WHERE is_deleted = 0;");
      if (accounts.step()) opening = accounts.i64(0);
    } else if (account_id > 0) {
      Stmt account(conn, "SELECT opening_cents FROM fund_account WHERE id = ?;");
      account.bind(1, account_id);
      if (!account.step()) {
        throw AppError("NOT_FOUND", "资金账户不存在");
      }
      opening = account.i64(0);
    }

    const std::string start = from.empty() ? std::string("0000-01-01") : std::string(from);
    const std::string end = to.empty() ? std::string("9999-12-31") : std::string(to);

    {
      Stmt prior(conn,
                 "SELECT COALESCE(SUM(CASE WHEN direction = 1 THEN amount_cents"
                 "                         ELSE -amount_cents END), 0)"
                 " FROM settlement WHERE (? = -1 OR account_id = ?) AND settled_on < ?;");
      prior.bind(1, account_id).bind(2, account_id).bind(3, start);
      if (prior.step()) {
        opening += prior.i64(0);
      }
    }

    std::int64_t balance = opening;
    rows.push_back(FundLedgerRow{.opening = true,
                                 .settled_on = start == "0000-01-01" ? std::string{}
                                                                     : start,
                                 .note = "期初余额",
                                 .balance_cents = balance});

    Stmt stmt(conn,
              "SELECT settled_on, settle_no, partner_name, method, operator, note, direction,"
              " amount_cents FROM settlement"
              " WHERE (? = -1 OR account_id = ?)"
              "   AND settled_on >= ? AND settled_on <= ?"
              " ORDER BY settled_on, id;");
    stmt.bind(1, account_id).bind(2, account_id).bind(3, start).bind(4, end);
    while (stmt.step()) {
      FundLedgerRow row;
      row.settled_on = stmt.text(0);
      row.settle_no = stmt.text(1);
      row.partner_name = stmt.text(2);
      row.method = stmt.text(3);
      row.operator_name = stmt.text(4);
      row.note = stmt.text(5);
      row.direction = static_cast<int>(stmt.i64(6));
      const std::int64_t amount = stmt.i64(7);
      // 列名已表达方向，金额只展示绝对值；撤销行（金额为负）由备注与类型说明。
      // 余额仍按带符号金额累计，保证撤销后余额回退。
      if (row.direction == 1) {
        row.income_cents = amount < 0 ? -amount : amount;
      } else {
        row.expense_cents = amount < 0 ? -amount : amount;
      }
      balance += row.direction == 1 ? amount : -amount;
      row.balance_cents = balance;
      rows.push_back(row);
    }
  });
  return rows;
}

std::vector<FundBalanceRow> SettlementService::account_balances(
    std::string_view as_of) const {
  std::vector<FundBalanceRow> rows;
  db_.read([&](sqlite3* conn) {
    const std::string cut = as_of.empty() ? std::string("9999-12-31") : std::string(as_of);
    Stmt stmt(conn,
              "SELECT a.id, a.code, a.name, a.kind, a.opening_cents,"
              " COALESCE(SUM(CASE WHEN s.direction = 1 THEN s.amount_cents ELSE 0 END), 0),"
              " COALESCE(SUM(CASE WHEN s.direction = -1 THEN s.amount_cents ELSE 0 END), 0)"
              " FROM fund_account a"
              " LEFT JOIN settlement s ON s.account_id = a.id AND s.settled_on <= ?"
              " WHERE a.is_deleted = 0"
              " GROUP BY a.id ORDER BY a.sort_order, a.id;");
    stmt.bind(1, cut);
    while (stmt.step()) {
      FundBalanceRow row;
      row.id = stmt.i64(0);
      row.code = stmt.text(1);
      row.name = stmt.text(2);
      row.kind = stmt.text(3);
      row.opening_cents = stmt.i64(4);
      row.inflow_cents = stmt.i64(5);
      row.outflow_cents = stmt.i64(6);
      row.balance_cents = row.opening_cents + row.inflow_cents - row.outflow_cents;
      rows.push_back(row);
    }

    // 历史记录没有账户归属，单独列一组，避免金额凭空消失。
    Stmt legacy(conn,
                "SELECT COUNT(*),"
                " COALESCE(SUM(CASE WHEN direction = 1 THEN amount_cents ELSE 0 END), 0),"
                " COALESCE(SUM(CASE WHEN direction = -1 THEN amount_cents ELSE 0 END), 0)"
                " FROM settlement WHERE account_id = 0 AND settled_on <= ?;");
    legacy.bind(1, cut);
    if (legacy.step() && legacy.i64(0) > 0) {
      FundBalanceRow row;
      row.id = 0;
      row.name = "未指定账户";
      row.kind = "OTHER";
      row.inflow_cents = legacy.i64(1);
      row.outflow_cents = legacy.i64(2);
      row.balance_cents = row.inflow_cents - row.outflow_cents;
      rows.push_back(row);
    }
  });
  return rows;
}

}  // namespace xiaotu
