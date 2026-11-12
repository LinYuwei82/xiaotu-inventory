#include <httplib.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xiaotu/context.h"
#include "xiaotu/document.h"
#include "xiaotu/error.h"
#include "xiaotu/http_guard.h"
#include "xiaotu/http_json.h"
#include "xiaotu/permissions.h"
#include "xiaotu/print_template.h"
#include "xiaotu/qty.h"
#include "xiaotu/report_service.h"
#include "xiaotu/services.h"

namespace xiaotu {
namespace {

nlohmann::json column_to_json(const PrintTemplateColumn& column) {
  return nlohmann::json{
      {"column_key", column.column_key},
      {"position", column.position},
      {"visible", column.visible},
      {"width_mm", column.width_mm},
      {"align", column.align},
      {"font_size_pt", column.font_size_pt},
      {"bold", column.bold},
  };
}

nlohmann::json template_to_json(const PrintTemplate& tmpl) {
  nlohmann::json columns = nlohmann::json::array();
  for (const auto& column : tmpl.columns) {
    columns.push_back(column_to_json(column));
  }
  return nlohmann::json{
      {"id", tmpl.id},
      {"code", tmpl.code},
      {"name", tmpl.name},
      {"target_type", tmpl.target_type},
      {"target_key", tmpl.target_key},
      {"page_size", tmpl.page_size},
      {"orientation", tmpl.orientation},
      {"paper_width_mm", tmpl.paper_width_mm},
      {"paper_height_mm", tmpl.paper_height_mm},
      {"margin_mm", tmpl.margin_mm},
      {"font_size_pt", tmpl.font_size_pt},
      {"header_text", tmpl.header_text},
      {"footer_text", tmpl.footer_text},
      {"show_page_no", tmpl.show_page_no},
      {"is_default", tmpl.is_default},
      {"active", tmpl.active},
      {"version", tmpl.version},
      {"created_at", tmpl.created_at},
      {"created_by", tmpl.created_by},
      {"updated_at", tmpl.updated_at},
      {"updated_by", tmpl.updated_by},
      {"columns", columns},
  };
}

nlohmann::json def_column_to_json(const PrintColumnDef& column) {
  return nlohmann::json{
      {"key", column.key},
      {"label", column.label},
      {"type", column.type},
      {"align", column.align},
      {"width_mm", column.width_mm},
  };
}

PrintTemplate template_from_json(const nlohmann::json& body) {
  PrintTemplate tmpl;
  tmpl.code = body.value("code", std::string{});
  tmpl.name = body.value("name", std::string{});
  tmpl.target_type = body.value("target_type", std::string{});
  tmpl.target_key = body.value("target_key", std::string{});
  tmpl.page_size = body.value("page_size", std::string{"A4"});
  tmpl.orientation = body.value("orientation", std::string{"PORTRAIT"});
  tmpl.paper_width_mm = body.value("paper_width_mm", 0);
  tmpl.paper_height_mm = body.value("paper_height_mm", 0);
  tmpl.margin_mm = body.value("margin_mm", 12);
  tmpl.font_size_pt = body.value("font_size_pt", 10);
  tmpl.header_text = body.value("header_text", std::string{});
  tmpl.footer_text = body.value("footer_text", std::string{});
  tmpl.show_page_no = body.value("show_page_no", true);
  tmpl.is_default = body.value("is_default", false);
  tmpl.active = body.value("active", true);
  tmpl.version = body.value("version", std::int64_t{1});
  if (body.contains("columns") && body["columns"].is_array()) {
    for (const auto& item : body["columns"]) {
      PrintTemplateColumn column;
      column.column_key = item.value("column_key", std::string{});
      column.position = item.value("position", 0);
      column.visible = item.value("visible", true);
      column.width_mm = item.value("width_mm", 0);
      column.align = item.value("align", std::string{"LEFT"});
      column.font_size_pt = item.value("font_size_pt", 0);
      column.bold = item.value("bold", false);
      tmpl.columns.push_back(std::move(column));
    }
  }
  return tmpl;
}

std::int64_t param_i64(const httplib::Request& req, const char* name, std::int64_t fallback) {
  if (!req.has_param(name)) return fallback;
  try {
    return std::stoll(req.get_param_value(name));
  } catch (const std::exception&) {
    throw AppError("INVALID_INPUT", std::string("参数不是合法数字：") + name);
  }
}

std::string param_text(const httplib::Request& req, const char* name) {
  return req.has_param(name) ? req.get_param_value(name) : std::string{};
}

std::string period_label(const std::string& from, const std::string& to) {
  if (from.empty() && to.empty()) return "全部期间";
  return (from.empty() ? std::string("起始不限") : from) + " 至 " +
         (to.empty() ? std::string("截止不限") : to);
}

std::string dimension_label(std::int64_t id, const char* name) {
  return id == 0 ? std::string(name) + " 全部"
                 : std::string(name) + " ID " + std::to_string(id);
}

std::string money_text(std::int64_t cents) {
  const bool negative = cents < 0;
  const std::int64_t abs = negative ? -cents : cents;
  std::string fraction = std::to_string(abs % 100);
  if (fraction.size() < 2) fraction.insert(0, 2 - fraction.size(), '0');
  return (negative ? "-" : "") + std::to_string(abs / 100) + "." + fraction;
}

std::string margin_text(std::int64_t revenue_cents, std::int64_t profit_cents) {
  if (revenue_cents == 0) return "—";
  // 毛利率保留一位小数，用整数千分比换算，展示层也不引入浮点。
  const std::int64_t direction = revenue_cents < 0 ? -1 : 1;
  const std::int64_t revenue = revenue_cents < 0 ? -revenue_cents : revenue_cents;
  const std::int64_t profit = profit_cents * direction;
  const bool negative = profit < 0;
  const std::int64_t absolute = negative ? -profit : profit;
  const std::int64_t tenths = (absolute * 1000 + revenue / 2) / revenue;
  return (negative ? "-" : "") + std::to_string(tenths / 10) + "." +
         std::to_string(tenths % 10) + "%";
}

std::string doc_type_label(const std::string& doc_type) {
  if (doc_type == "SAL_OUT") return "销售出库";
  if (doc_type == "SAL_RET") return "销售退货";
  if (doc_type == "PUR_IN") return "采购入库";
  if (doc_type == "PUR_RET") return "采购退货";
  if (doc_type == "STK_TRF") return "库存调拨";
  if (doc_type == "STK_CNT") return "库存盘点";
  return doc_type;
}

// 库存流水类型标签：盘盈/盘亏与各种 *_REVERSE 统一转中文。
std::string stock_biz_label(const std::string& biz_type) {
  if (biz_type == "REVERSE") return "反审核回冲";
  constexpr std::string_view kReverse = "_REVERSE";
  if (biz_type.size() > kReverse.size() &&
      biz_type.compare(biz_type.size() - kReverse.size(), kReverse.size(), kReverse) == 0) {
    return stock_biz_label(biz_type.substr(0, biz_type.size() - kReverse.size())) + "回冲";
  }
  if (biz_type == "PUR_IN") return "采购入库";
  if (biz_type == "PUR_RET") return "采购退货";
  if (biz_type == "SAL_OUT") return "销售出库";
  if (biz_type == "SAL_RET") return "销售退货";
  if (biz_type == "OTH_IN") return "其他入库";
  if (biz_type == "OTH_OUT") return "其他出库";
  if (biz_type == "STK_TRF") return "调拨出库";
  if (biz_type == "STK_TRF_IN") return "调拨入库";
  if (biz_type == "STK_CNT_IN") return "盘盈";
  if (biz_type == "STK_CNT_OUT") return "盘亏";
  if (biz_type == "STK_CNT") return "盘点调整";
  if (biz_type == "CST_ADJ") return "成本调整";
  return biz_type;
}

std::string qty_direction_label(int direction) {
  if (direction > 0) return "入库";
  if (direction < 0) return "出库";
  return "全部";
}

std::string stock_biz_category_label(const std::string& category) {
  if (category == "GIFT") return "赠品";
  if (category == "USAGE") return "领用";
  if (category == "LOSS") return "报损";
  if (category == "OPENING") return "期初";
  if (category == "OTHER") return "其他";
  return category;
}

std::string doc_status_label(const std::string& status) {
  if (status == "DRAFT") return "草稿";
  if (status == "APPROVED") return "已审核";
  if (status == "VOIDED") return "已作废";
  return status;
}

// 页眉页脚只做纯文本占位符替换；未知占位符保持原样，不执行任何表达式。
std::string apply_placeholders(std::string text,
                               const std::map<std::string, std::string>& values) {
  for (const auto& [key, value] : values) {
    const std::string token = "{" + key + "}";
    std::string::size_type pos = 0;
    while ((pos = text.find(token, pos)) != std::string::npos) {
      text.replace(pos, token.size(), value);
      pos += value.size();
    }
  }
  return text;
}

const char* data_permission(const PrintTargetDef& target) {
  if (target.type == "DOC") return "document.read";
  if (target.key == "sales-profit" || target.key == "purchase-detail" ||
      target.key == "purchase-summary-supplier" || target.key == "purchase-summary-item" ||
      target.key == "ar-ap-aging" || target.key == "statement" || target.key == "fund-ledger") {
    return "finance.read";
  }
  return "stock.read";
}

ReportFilter report_filter(const httplib::Request& req) {
  ReportFilter filter;
  filter.from = param_text(req, "from");
  filter.to = param_text(req, "to");
  filter.item_id = param_i64(req, "item_id", 0);
  filter.warehouse_id = param_i64(req, "warehouse_id", 0);
  filter.customer_id = param_i64(req, "customer_id", 0);
  filter.supplier_id = param_i64(req, "supplier_id", 0);
  const std::string flow = param_text(req, "flow");
  if (flow == "in") filter.qty_direction = 1;
  if (flow == "out") filter.qty_direction = -1;
  filter.biz_category = param_text(req, "biz_category");
  filter.limit = static_cast<int>(param_i64(req, "limit", 100000));
  return filter;
}

struct PrintRender {
  std::string title;
  std::string subtitle;
  nlohmann::json rows = nlohmann::json::array();
  nlohmann::json header_fields = nlohmann::json::array();
  nlohmann::json totals = nlohmann::json::object();
  std::map<std::string, std::string> placeholders;
};

void render_report(Services& services, const PrintTargetDef& target, const httplib::Request& req,
                   PrintRender& render) {
  const ReportFilter filter = report_filter(req);

  if (target.key == "stock-balance") {
    render.subtitle = "全部仓库 / 全部商品";
    for (const auto& row : services.stock.balances()) {
      render.rows.push_back({{"item_code", row.item_code},
                             {"item_name", row.item_name},
                             {"unit", row.unit},
                             {"warehouse_code", row.warehouse_code},
                             {"warehouse_name", row.warehouse_name},
                             {"qty", qty_text(row.qty)},
                             {"amount_cents", row.amount_cents}});
    }
    return;
  }

  if (target.key == "stock-ledger") {
    render.subtitle = period_label(filter.from, filter.to) + "；" +
                      dimension_label(filter.item_id, "商品") + "；" +
                      dimension_label(filter.warehouse_id, "仓库") + "；方向 " +
                      qty_direction_label(filter.qty_direction) + "；业务类型 " +
                      (filter.biz_category.empty() ? std::string("全部")
                                                   : stock_biz_category_label(filter.biz_category));
    for (const auto& row : services.reports.stock_ledger(filter)) {
      render.rows.push_back({{"biz_time", row.biz_time},
                             {"biz_type", stock_biz_label(row.biz_type)},
                             {"biz_category", stock_biz_category_label(row.biz_category)},
                             {"doc_no", row.doc_no},
                             {"item_code", row.item_code},
                             {"item_name", row.item_name},
                             {"warehouse_name", row.warehouse_name},
                             {"qty", qty_text(row.qty)},
                             {"amount_cents", row.amount_cents}});
    }
    return;
  }

  if (target.key == "goods-flow") {
    render.subtitle = period_label(filter.from, filter.to) + "；" +
                      dimension_label(filter.warehouse_id, "仓库");
    for (const auto& row : services.reports.goods_flow(filter)) {
      render.rows.push_back({{"item_code", row.item_code},
                             {"item_name", row.item_name},
                             {"unit", row.unit},
                             {"warehouse_code", row.warehouse_code},
                             {"warehouse_name", row.warehouse_name},
                             {"opening_qty", qty_text(row.opening_qty)},
                             {"in_qty", qty_text(row.in_qty)},
                             {"out_qty", qty_text(row.out_qty)},
                             {"closing_qty", qty_text(row.closing_qty)},
                             {"in_amount_cents", row.in_amount_cents},
                             {"out_amount_cents", row.out_amount_cents}});
    }
    return;
  }

  if (target.key == "inventory-summary") {
    render.subtitle = period_label(filter.from, filter.to) + "；" +
                      dimension_label(filter.item_id, "商品") + "；" +
                      dimension_label(filter.warehouse_id, "仓库") + "；按商品跨仓库汇总";
    for (const auto& row : services.reports.inventory_summary(filter)) {
      render.rows.push_back({{"item_code", row.item_code},
                             {"item_name", row.item_name},
                             {"unit", row.unit},
                             {"opening_qty", qty_text(row.opening_qty)},
                             {"opening_amount_cents", row.opening_amount_cents},
                             {"in_qty", qty_text(row.in_qty)},
                             {"in_amount_cents", row.in_amount_cents},
                             {"out_qty", qty_text(row.out_qty)},
                             {"out_amount_cents", row.out_amount_cents},
                             {"closing_qty", qty_text(row.closing_qty)},
                             {"closing_amount_cents", row.closing_amount_cents}});
    }
    return;
  }

  if (target.key == "sales-profit") {
    render.subtitle = period_label(filter.from, filter.to) + "；" +
                      dimension_label(filter.item_id, "商品") + "；客户 " +
                      (filter.customer_id == 0 ? std::string("全部")
                                               : "ID " + std::to_string(filter.customer_id));
    for (const auto& row : services.reports.sales_profit(filter)) {
      render.rows.push_back({{"item_code", row.item_code},
                             {"item_name", row.item_name},
                             {"unit", row.unit},
                             {"qty", qty_text(row.qty)},
                             {"revenue_cents", row.revenue_cents},
                             {"cost_cents", row.cost_cents},
                             {"profit_cents", row.profit_cents},
                             {"margin", margin_text(row.revenue_cents, row.profit_cents)}});
    }
    return;
  }

  if (target.key == "purchase-detail") {
    render.subtitle = period_label(filter.from, filter.to) + "；" +
                      dimension_label(filter.supplier_id, "供应商") + "；" +
                      dimension_label(filter.item_id, "商品") + "；" +
                      dimension_label(filter.warehouse_id, "仓库") +
                      "；退货数量与金额按负数冲减";
    std::int64_t total_qty = 0;
    std::int64_t total_amount = 0;
    for (const auto& row : services.reports.purchase_detail(filter)) {
      total_qty += row.qty;
      total_amount += row.amount_cents;
      render.rows.push_back({{"date", row.date},
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
    render.totals = {{"total_qty", qty_text(total_qty)},
                     {"total_amount_text", money_text(total_amount)}};
    return;
  }

  if (target.key == "purchase-summary-supplier") {
    render.subtitle = period_label(filter.from, filter.to) + "；" +
                      dimension_label(filter.item_id, "商品") + "；" +
                      dimension_label(filter.warehouse_id, "仓库") + "；按供应商汇总";
    std::int64_t total_qty = 0;
    std::int64_t total_amount = 0;
    for (const auto& row : services.reports.purchase_summary_by_supplier(filter)) {
      total_qty += row.qty;
      total_amount += row.amount_cents;
      render.rows.push_back({{"supplier", row.supplier},
                             {"qty", qty_text(row.qty)},
                             {"amount_cents", row.amount_cents},
                             {"avg_price_cents", row.avg_price_cents}});
    }
    render.totals = {{"total_qty", qty_text(total_qty)},
                     {"total_amount_text", money_text(total_amount)}};
    return;
  }

  if (target.key == "purchase-summary-item") {
    render.subtitle = period_label(filter.from, filter.to) + "；" +
                      dimension_label(filter.supplier_id, "供应商") + "；" +
                      dimension_label(filter.warehouse_id, "仓库") + "；按商品汇总";
    std::int64_t total_qty = 0;
    std::int64_t total_amount = 0;
    for (const auto& row : services.reports.purchase_summary_by_item(filter)) {
      total_qty += row.qty;
      total_amount += row.amount_cents;
      render.rows.push_back({{"item_code", row.item_code},
                             {"item_name", row.item_name},
                             {"spec", row.spec},
                             {"unit", row.unit},
                             {"qty", qty_text(row.qty)},
                             {"amount_cents", row.amount_cents},
                             {"avg_price_cents", row.avg_price_cents}});
    }
    render.totals = {{"total_qty", qty_text(total_qty)},
                     {"total_amount_text", money_text(total_amount)}};
    return;
  }

  if (target.key == "fund-ledger") {
    const std::int64_t account_id = param_i64(req, "account_id", 0);
    const std::string from = param_text(req, "from");
    const std::string to = param_text(req, "to");
    render.subtitle = period_label(from, to) + "；账户 " +
                      (account_id == 0 ? std::string("未指定账户")
                                       : "ID " + std::to_string(account_id));
    const auto fund_method = [](const std::string& method) -> std::string {
      if (method == "CASH") return "现金";
      if (method == "BANK") return "银行";
      if (method == "WECHAT") return "微信";
      if (method == "ALIPAY") return "支付宝";
      if (method == "OTHER") return "其他";
      return method.empty() ? "未记录" : method;
    };
    for (const auto& row : services.settlements.ledger(account_id, from, to)) {
      render.rows.push_back(
          {{"date", row.settled_on},
           {"kind", row.opening ? "期初" : (row.direction == 1 ? "收款" : "付款")},
           {"doc_no", row.settle_no},
           {"partner_name", row.partner_name},
           {"method", fund_method(row.method)},
           {"operator", row.operator_name},
           {"summary", row.note},
           {"income_cents", row.income_cents},
           {"expense_cents", row.expense_cents},
           {"balance_cents", row.balance_cents}});
    }
    return;
  }

  if (target.key == "ar-ap-aging") {
    int direction = 0;
    const std::string value = param_text(req, "direction");
    if (!value.empty()) direction = (value == "-1" || value == "AP") ? -1 : 1;
    const std::string as_of = req.has_param("as_of") ? req.get_param_value("as_of")
                                                     : today_text();
    render.subtitle = std::string("方向 ") + (direction == -1 ? "应付" : "应收") + "；截止日 " +
                      as_of;
    render.placeholders["as_of"] = as_of;
    for (const auto& row : services.reports.ar_ap_aging(direction, as_of)) {
      render.rows.push_back({{"partner_name", row.partner_name.empty() ? row.partner_key
                                                                      : row.partner_name},
                             {"direction", row.direction == -1 ? "应付" : "应收"},
                             {"total_cents", row.total_cents},
                             {"not_due_cents", row.not_due_cents},
                             {"d1_30_cents", row.d1_30_cents},
                             {"d31_60_cents", row.d31_60_cents},
                             {"d61_90_cents", row.d61_90_cents},
                             {"d90_plus_cents", row.d90_plus_cents}});
    }
    return;
  }

  // 对账单
  const std::string partner_key = param_text(req, "partner_key");
  if (partner_key.empty()) {
    throw AppError("INVALID_INPUT", "请指定往来单位");
  }
  int direction = partner_key.rfind("S:", 0) == 0 ? -1 : 1;
  const std::string value = param_text(req, "direction");
  if (!value.empty()) direction = (value == "-1" || value == "AP") ? -1 : 1;
  const StatementResult result =
      services.reports.partner_statement(partner_key, direction, filter.from, filter.to);
  render.subtitle = std::string("往来单位 ") + result.partner_name + "；方向 " +
                    (direction == -1 ? "应付" : "应收") + "；期间 " +
                    period_label(filter.from, filter.to);
  render.placeholders["partner"] = result.partner_name;
  for (const auto& line : result.lines) {
    const std::string kind = line.kind == "DOC" ? "单据" : (direction == -1 ? "付款" : "收款");
    std::string summary = line.kind == "DOC" ? doc_type_label(line.doc_type) : line.note;
    if (summary.empty()) summary = kind;
    render.rows.push_back({{"date", line.date},
                           {"kind", kind},
                           {"doc_no", line.doc_no},
                           {"summary", summary},
                           {"increase_cents", line.increase_cents},
                           {"decrease_cents", line.decrease_cents},
                           {"balance_cents", line.balance_cents}});
  }
  render.totals = {{"opening_cents", result.opening_cents},
                   {"increase_cents", result.increase_cents},
                   {"decrease_cents", result.decrease_cents},
                   {"closing_cents", result.closing_cents},
                   {"opening_text", money_text(result.opening_cents)},
                   {"closing_text", money_text(result.closing_cents)}};
}

void render_document(Services& services, const PrintTargetDef& target, const httplib::Request& req,
                     PrintRender& render) {
  const std::int64_t doc_id = param_i64(req, "doc_id", 0);
  if (doc_id <= 0) {
    throw AppError("INVALID_INPUT", "请指定单据");
  }
  const DocDetail detail = services.docs.detail(doc_id);
  if (detail.doc_type != target.key) {
    throw AppError("INVALID_INPUT",
                   "单据类型与打印目标不一致：" + detail.doc_type + " / " + target.key);
  }

  std::map<std::int64_t, gen::ItemRow> items;
  for (const auto& row : services.item.list()) {
    items[row.id] = row;
  }
  std::string warehouse_name;
  std::map<std::int64_t, std::string> warehouse_names;
  for (const auto& row : services.warehouse.list()) {
    warehouse_names[row.id] = row.name;
    if (row.id == detail.warehouse_id) warehouse_name = row.name;
  }

  int line_no = 0;
  for (const auto& line : detail.lines) {
    const auto found = items.find(line.item_id);
    const std::string code = found == items.end() ? std::string{} : found->second.code;
    const std::string name = found == items.end() ? std::string{} : found->second.name;
    const std::string spec = found == items.end() ? std::string{} : found->second.spec;
    const std::string unit = found == items.end() ? std::string{} : found->second.unit;
    if (detail.doc_type == "CST_ADJ") {
      const auto wh = warehouse_names.find(line.warehouse_id);
      render.rows.push_back({{"line_no", ++line_no},
                             {"item_code", code},
                             {"item_name", name},
                             {"spec", spec},
                             {"unit", unit},
                             {"warehouse", wh == warehouse_names.end() ? std::string{}
                                                                       : wh->second},
                             {"adjust_cents", line.adjust_cents},
                             {"adjust_reason", line.adjust_reason}});
      continue;
    }
    render.rows.push_back({{"line_no", ++line_no},
                           {"item_code", code},
                           {"item_name", name},
                           {"spec", spec},
                           {"unit", unit},
                           {"qty", qty_text(line.qty)},
                           {"price_cents", line.price_cents},
                           {"amount_cents", mul_qty_price(line.qty, line.price_cents)}});
  }

  const std::string header_warehouse =
      detail.doc_type == "CST_ADJ" ? std::string("按明细行") : warehouse_name;
  render.subtitle = "单号 " + detail.doc_no + "；日期 " + detail.doc_date + "；仓库 " +
                    (header_warehouse.empty() ? "—" : header_warehouse) + "；状态 " +
                    doc_status_label(detail.status);
  render.header_fields = nlohmann::json::array({
      {{"label", "单号"}, {"value", detail.doc_no}},
      {{"label", "日期"}, {"value", detail.doc_date}},
      {{"label", "仓库"}, {"value", header_warehouse}},
      {{"label", "往来单位"}, {"value", detail.partner}},
  });
  if (!detail.biz_category.empty()) {
    render.header_fields.push_back(
        {{"label", "业务类型"}, {"value", stock_biz_category_label(detail.biz_category)}});
  }
  render.header_fields.push_back({{"label", "状态"}, {"value", doc_status_label(detail.status)}});
  render.totals = {{"total_qty", qty_text(detail.total_qty)},
                   {"total_amount_cents", detail.total_amount_cents},
                   {"total_amount_text", money_text(detail.total_amount_cents)}};
  render.placeholders["doc_no"] = detail.doc_no;
  render.placeholders["doc_date"] = detail.doc_date;
  render.placeholders["partner"] = detail.partner;
  render.placeholders["warehouse"] = warehouse_name;
}

void respond_render(Services& services, const httplib::Request& req, httplib::Response& res) {
  const std::string target_type = param_text(req, "target_type");
  const std::string target_key = param_text(req, "target_key");
  const PrintTargetDef* target = find_print_target(target_type, target_key);
  if (target == nullptr) {
    throw AppError("INVALID_INPUT", "未知的打印目标：" + target_type + "/" + target_key);
  }

  // 两层权限：渲染不要求模板管理权限，但必须拥有该目标本身的数据权限。
  const char* permission = data_permission(*target);
  if (!role_has_permission(current_context().role, permission)) {
    json_error(res, 403, "FORBIDDEN", std::string("没有权限查看该数据: ") + permission);
    return;
  }

  const PrintTemplate tmpl =
      services.print_templates.resolve(target->type, target->key,
                                       param_i64(req, "template_id", 0));

  PrintRender render;
  render.title = target->name;
  if (target->type == "DOC") {
    render_document(services, *target, req, render);
  } else {
    render_report(services, *target, req, render);
  }

  render.placeholders["title"] = render.title;
  render.placeholders["date"] = today_text();
  render.placeholders["printed_at"] = now_text();
  render.placeholders["period"] = render.subtitle;

  nlohmann::json columns = nlohmann::json::array();
  for (const auto& column : resolved_print_columns(tmpl)) {
    columns.push_back({{"key", column.def.key},
                       {"label", column.def.label},
                       {"type", column.def.type},
                       {"align", column.align},
                       {"width_mm", column.width_mm},
                       {"font_size_pt", column.font_size_pt},
                       {"bold", column.bold}});
  }

  std::string source = "BUILTIN";
  if (tmpl.id > 0) source = tmpl.is_default ? "DEFAULT" : "EXPLICIT";

  nlohmann::json body;
  body["target"] = {{"type", target->type}, {"key", target->key}, {"name", target->name}};
  body["template"] = template_to_json(tmpl);
  body["template_source"] = source;
  body["columns"] = columns;
  body["title"] = render.title;
  body["subtitle"] = render.subtitle;
  body["printed_at"] = now_text();
  body["header_text"] = apply_placeholders(tmpl.header_text, render.placeholders);
  body["footer_text"] = apply_placeholders(tmpl.footer_text, render.placeholders);
  body["header_fields"] = render.header_fields;
  body["totals"] = render.totals;
  body["rows"] = render.rows;
  json_ok(res, body);
}

}  // namespace

void register_print_routes(httplib::Server& server, Services& services) {
  server.Get("/api/print/targets",
             guard("print.template.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     (void)req;
                     (void)services;
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& target : print_targets()) {
                       nlohmann::json columns = nlohmann::json::array();
                       for (const auto& column : target.columns) {
                         columns.push_back(def_column_to_json(column));
                       }
                       list.push_back({{"type", target.type},
                                       {"key", target.key},
                                       {"name", target.name},
                                       {"columns", columns},
                                       {"builtin", template_to_json(
                                                       builtin_print_template(target.type,
                                                                              target.key))}});
                     }
                     json_ok(res, list);
                   }));

  server.Get("/api/print-templates",
             guard("print.template.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const auto rows = services.print_templates.list(param_text(req, "target_type"),
                                                                     param_text(req, "target_key"));
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : rows) {
                       list.push_back(template_to_json(row));
                     }
                     json_ok(res, list);
                   }));

  server.Get(R"(/api/print-templates/(\d+))",
             guard("print.template.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const auto id = std::stoll(req.matches[1].str());
                     json_ok(res, template_to_json(services.print_templates.get(id)));
                   }));

  server.Post("/api/print-templates",
              guard("print.template.write",
                    [&services](const httplib::Request& req, httplib::Response& res) {
                      const auto created =
                          services.print_templates.create(template_from_json(json_body(req)));
                      json_ok(res, template_to_json(created));
                    }));

  server.Put(R"(/api/print-templates/(\d+))",
             guard("print.template.write",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const auto id = std::stoll(req.matches[1].str());
                     const auto updated = services.print_templates.update(
                         id, template_from_json(json_body(req)));
                     json_ok(res, template_to_json(updated));
                   }));

  server.Delete(R"(/api/print-templates/(\d+))",
                guard("print.template.write",
                      [&services](const httplib::Request& req, httplib::Response& res) {
                        const auto id = std::stoll(req.matches[1].str());
                        services.print_templates.remove(id);
                        json_ok(res, nlohmann::json{{"ok", true}});
                      }));

  server.Post(R"(/api/print-templates/(\d+)/default)",
              guard("print.template.write",
                    [&services](const httplib::Request& req, httplib::Response& res) {
                      const auto id = std::stoll(req.matches[1].str());
                      json_ok(res, template_to_json(services.print_templates.set_default(id)));
                    }));

  // 出数入口：权限按目标数据本身判定，模板管理权限不参与。
  server.Get("/api/print/render", [&services](const httplib::Request& req,
                                              httplib::Response& res) {
    respond_render(services, req, res);
  });
}

}  // namespace xiaotu
