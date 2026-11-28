#include "print_render.h"

#include <cstdint>
#include <string>

#include "xiaotu/db.h"
#include "xiaotu/edition.h"
#include "xiaotu/error.h"
#include "xiaotu/http_json.h"
#include "xiaotu/qty.h"
#include "xiaotu/services.h"

namespace xiaotu {

void render_sales_summary(PrintReportContext& ctx) {
  const bool by_salesperson = ctx.target.key == "sales-salesperson";
  const auto rows = ctx.services.reports.sales_summary(
      ctx.filter, by_salesperson ? "salesperson" : "customer");
  std::int64_t total_net = 0;
  std::int64_t total_profit = 0;
  std::int64_t total_qty = 0;
  for (const auto& row : rows) {
    total_net += row.net_cents;
    total_profit += row.profit_cents;
    total_qty += row.qty;
  }
  ctx.render.subtitle = period_label(ctx.filter.from, ctx.filter.to) +
                        "；只计已审核的销售出库与销售退货；" +
                        "退货按负数冲减；毛利按出库时的移动加权成本";
  for (const auto& row : rows) {
    nlohmann::json item{{"name", row.name},
                        {"orders", row.orders},
                        {"customers", row.customers},
                        {"qty", qty_text(row.qty)},
                        {"sales_cents", row.sales_cents},
                        {"return_cents", row.return_cents},
                        {"net_cents", row.net_cents},
                        {"cost_cents", row.cost_cents},
                        {"profit_cents", row.profit_cents},
                        {"margin", margin_text(row.net_cents, row.profit_cents)},
                        {"ratio", margin_text(total_net, row.net_cents)}};
    ctx.render.rows.push_back(std::move(item));
  }
  ctx.render.totals = {{"text", "合计 " + std::to_string(rows.size()) + " 组，数量 " +
                                    qty_text(total_qty) + "，净销售额 " + print_money_text(total_net) +
                                    " 元，毛利 " + print_money_text(total_profit) + " 元"}};
}

void render_stock_balance(PrintReportContext& ctx) {
  const bool hide_zero =
      ctx.req.has_param("hide_zero") && ctx.req.get_param_value("hide_zero") != "0";
  ctx.render.subtitle = dimension_label(ctx.filter.warehouse_id, "仓库") + "；" +
                        dimension_label(ctx.filter.item_id, "商品") +
                        (hide_zero ? "；隐藏零库存" : "；包含零库存") +
                        "；数量跨单位不可加总，合计只汇总金额";
  std::int64_t balance_total = 0;
  std::int64_t balance_rows = 0;
  for (const auto& row :
       ctx.services.stock.balances(ctx.filter.warehouse_id, ctx.filter.item_id, hide_zero)) {
    balance_total += row.amount_cents;
    ++balance_rows;
    ctx.render.rows.push_back({{"item_code", row.item_code},
                               {"item_name", row.item_name},
                               {"unit", row.unit},
                               {"warehouse_code", row.warehouse_code},
                               {"warehouse_name", row.warehouse_name},
                               {"qty", qty_text(row.qty)},
                               {"amount_cents", row.amount_cents}});
  }
  ctx.render.totals = {{"text", "合计 " + std::to_string(balance_rows) + " 行，金额 " +
                                    print_money_text(balance_total) + " 元"}};
}

void render_stock_ledger(PrintReportContext& ctx) {
  ctx.render.subtitle =
      period_label(ctx.filter.from, ctx.filter.to) + "；" +
      dimension_label(ctx.filter.item_id, "商品") + "；" +
      dimension_label(ctx.filter.warehouse_id, "仓库") + "；方向 " +
      qty_direction_label(ctx.filter.qty_direction) + "；业务类型 " +
      (ctx.filter.biz_category.empty() ? std::string("全部")
                                       : stock_biz_category_label(ctx.filter.biz_category));
  const auto ledger_rows = ctx.services.reports.stock_ledger(ctx.filter);
  const auto ledger_totals = ctx.services.reports.stock_ledger_totals(ctx.filter);
  if (ledger_totals.rows > static_cast<std::int64_t>(ledger_rows.size())) {
    ctx.render.subtitle += "；共 " + std::to_string(ledger_totals.rows) + " 行，本次仅含前 " +
                           std::to_string(ledger_rows.size()) + " 行";
  }
  for (const auto& row : ledger_rows) {
    ctx.render.rows.push_back({{"biz_time", row.biz_time},
                               {"biz_type", ledger_biz_label(row)},
                               {"biz_category", stock_biz_category_label(row.biz_category)},
                               {"doc_no", row.doc_no},
                               {"item_code", row.item_code},
                               {"item_name", row.item_name},
                               {"warehouse_name", row.warehouse_name},
                               {"qty", qty_text(row.qty)},
                               {"amount_cents", row.amount_cents}});
  }
  ctx.render.totals = {{"total_qty", qty_text(ledger_totals.qty)},
                       {"total_amount_text", print_money_text(ledger_totals.amount_cents)}};
}

void render_goods_flow(PrintReportContext& ctx) {
  ctx.render.subtitle = period_label(ctx.filter.from, ctx.filter.to) + "；" +
                        dimension_label(ctx.filter.warehouse_id, "仓库") +
                        "；数量跨单位不可加总，合计只汇总金额";
  std::int64_t flow_in = 0;
  std::int64_t flow_out = 0;
  std::int64_t flow_adjust = 0;
  for (const auto& row : ctx.services.reports.goods_flow(ctx.filter)) {
    flow_in += row.in_amount_cents;
    flow_out += row.out_amount_cents;
    flow_adjust += row.adjust_amount_cents;
    ctx.render.rows.push_back({{"item_code", row.item_code},
                               {"item_name", row.item_name},
                               {"unit", row.unit},
                               {"warehouse_code", row.warehouse_code},
                               {"warehouse_name", row.warehouse_name},
                               {"opening_qty", qty_text(row.opening_qty)},
                               {"in_qty", qty_text(row.in_qty)},
                               {"out_qty", qty_text(row.out_qty)},
                               {"closing_qty", qty_text(row.closing_qty)},
                               {"in_amount_cents", row.in_amount_cents},
                               {"out_amount_cents", row.out_amount_cents},
                               {"adjust_amount_cents", row.adjust_amount_cents}});
  }
  ctx.render.totals = {{"text", "合计：收入金额 " + print_money_text(flow_in) + " 元，发出金额 " +
                                    print_money_text(flow_out) + " 元，其他调整 " +
                                    print_money_text(flow_adjust) + " 元"}};
}

void render_inventory_summary(PrintReportContext& ctx) {
  ctx.render.subtitle = period_label(ctx.filter.from, ctx.filter.to) + "；" +
                        dimension_label(ctx.filter.item_id, "商品") + "；" +
                        dimension_label(ctx.filter.warehouse_id, "仓库") +
                        "；按商品跨仓库汇总；数量跨单位不可加总，合计只汇总金额；"
                        "数量为 0、只动金额的流水单列「其他调整」";
  std::int64_t sum_opening = 0;
  std::int64_t sum_in = 0;
  std::int64_t sum_out = 0;
  std::int64_t sum_adjust = 0;
  std::int64_t sum_closing = 0;
  for (const auto& row : ctx.services.reports.inventory_summary(ctx.filter)) {
    sum_opening += row.opening_amount_cents;
    sum_in += row.in_amount_cents;
    sum_out += row.out_amount_cents;
    sum_adjust += row.adjust_amount_cents;
    sum_closing += row.closing_amount_cents;
    ctx.render.rows.push_back({{"item_code", row.item_code},
                               {"item_name", row.item_name},
                               {"unit", row.unit},
                               {"opening_qty", qty_text(row.opening_qty)},
                               {"opening_amount_cents", row.opening_amount_cents},
                               {"in_qty", qty_text(row.in_qty)},
                               {"in_amount_cents", row.in_amount_cents},
                               {"out_qty", qty_text(row.out_qty)},
                               {"out_amount_cents", row.out_amount_cents},
                               {"adjust_amount_cents", row.adjust_amount_cents},
                               {"closing_qty", qty_text(row.closing_qty)},
                               {"closing_amount_cents", row.closing_amount_cents}});
  }
  ctx.render.totals = {{"text", "合计：期初 " + print_money_text(sum_opening) + " 元，入库 " +
                                    print_money_text(sum_in) + " 元，出库 " + print_money_text(sum_out) +
                                    " 元，其他调整 " + print_money_text(sum_adjust) + " 元，期末 " +
                                    print_money_text(sum_closing) + " 元"}};
}

void render_sales_profit(PrintReportContext& ctx) {
  EditionPolicy::instance().require("report.profit");
  ctx.render.subtitle =
      period_label(ctx.filter.from, ctx.filter.to) + "；" +
      dimension_label(ctx.filter.item_id, "商品") + "；客户 " +
      (ctx.filter.customer_id == 0 ? std::string("全部")
                                   : "ID " + std::to_string(ctx.filter.customer_id));
  std::int64_t revenue = 0;
  std::int64_t cost = 0;
  std::int64_t profit = 0;
  for (const auto& row : ctx.services.reports.sales_profit(ctx.filter)) {
    revenue += row.revenue_cents;
    cost += row.cost_cents;
    profit += row.profit_cents;
    ctx.render.rows.push_back({{"item_code", row.item_code},
                               {"item_name", row.item_name},
                               {"unit", row.unit},
                               {"qty", qty_text(row.qty)},
                               {"revenue_cents", row.revenue_cents},
                               {"cost_cents", row.cost_cents},
                               {"profit_cents", row.profit_cents},
                               {"margin", margin_text(row.revenue_cents, row.profit_cents)}});
  }
  ctx.render.totals = {{"text", "合计：收入 " + print_money_text(revenue) + " 元，成本 " +
                                    print_money_text(cost) + " 元，毛利 " + print_money_text(profit) +
                                    " 元，毛利率 " + margin_text(revenue, profit)}};
}

void render_purchase_detail(PrintReportContext& ctx) {
  ctx.render.subtitle = period_label(ctx.filter.from, ctx.filter.to) + "；" +
                        dimension_label(ctx.filter.supplier_id, "供应商") + "；" +
                        dimension_label(ctx.filter.item_id, "商品") + "；" +
                        dimension_label(ctx.filter.warehouse_id, "仓库") +
                        "；退货数量与金额按负数冲减";
  const auto rows = ctx.services.reports.purchase_detail(ctx.filter);
  const auto totals = ctx.services.reports.purchase_detail_totals(ctx.filter);
  if (totals.rows > static_cast<std::int64_t>(rows.size())) {
    ctx.render.subtitle += "；共 " + std::to_string(totals.rows) + " 行，本次仅含前 " +
                           std::to_string(rows.size()) + " 行";
  }
  for (const auto& row : rows) {
    ctx.render.rows.push_back({{"date", row.date},
                               {"doc_no", row.doc_no},
                               {"kind", row.kind},
                               {"supplier", row.supplier},
                               {"item_code", row.item_code},
                               {"item_name", row.item_name},
                               {"spec", row.spec},
                               {"unit", row.unit},
                               {"warehouse_name", row.warehouse_name},
                               {"qty", qty_text(row.qty)},
                               {"price_cents", row.price_cents},
                               {"amount_cents", row.amount_cents}});
  }
  ctx.render.totals = {{"total_qty", qty_text(totals.qty)},
                       {"total_amount_text", print_money_text(totals.amount_cents)}};
}

void render_purchase_summary_supplier(PrintReportContext& ctx) {
  ctx.render.subtitle = period_label(ctx.filter.from, ctx.filter.to) + "；" +
                        dimension_label(ctx.filter.item_id, "商品") + "；" +
                        dimension_label(ctx.filter.warehouse_id, "仓库") +
                        "；按供应商汇总；均价 = 入库金额 ÷ 入库数量";
  for (const auto& row : ctx.services.reports.purchase_summary_by_supplier(ctx.filter)) {
    ctx.render.rows.push_back({{"supplier", row.supplier},
                               {"in_qty", qty_text(row.in_qty)},
                               {"in_amount_cents", row.in_amount_cents},
                               {"return_amount_cents", row.return_amount_cents},
                               {"qty", qty_text(row.qty)},
                               {"amount_cents", row.amount_cents},
                               {"avg_price_cents",
                                row.has_inbound ? nlohmann::json(row.avg_price_cents)
                                                : nlohmann::json("—")}});
  }
  const auto totals = ctx.services.reports.purchase_summary_totals(ctx.filter);
  ctx.render.totals = {{"total_qty", qty_text(totals.qty)},
                       {"total_amount_text", print_money_text(totals.amount_cents)}};
}

void render_purchase_summary_item(PrintReportContext& ctx) {
  ctx.render.subtitle = period_label(ctx.filter.from, ctx.filter.to) + "；" +
                        dimension_label(ctx.filter.supplier_id, "供应商") + "；" +
                        dimension_label(ctx.filter.warehouse_id, "仓库") +
                        "；按商品汇总；均价 = 入库金额 ÷ 入库数量";
  for (const auto& row : ctx.services.reports.purchase_summary_by_item(ctx.filter)) {
    ctx.render.rows.push_back({{"item_code", row.item_code},
                               {"item_name", row.item_name},
                               {"spec", row.spec},
                               {"unit", row.unit},
                               {"in_qty", qty_text(row.in_qty)},
                               {"in_amount_cents", row.in_amount_cents},
                               {"return_amount_cents", row.return_amount_cents},
                               {"qty", qty_text(row.qty)},
                               {"amount_cents", row.amount_cents},
                               {"avg_price_cents",
                                row.has_inbound ? nlohmann::json(row.avg_price_cents)
                                                : nlohmann::json("—")}});
  }
  const auto totals = ctx.services.reports.purchase_summary_totals(ctx.filter);
  ctx.render.totals = {{"total_qty", qty_text(totals.qty)},
                       {"total_amount_text", print_money_text(totals.amount_cents)}};
}

void render_other_flow_summary(PrintReportContext& ctx) {
  ctx.render.subtitle =
      period_label(ctx.filter.from, ctx.filter.to) + "；" +
      dimension_label(ctx.filter.warehouse_id, "仓库") + "；业务类型 " +
      (ctx.filter.biz_category.empty() ? std::string("全部")
                                       : stock_biz_category_label(ctx.filter.biz_category));
  for (const auto& row : ctx.services.reports.other_flow_summary(ctx.filter)) {
    ctx.render.rows.push_back({{"category", stock_biz_category_label(row.category)},
                               {"in_qty", qty_text(row.in_qty)},
                               {"in_amount_cents", row.in_amount_cents},
                               {"out_qty", qty_text(row.out_qty)},
                               {"out_amount_cents", row.out_amount_cents}});
  }
}

void render_fund_ledger(PrintReportContext& ctx) {
  const std::string account_param = param_text(ctx.req, "account_id");
  const std::int64_t account_id =
      account_param == "all" ? -1 : (account_param.empty() ? 0 : parse_id(account_param));
  const std::string from = param_text(ctx.req, "from");
  const std::string to = param_text(ctx.req, "to");
  ctx.render.subtitle = period_label(from, to) + "；账户 " +
                        (account_id < 0 ? std::string("全部账户")
                                        : (account_id == 0 ? std::string("未指定账户")
                                                           : "ID " + std::to_string(account_id)));
  const auto fund_method = [](const std::string& method) -> std::string {
    if (method == "CASH") return "现金";
    if (method == "BANK") return "银行";
    if (method == "WECHAT") return "微信";
    if (method == "ALIPAY") return "支付宝";
    if (method == "OTHER") return "其他";
    return method.empty() ? "未记录" : method;
  };
  for (const auto& row : ctx.services.settlements.ledger(account_id, from, to)) {
    const bool reversed = !row.opening && row.note.rfind("撤销", 0) == 0;
    const std::string kind =
        row.opening ? "期初"
                    : (row.direction == 1 ? (reversed ? "撤销收款" : "收款")
                                          : (reversed ? "撤销付款" : "付款"));
    ctx.render.rows.push_back({{"date", row.settled_on},
                               {"kind", kind},
                               {"doc_no", row.settle_no},
                               {"partner_name", row.partner_name},
                               {"method", fund_method(row.method)},
                               {"operator", row.operator_name},
                               {"summary", row.note},
                               {"income_cents", row.income_cents},
                               {"expense_cents", row.expense_cents},
                               {"balance_cents", row.balance_cents}});
  }
}

void render_ar_ap_aging(PrintReportContext& ctx) {
  int direction = 0;
  const std::string value = param_text(ctx.req, "direction");
  if (!value.empty()) direction = (value == "-1" || value == "AP") ? -1 : 1;
  const std::string as_of = ctx.req.has_param("as_of") ? ctx.req.get_param_value("as_of")
                                                       : today_text();
  ctx.render.subtitle =
      std::string("方向 ") + (direction == -1 ? "应付" : "应收") + "；截止日 " + as_of +
      "；按截止日时点还原（业务日期晚于截止日不计入，核销按核销日期还原）";
  ctx.render.placeholders["as_of"] = as_of;
  std::int64_t aging_total = 0;
  std::int64_t aging_not_due = 0;
  std::int64_t aging_d1 = 0;
  std::int64_t aging_d31 = 0;
  std::int64_t aging_d61 = 0;
  std::int64_t aging_d90 = 0;
  for (const auto& row : ctx.services.reports.ar_ap_aging(direction, as_of)) {
    aging_total += row.total_cents;
    aging_not_due += row.not_due_cents;
    aging_d1 += row.d1_30_cents;
    aging_d31 += row.d31_60_cents;
    aging_d61 += row.d61_90_cents;
    aging_d90 += row.d90_plus_cents;
    ctx.render.rows.push_back({{"partner_name", row.partner_name.empty() ? row.partner_key
                                                                        : row.partner_name},
                               {"direction", row.direction == -1 ? "应付" : "应收"},
                               {"total_cents", row.total_cents},
                               {"not_due_cents", row.not_due_cents},
                               {"d1_30_cents", row.d1_30_cents},
                               {"d31_60_cents", row.d31_60_cents},
                               {"d61_90_cents", row.d61_90_cents},
                               {"d90_plus_cents", row.d90_plus_cents}});
  }
  ctx.render.totals = {{"text", "合计 " + print_money_text(aging_total) + " 元；未到期 " +
                                    print_money_text(aging_not_due) + "；1-30 天 " + print_money_text(aging_d1) +
                                    "；31-60 天 " + print_money_text(aging_d31) + "；61-90 天 " +
                                    print_money_text(aging_d61) + "；90 天以上 " + print_money_text(aging_d90)}};
}

// 对账单
void render_partner_statement(PrintReportContext& ctx) {
  const std::string partner_key = param_text(ctx.req, "partner_key");
  if (partner_key.empty()) {
    throw AppError("INVALID_INPUT", "请指定往来单位");
  }
  // 方向：显式参数优先；未传时按伙伴角色（P:）或台账方向推断，C:/T: 应收、S: 应付。
  int direction = 1;
  const std::string value = param_text(ctx.req, "direction");
  if (!value.empty()) {
    direction = (value == "-1" || value == "AP") ? -1 : 1;
  } else {
    ctx.services.db.read([&](Conn& conn) {
      direction = partner_key_direction(conn, partner_key);
    });
  }
  const StatementResult result = ctx.services.reports.partner_statement(
      partner_key, direction, ctx.filter.from, ctx.filter.to);
  ctx.render.subtitle = std::string("往来单位 ") + result.partner_name + "；方向 " +
                        (direction == -1 ? "应付" : "应收") + "；期间 " +
                        period_label(ctx.filter.from, ctx.filter.to);
  ctx.render.placeholders["partner"] = result.partner_name;
  for (const auto& line : result.lines) {
    const std::string base = direction == -1 ? "付款" : "收款";
    const std::string kind =
        line.kind == "DOC" ? "单据" : (line.is_reverse ? base + "撤销" : base);
    std::string summary;
    if (line.kind == "DOC") {
      summary = doc_type_label(line.doc_type);
    } else if (line.is_reverse) {
      summary = std::string("撤销") + base;
      if (!line.reverses_no.empty()) summary += "（原单 " + line.reverses_no + "）";
      if (!line.note.empty()) summary += "；" + line.note;
    } else {
      summary = line.note.empty() ? kind : line.note;
    }
    ctx.render.rows.push_back({{"date", line.date},
                               {"kind", kind},
                               {"doc_no", line.doc_no},
                               {"summary", summary},
                               {"increase_cents", line.increase_cents},
                               {"decrease_cents", line.decrease_cents},
                               {"balance_cents", line.balance_cents}});
  }
  ctx.render.totals = {{"opening_cents", result.opening_cents},
                       {"increase_cents", result.increase_cents},
                       {"decrease_cents", result.decrease_cents},
                       {"closing_cents", result.closing_cents},
                       {"opening_text", print_money_text(result.opening_cents)},
                       {"closing_text", print_money_text(result.closing_cents)},
                       {"reverse_cents", result.reverse_cents},
                       {"settle_net_cents", result.settle_net_cents},
                       {"reverse_text", std::to_string(result.reverse_count) + " 笔 " +
                                            print_money_text(result.reverse_cents) + " 元"},
                       {"settle_net_text", print_money_text(result.settle_net_cents) + " 元"}};
}

void render_report(PrintReportContext& ctx) {
  const std::string& key = ctx.target.key;
  if (key == "sales-customer" || key == "sales-salesperson") {
    render_sales_summary(ctx);
    return;
  }
  if (key == "stock-balance") {
    render_stock_balance(ctx);
    return;
  }
  if (key == "stock-ledger") {
    render_stock_ledger(ctx);
    return;
  }
  if (key == "goods-flow") {
    render_goods_flow(ctx);
    return;
  }
  if (key == "inventory-summary") {
    render_inventory_summary(ctx);
    return;
  }
  if (key == "sales-profit") {
    render_sales_profit(ctx);
    return;
  }
  if (key == "purchase-detail") {
    render_purchase_detail(ctx);
    return;
  }
  if (key == "purchase-summary-supplier") {
    render_purchase_summary_supplier(ctx);
    return;
  }
  if (key == "purchase-summary-item") {
    render_purchase_summary_item(ctx);
    return;
  }
  if (key == "other-flow-summary") {
    render_other_flow_summary(ctx);
    return;
  }
  if (key == "fund-ledger") {
    render_fund_ledger(ctx);
    return;
  }
  if (key == "ar-ap-aging") {
    render_ar_ap_aging(ctx);
    return;
  }
  render_partner_statement(ctx);
}

}  // namespace xiaotu
