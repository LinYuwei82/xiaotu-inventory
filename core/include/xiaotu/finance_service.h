#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "xiaotu/db.h"

namespace xiaotu {

struct ArApRow {
  std::int64_t id = 0;
  std::int64_t doc_id = 0;
  std::string doc_no;
  std::string doc_type;
  int direction = 0;  // 1 应收 / -1 应付
  std::int64_t partner_id = 0;
  std::string partner_name;
  std::string partner_key;
  std::string biz_date;
  std::string due_date;
  std::int64_t amount_cents = 0;
  std::int64_t settled_cents = 0;
  std::int64_t outstanding_cents = 0;
  std::string status;
};

// 应收应付台账查询。写入发生在单据审核/反审核事务内，这里只读。
class FinanceService {
 public:
  explicit FinanceService(Db& db) : db_(db) {}

  // direction: 1 应收，-1 应付，0 全部；status 为空表示全部状态。
  std::vector<ArApRow> entries(int direction, std::string_view partner_key = {},
                               std::string_view status = {}) const;

 private:
  Db& db_;
};

}  // namespace xiaotu
