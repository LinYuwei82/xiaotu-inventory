#pragma once

#include <cstdint>
#include <vector>

#include "xiaotu/db.h"
#include "xiaotu/models.h"

namespace xiaotu {

// 库存查询。余额表只是流水的结果缓存，异常时可按流水重算（重算任务属于 M2）。
class StockService {
 public:
  explicit StockService(Db& db) : db_(db) {}

  // warehouse_id / item_id 为 0 表示不限；hide_zero 用于隐藏数量为零的结存行。
  std::vector<StockBalanceRow> balances(std::int64_t warehouse_id = 0,
                                        std::int64_t item_id = 0,
                                        bool hide_zero = false);
  std::vector<StockLedgerRow> ledger(std::int64_t item_id, int limit = 200);

 private:
  Db& db_;
};

}  // namespace xiaotu
