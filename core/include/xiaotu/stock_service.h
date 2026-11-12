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

  std::vector<StockBalanceRow> balances();
  std::vector<StockLedgerRow> ledger(std::int64_t item_id, int limit = 200);

 private:
  Db& db_;
};

}  // namespace xiaotu
