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
};

// 核销：只追加。撤销生成负数记录与负数分摊，不物理删除。
// 一次核销可覆盖多张单据，但必须属于同一往来单位、同一方向。
class SettlementService {
 public:
  explicit SettlementService(Db& db) : db_(db) {}

  SettlementRow create(int direction, std::string_view partner_key, std::int64_t amount_cents,
                       const std::vector<AllocationInput>& allocations,
                       std::string_view settled_on, std::string_view note);

  SettlementRow reverse(std::int64_t settlement_id);

  std::vector<SettlementRow> list(int direction, std::string_view partner_key = {}) const;

 private:
  Db& db_;
};

}  // namespace xiaotu
