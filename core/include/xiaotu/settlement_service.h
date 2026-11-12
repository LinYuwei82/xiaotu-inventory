#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "xiaotu/db.h"

namespace xiaotu {

struct AllocationInput {
  std::int64_t entry_id = 0;
  std::int64_t amount_cents = 0;
};

// 收付款单入参：一张单只对一个往来单位；分摊可以为空（预收/预付），
// 也可以小于单据金额（部分核销），但合计不能超过单据金额。
struct SettlementCreateInput {
  int direction = 0;  // 1 收款，-1 付款
  std::string partner_key;
  std::string partner_name;
  std::int64_t amount_cents = 0;
  std::int64_t account_id = 0;
  std::string method;
  std::string operator_name;
  std::vector<AllocationInput> allocations;
  std::string settled_on;
  std::string note;
};

struct SettlementRow {
  std::int64_t id = 0;
  std::string settle_no;
  int direction = 0;
  std::string partner_key;
  std::string partner_name;
  std::int64_t amount_cents = 0;
  std::string settled_on;
  std::int64_t reverses_id = 0;
  std::string note;
  std::int64_t account_id = 0;
  std::string method;
  std::string operator_name;
};

// 资金流水行：期初行 opening=true，其余为收付款记录，余额逐行累计。
struct FundLedgerRow {
  bool opening = false;
  std::string settled_on;
  std::string settle_no;
  std::string partner_name;
  std::string method;
  std::string operator_name;
  std::string note;
  int direction = 0;
  std::int64_t income_cents = 0;
  std::int64_t expense_cents = 0;
  std::int64_t balance_cents = 0;
};

// 账户余额一览：期初 + 累计收入 − 累计支出。
struct FundBalanceRow {
  std::int64_t id = 0;
  std::string code;
  std::string name;
  std::string kind;
  std::int64_t opening_cents = 0;
  std::int64_t inflow_cents = 0;
  std::int64_t outflow_cents = 0;
  std::int64_t balance_cents = 0;
};

// 收付款：只追加。撤销生成负数记录与负数分摊，不物理删除。
// 一次收付款可覆盖多张单据，但必须属于同一往来单位、同一方向。
class SettlementService {
 public:
  explicit SettlementService(Db& db) : db_(db) {}

  SettlementRow create(const SettlementCreateInput& input);

  SettlementRow reverse(std::int64_t settlement_id);

  std::vector<SettlementRow> list(int direction, std::string_view partner_key = {}) const;

  // 资金流水（现金日记账）。account_id=0 表示「未指定账户」的历史记录。
  std::vector<FundLedgerRow> ledger(std::int64_t account_id, std::string_view from,
                                    std::string_view to) const;

  // 账户余额一览；as_of 为空表示截至今天。
  std::vector<FundBalanceRow> account_balances(std::string_view as_of) const;

 private:
  Db& db_;
};

}  // namespace xiaotu
