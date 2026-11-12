#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "xiaotu/db.h"

namespace xiaotu {

struct LedgerDetailRow {
  std::int64_t id = 0;
  std::string biz_time;
  std::string biz_type;
  std::string doc_no;
  std::string item_code;
  std::string item_name;
  std::string warehouse_name;
  std::int64_t qty = 0;
  std::int64_t amount_cents = 0;
};

struct GoodsFlowRow {
  std::int64_t item_id = 0;
  std::string item_code;
  std::string item_name;
  std::string unit;
  std::string warehouse_code;
  std::string warehouse_name;
  std::int64_t opening_qty = 0;
  std::int64_t in_qty = 0;
  std::int64_t out_qty = 0;
  std::int64_t closing_qty = 0;
  std::int64_t in_amount_cents = 0;
  std::int64_t out_amount_cents = 0;
};

// 账龄分桶：未到期、1-30、31-60、61-90、90 天以上（按 due_date 与截止日计算）。
struct AgingRow {
  std::string partner_key;
  std::string partner_name;
  int direction = 0;  // 1 应收 / -1 应付
  std::int64_t total_cents = 0;
  std::int64_t not_due_cents = 0;
  std::int64_t d1_30_cents = 0;
  std::int64_t d31_60_cents = 0;
  std::int64_t d61_90_cents = 0;
  std::int64_t d90_plus_cents = 0;
};

struct ReportFilter {
  std::string from;  // 含（YYYY-MM-DD），空表示不限
  std::string to;    // 含（YYYY-MM-DD），空表示不限
  std::int64_t item_id = 0;
  std::int64_t warehouse_id = 0;
  int limit = 500;
};

// 报表查询：全部只读，直接基于流水与往来台账聚合，不落缓存表。
class ReportService {
 public:
  explicit ReportService(Db& db) : db_(db) {}

  std::vector<LedgerDetailRow> stock_ledger(const ReportFilter& filter) const;
  std::vector<GoodsFlowRow> goods_flow(const ReportFilter& filter) const;
  std::vector<AgingRow> ar_ap_aging(int direction, std::string_view as_of) const;

 private:
  Db& db_;
};

}  // namespace xiaotu
