#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "xiaotu/db.h"

namespace xiaotu {

struct CostRecalcRow {
  std::int64_t item_id = 0;
  std::string item_code;
  std::string item_name;
  std::int64_t warehouse_id = 0;
  std::string warehouse_name;
  std::int64_t book_qty = 0;
  std::int64_t book_amount_cents = 0;
  std::int64_t replay_qty = 0;
  std::int64_t replay_amount_cents = 0;
  std::int64_t qty_diff = 0;
  std::int64_t amount_diff_cents = 0;
};

struct CostRecalcResult {
  std::int64_t ledger_rows = 0;
  std::int64_t item_count = 0;
  std::int64_t difference_count = 0;
  std::int64_t qty_difference_count = 0;
  std::vector<CostRecalcRow> differences;
};

// 成本重算（只读）：按库存流水重放移动加权平均，重算结存数量与金额，
// 与 stock_balance 当前账面值核对并输出差异清单。
// 第一版不写回任何数据；写回策略需另行确认。
class CostRecalcService {
 public:
  explicit CostRecalcService(Db& db) : db_(db) {}

  // item_id / warehouse_id 为 0 表示不筛选。
  CostRecalcResult recalculate(std::int64_t item_id = 0,
                               std::int64_t warehouse_id = 0) const;

 private:
  Db& db_;
};

}  // namespace xiaotu
