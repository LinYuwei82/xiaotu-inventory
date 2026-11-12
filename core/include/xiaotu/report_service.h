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
  std::string biz_category;  // 其他出入库的业务类型，其余单据为空
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

// 销售毛利：销售收入取单据金额，销售成本取审核时写入的库存流水成本。
struct SalesProfitRow {
  std::int64_t item_id = 0;
  std::string item_code;
  std::string item_name;
  std::string unit;
  std::int64_t qty = 0;
  std::int64_t revenue_cents = 0;
  std::int64_t cost_cents = 0;
  std::int64_t profit_cents = 0;
};

// 进销存汇总：按商品（跨仓库）汇总期初、入库、出库、期末，数量与金额双口径。
struct InventorySummaryRow {
  std::int64_t item_id = 0;
  std::string item_code;
  std::string item_name;
  std::string unit;
  std::int64_t opening_qty = 0;
  std::int64_t in_qty = 0;
  std::int64_t out_qty = 0;
  std::int64_t closing_qty = 0;
  std::int64_t opening_amount_cents = 0;
  std::int64_t in_amount_cents = 0;
  std::int64_t out_amount_cents = 0;
  std::int64_t closing_amount_cents = 0;
};

// 对账单明细行：单据与核销记录合并后按日期排序，余额逐行累计。
struct StatementLine {
  std::string date;
  std::string kind;  // DOC 单据 / SETTLE 核销
  std::string doc_no;
  std::string doc_type;  // 单据类型（kind=DOC 时有效）
  std::string note;      // 核销备注（kind=SETTLE 时有效）
  std::int64_t increase_cents = 0;  // 应收/应付增加
  std::int64_t decrease_cents = 0;  // 应收/应付减少
  std::int64_t balance_cents = 0;
};

struct StatementResult {
  std::string partner_key;
  std::string partner_name;
  int direction = 1;  // 1 应收 / -1 应付
  std::string from;
  std::string to;
  std::int64_t opening_cents = 0;
  std::int64_t increase_cents = 0;
  std::int64_t decrease_cents = 0;
  std::int64_t closing_cents = 0;
  std::vector<StatementLine> lines;
};

// 采购明细：采购入库为正、采购退货为负，金额按行数量 × 单价取整到分。
struct PurchaseDetailRow {
  std::string date;
  std::string doc_no;
  std::string kind;  // 采购入库 / 采购退货
  std::string supplier;
  std::int64_t partner_id = 0;
  std::int64_t item_id = 0;
  std::string item_code;
  std::string item_name;
  std::string spec;
  std::string unit;
  std::string warehouse_name;
  std::int64_t qty = 0;          // 退货为负
  std::int64_t price_cents = 0;  // 退货仍为正的退货单价
  std::int64_t amount_cents = 0; // 退货为负
};

struct PurchaseSupplierSummaryRow {
  std::int64_t partner_id = 0;
  std::string supplier;
  std::int64_t qty = 0;
  std::int64_t amount_cents = 0;
  std::int64_t avg_price_cents = 0;
};

struct PurchaseItemSummaryRow {
  std::int64_t item_id = 0;
  std::string item_code;
  std::string item_name;
  std::string spec;
  std::string unit;
  std::int64_t qty = 0;
  std::int64_t amount_cents = 0;
  std::int64_t avg_price_cents = 0;
};

struct ReportFilter {
  std::string from;  // 含（YYYY-MM-DD），空表示不限
  std::string to;    // 含（YYYY-MM-DD），空表示不限
  std::int64_t item_id = 0;
  std::int64_t warehouse_id = 0;
  std::int64_t customer_id = 0;
  std::int64_t supplier_id = 0;
  int qty_direction = 0;  // 1 只看入库（qty>0）/ -1 只看出库（qty<0）/ 0 全部
  std::string biz_category;  // 其他出入库的业务类型筛选，空表示全部
  int limit = 500;
};

// 报表查询：全部只读，直接基于流水与往来台账聚合，不落缓存表。
class ReportService {
 public:
  explicit ReportService(Db& db) : db_(db) {}

  std::vector<LedgerDetailRow> stock_ledger(const ReportFilter& filter) const;
  std::vector<GoodsFlowRow> goods_flow(const ReportFilter& filter) const;
  std::vector<InventorySummaryRow> inventory_summary(const ReportFilter& filter) const;
  std::vector<SalesProfitRow> sales_profit(const ReportFilter& filter) const;
  // 采购明细与汇总：默认含采购入库与采购退货，退货按负数冲减。
  std::vector<PurchaseDetailRow> purchase_detail(const ReportFilter& filter) const;
  std::vector<PurchaseSupplierSummaryRow> purchase_summary_by_supplier(
      const ReportFilter& filter) const;
  std::vector<PurchaseItemSummaryRow> purchase_summary_by_item(
      const ReportFilter& filter) const;
  std::vector<AgingRow> ar_ap_aging(int direction, std::string_view as_of) const;

  // 往来对账单：期初 + 期间单据 - 期间核销 = 期末；只读，不落表。
  StatementResult partner_statement(std::string_view partner_key, int direction,
                                    std::string_view from, std::string_view to) const;

 private:
  Db& db_;
};

}  // namespace xiaotu
