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
  std::int64_t in_qty = 0;            // 入库数量（只计 PUR_IN）
  std::int64_t in_amount_cents = 0;   // 入库金额（只计 PUR_IN）
  std::int64_t return_amount_cents = 0;  // 退货额（正数，便于展示）
  std::int64_t qty = 0;               // 净数量 = 入库 - 退货
  std::int64_t amount_cents = 0;      // 净金额 = 入库 - 退货
  std::int64_t avg_price_cents = 0;   // 入库均价 = 入库金额 / 入库数量
  bool has_inbound = false;           // 无入库时均价显示「—」
};

struct PurchaseItemSummaryRow {
  std::int64_t item_id = 0;
  std::string item_code;
  std::string item_name;
  std::string spec;
  std::string unit;
  std::int64_t in_qty = 0;
  std::int64_t in_amount_cents = 0;
  std::int64_t return_amount_cents = 0;
  std::int64_t qty = 0;
  std::int64_t amount_cents = 0;
  std::int64_t avg_price_cents = 0;
  bool has_inbound = false;
};

// 采购汇总合计：均价同样按入库行全量计算，不能对分组均价再平均。
struct PurchaseSummaryTotals {
  std::int64_t in_qty = 0;
  std::int64_t in_amount_cents = 0;
  std::int64_t return_amount_cents = 0;
  std::int64_t qty = 0;
  std::int64_t amount_cents = 0;
  std::int64_t avg_price_cents = 0;
  bool has_inbound = false;
};

// 分页受限的明细报表：合计必须按全量算，不能用页面截断后的行求和。
struct ReportTotals {
  std::int64_t rows = 0;
  std::int64_t qty = 0;
  std::int64_t amount_cents = 0;
};

// 其他出入库按业务类型汇总：入库与出库分列，出库数量与金额取绝对值。
struct OtherFlowSummaryRow {
  std::string category;  // GIFT / USAGE / LOSS / OPENING / OTHER
  std::int64_t in_qty = 0;
  std::int64_t in_amount_cents = 0;
  std::int64_t out_qty = 0;
  std::int64_t out_amount_cents = 0;
};

// 销售报表分组维度：客户 / 业务员。空客户或空业务员单列「未指定」，不并入其它组。
struct SalesSummaryRow {
  std::int64_t group_id = 0;
  std::string name;
  std::int64_t orders = 0;      // 单数（出库 + 退货去重）
  std::int64_t customers = 0;   // 业务员维度：客户数
  std::int64_t qty = 0;         // 净数量（退货为负）
  std::int64_t sales_cents = 0; // 销售出库金额
  std::int64_t return_cents = 0;// 退货额（正数）
  std::int64_t net_cents = 0;   // 净销售额
  std::int64_t cost_cents = 0;  // 净成本
  std::int64_t profit_cents = 0;
};

struct SalesDetailRow {
  std::string date;
  std::string doc_no;
  std::string customer;
  std::string salesperson;
  std::string item_code;
  std::string item_name;
  std::string spec;
  std::string unit;
  std::int64_t qty = 0;          // 退货为负
  std::int64_t price_cents = 0;  // 退货单价仍为正
  std::int64_t amount_cents = 0; // 退货为负
  std::int64_t cost_cents = 0;   // 退货成本为负
};

// 销售明细合计：按全量筛选条件计算，不受分页影响。
struct SalesDetailTotals {
  std::int64_t rows = 0;
  std::int64_t qty = 0;
  std::int64_t amount_cents = 0;
  std::int64_t cost_cents = 0;
  std::int64_t profit_cents = 0;
};

struct DashboardDebtor {
  std::int64_t partner_id = 0;
  std::string name;
  std::int64_t outstanding_cents = 0;
};

// 经营看板：今日/本月销售额与毛利按单据日期归属，退货冲减；
// 毛利按出库时的移动加权成本（库存流水金额）；库存总额取当前结存金额。
struct DashboardResult {
  std::string today;
  std::string month_start;
  std::int64_t today_sales_cents = 0;
  std::int64_t today_return_cents = 0;
  std::int64_t today_cost_cents = 0;
  std::int64_t today_profit_cents = 0;
  std::int64_t month_sales_cents = 0;
  std::int64_t month_return_cents = 0;
  std::int64_t month_cost_cents = 0;
  std::int64_t month_profit_cents = 0;
  std::int64_t stock_value_cents = 0;
  std::vector<DashboardDebtor> top_debtors;
};

struct ReportFilter {
  std::string from;  // 含（YYYY-MM-DD），空表示不限
  std::string to;    // 含（YYYY-MM-DD），空表示不限
  std::int64_t item_id = 0;
  std::int64_t warehouse_id = 0;
  std::int64_t customer_id = 0;
  std::int64_t supplier_id = 0;
  std::int64_t salesperson_id = 0;  // 销售报表按业务员筛选，0 表示全部
  int qty_direction = 0;  // 1 只看入库（qty>0）/ -1 只看出库（qty<0）/ 0 全部
  std::string biz_category;  // 其他出入库的业务类型筛选，空表示全部
  int limit = 200;
  int offset = 0;  // 真分页偏移；导出时由调用方把 limit 放大到全量
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
  PurchaseSummaryTotals purchase_summary_totals(const ReportFilter& filter) const;
  std::vector<OtherFlowSummaryRow> other_flow_summary(const ReportFilter& filter) const;
  // 销售报表：dimension = "customer"（默认）或 "salesperson"。
  std::vector<SalesSummaryRow> sales_summary(const ReportFilter& filter,
                                             std::string_view dimension) const;
  std::vector<SalesDetailRow> sales_detail(const ReportFilter& filter,
                                           std::string_view dimension) const;
  SalesDetailTotals sales_detail_totals(const ReportFilter& filter) const;
  // 经营看板：today 传 YYYY-MM-DD，本月按自然月 1 号起算。
  DashboardResult dashboard(std::string_view today) const;
  ReportTotals purchase_detail_totals(const ReportFilter& filter) const;
  ReportTotals stock_ledger_totals(const ReportFilter& filter) const;
  std::vector<AgingRow> ar_ap_aging(int direction, std::string_view as_of) const;

  // 往来对账单：期初 + 期间单据 - 期间核销 = 期末；只读，不落表。
  StatementResult partner_statement(std::string_view partner_key, int direction,
                                    std::string_view from, std::string_view to) const;

 private:
  Db& db_;
};

}  // namespace xiaotu
