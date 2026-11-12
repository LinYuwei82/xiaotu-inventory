#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xiaotu {

struct Item {
  std::int64_t id = 0;
  std::string code;
  std::string name;
  std::string spec;
  std::string unit;
  bool active = true;
};

struct Warehouse {
  std::int64_t id = 0;
  std::string code;
  std::string name;
  bool is_default = false;
  bool active = true;
};

struct DocLineInput {
  std::int64_t item_id = 0;
  std::int64_t qty = 0;          // 数量（M0 骨架按整数件，精度策略见方案 Q4）
  std::int64_t price_cents = 0;  // 单价（分）
  std::int64_t id = 0;           // doc_line 主键，详情返回时使用
  std::int64_t converted_qty = 0;        // 订单行累计已转出数量
  std::int64_t source_order_line_id = 0;  // 出库行来源订单行
};

struct SalesOutboundInput {
  std::int64_t warehouse_id = 0;
  std::string doc_date;  // 空则取今天
  std::string customer;
  std::vector<DocLineInput> lines;
};

struct DocSummary {
  std::int64_t id = 0;
  std::string doc_no;
  std::string doc_date;
  std::string warehouse_name;
  std::string customer;
  std::int64_t total_qty = 0;
  std::int64_t total_amount_cents = 0;
  std::string status;
  std::string order_status;
  std::string created_at;
};

struct StockBalanceRow {
  std::int64_t item_id = 0;
  std::string item_code;
  std::string item_name;
  std::string unit;
  std::string warehouse_code;
  std::string warehouse_name;
  std::int64_t qty = 0;
  std::int64_t amount_cents = 0;
};

struct StockLedgerRow {
  std::int64_t id = 0;
  std::string biz_type;
  std::string biz_time;
  std::string doc_no;
  std::string item_code;
  std::string item_name;
  std::string warehouse_name;
  std::int64_t qty = 0;
  std::int64_t amount_cents = 0;
};

}  // namespace xiaotu
