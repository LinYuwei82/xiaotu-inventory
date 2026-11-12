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

namespace xiaotu {
namespace {

std::string next_settle_no(sqlite3* conn, const std::string& date) {
  std::string compact = date;
  compact.erase(std::remove(compact.begin(), compact.end(), '-'), compact.end());
  const std::string prefix = "HX" + compact + "-";
  Stmt stmt(conn,
            "SELECT settle_no FROM settlement WHERE settle_no LIKE ?"
            " ORDER BY settle_no DESC LIMIT 1;");
  stmt.bind(1, prefix + "%");
  int sequence = 1;
  if (stmt.step()) {
    const std::string last = stmt.text(0);
    const std::size_t dash = last.rfind('-');
    if (dash != std::string::npos) {
      sequence = std::stoi(last.substr(dash + 1)) + 1;
    }
  }
  std::string number = std::to_string(sequence);
  while (number.size() < 4) {
    number.insert(number.begin(), '0');
  }
  return prefix + number;
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

SettlementRow SettlementService::create(int direction, std::string_view partner_key,
                                        std::int64_t amount_cents,
                                        const std::vector<AllocationInput>& allocations,
                                        std::string_view settled_on, std::string_view note) {
  if (direction != 1 && direction != -1) {
    throw AppError("INVALID_INPUT", "核销方向无效");
  }
  if (amount_cents <= 0) {
    throw AppError("INVALID_INPUT", "核销金额必须大于 0");
  }
  if (allocations.empty()) {
    throw AppError("INVALID_INPUT", "请选择要核销的单据");
  }
  std::int64_t allocated = 0;
  for (const auto& item : allocations) {
    allocated += item.amount_cents;
  }
  if (allocated != amount_cents) {
    throw AppError("INVALID_INPUT", "分摊合计必须等于核销金额");
  }

  SettlementRow created;
  db_.write([&](sqlite3* conn) {
    std::set<std::int64_t> seen;
    std::string partner_name;
    for (const auto& item : allocations) {
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
      if (entry.direction != direction || entry.partner_key != partner_key) {
        throw AppError("INVALID_INPUT", "只能核销同一往来单位、同一方向的单据");
      }
      const std::int64_t outstanding = entry.amount - entry.settled;
      const bool same_sign =
          (outstanding > 0 && item.amount_cents > 0) || (outstanding < 0 && item.amount_cents < 0);
      if (!same_sign) {
        throw AppError("SETTLE_DIRECTION", "核销金额方向与未核销余额不一致");
      }
      const std::int64_t abs_outstanding = outstanding < 0 ? -outstanding : outstanding;
      const std::int64_t abs_amount =
          item.amount_cents < 0 ? -item.amount_cents : item.amount_cents;
      if (abs_amount > abs_outstanding) {
        throw AppError("SETTLE_OVER", "核销金额超过未核销余额");
      }
      partner_name = entry.partner_name;
    }

    const std::string date = settled_on.empty() ? now_text().substr(0, 10)
                                                : std::string(settled_on);
    created.id = next_id();
    created.settle_no = next_settle_no(conn, date);
    created.direction = direction;
    created.partner_key = std::string(partner_key);
    created.partner_name = partner_name;
    created.amount_cents = amount_cents;
    created.settled_on = date;
    created.note = std::string(note);

    Stmt header(conn,
                "INSERT INTO settlement (id, settle_no, direction, partner_key, partner_name,"
                " amount_cents, settled_on, reverses_id, note, created_at, created_by)"
                " VALUES (?, ?, ?, ?, ?, ?, ?, 0, ?, ?, ?);");
    header.bind(1, created.id)
        .bind(2, created.settle_no)
        .bind(3, static_cast<std::int64_t>(direction))
        .bind(4, created.partner_key)
        .bind(5, created.partner_name)
        .bind(6, amount_cents)
        .bind(7, created.settled_on)
        .bind(8, created.note)
        .bind(9, now_text())
        .bind(10, current_user())
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
    audit_write(conn, "create", "settlement", created.id, created.settle_no,
                "{\"direction\":" + std::to_string(direction) +
                    ",\"amount_cents\":" + std::to_string(amount_cents) + "}");
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
                " settled_on, reverses_id, note FROM settlement WHERE id = ?;");
      stmt.bind(1, settlement_id);
      if (!stmt.step()) {
        throw AppError("NOT_FOUND", "核销记录不存在");
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
    }
    if (original.reverses_id != 0 || original.amount_cents < 0) {
      throw AppError("INVALID_STATUS", "撤销记录不能再次撤销");
    }
    {
      Stmt check(conn, "SELECT 1 FROM settlement WHERE reverses_id = ? LIMIT 1;");
      check.bind(1, settlement_id);
      if (check.step()) {
        throw AppError("INVALID_STATUS", "该核销记录已撤销");
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
    if (allocations.empty()) {
      throw AppError("INVALID_STATUS", "核销记录没有分摊明细");
    }

    created.id = next_id();
    created.settle_no = next_settle_no(conn, now_text().substr(0, 10));
    created.direction = original.direction;
    created.partner_key = original.partner_key;
    created.partner_name = original.partner_name;
    created.amount_cents = -original.amount_cents;
    created.settled_on = now_text().substr(0, 10);
    created.reverses_id = settlement_id;
    created.note = "撤销 " + original.settle_no;

    Stmt header(conn,
                "INSERT INTO settlement (id, settle_no, direction, partner_key, partner_name,"
                " amount_cents, settled_on, reverses_id, note, created_at, created_by)"
                " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
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
    Stmt stmt(conn,
              "SELECT id, settle_no, direction, partner_key, partner_name, amount_cents,"
              " settled_on, reverses_id, note FROM settlement"
              " WHERE (? = 0 OR direction = ?) AND (? = '' OR partner_key = ?)"
              " ORDER BY settled_on DESC, id DESC;");
    stmt.bind(1, static_cast<std::int64_t>(direction))
        .bind(2, static_cast<std::int64_t>(direction))
        .bind(3, partner_key.empty() ? std::string_view{""} : partner_key)
        .bind(4, partner_key.empty() ? std::string_view{""} : partner_key);
    while (stmt.step()) {
      rows.push_back(SettlementRow{.id = stmt.i64(0),
                                   .settle_no = stmt.text(1),
                                   .direction = static_cast<int>(stmt.i64(2)),
                                   .partner_key = stmt.text(3),
                                   .partner_name = stmt.text(4),
                                   .amount_cents = stmt.i64(5),
                                   .settled_on = stmt.text(6),
                                   .reverses_id = stmt.i64(7),
                                   .note = stmt.text(8)});
    }
  });
  return rows;
}

}  // namespace xiaotu
