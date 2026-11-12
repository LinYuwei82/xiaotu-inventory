#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "xiaotu/error.h"
#include "xiaotu/http_guard.h"
#include "xiaotu/http_json.h"
#include "xiaotu/models.h"
#include "xiaotu/qty.h"
#include "xiaotu/report_service.h"
#include "xiaotu/services.h"
#include "xiaotu/xlsx.h"

namespace xiaotu {
namespace {

// 账龄口径与应收应付台账一致：只统计 v7 迁移后审核的单据。
constexpr const char* kArApScopeHeader = "X-Xiaotu-Data-Scope";
constexpr const char* kArApScope = "ar_ap_since_v7";

ReportFilter parse_filter(const httplib::Request& req) {
  ReportFilter filter;
  if (req.has_param("from")) filter.from = req.get_param_value("from");
  if (req.has_param("to")) filter.to = req.get_param_value("to");
  if (req.has_param("item_id")) filter.item_id = std::stoll(req.get_param_value("item_id"));
  if (req.has_param("warehouse_id")) {
    filter.warehouse_id = std::stoll(req.get_param_value("warehouse_id"));
  }
  if (req.has_param("customer_id")) {
    filter.customer_id = std::stoll(req.get_param_value("customer_id"));
  }
  if (req.has_param("supplier_id")) {
    filter.supplier_id = std::stoll(req.get_param_value("supplier_id"));
  }
  if (req.has_param("limit")) filter.limit = std::stoi(req.get_param_value("limit"));
  return filter;
}

// ---------- CSV 导出 ----------

bool csv_requested(const httplib::Request& req) {
  return req.has_param("format") && req.get_param_value("format") == "csv";
}

bool xlsx_requested(const httplib::Request& req) {
  return req.has_param("format") && req.get_param_value("format") == "xlsx";
}

std::string csv_escape(const std::string& value) {
  if (value.find_first_of(",\"\r\n") == std::string::npos) return value;
  std::string out = "\"";
  for (const char ch : value) {
    if (ch == '"') out += "\"\"";
    else out += ch;
  }
  out += '"';
  return out;
}

void csv_row(std::string& out, std::initializer_list<std::string> fields) {
  bool first = true;
  for (const auto& field : fields) {
    if (!first) out += ',';
    first = false;
    out += csv_escape(field);
  }
  out += "\r\n";
}

// 金额统一按「元、两位小数」输出；从整数分换算，避免浮点误差。
std::string yuan(std::int64_t cents) {
  const bool negative = cents < 0;
  const std::int64_t abs = negative ? -cents : cents;
  std::string fraction = std::to_string(abs % 100);
  if (fraction.size() < 2) fraction.insert(0, 2 - fraction.size(), '0');
  return (negative ? "-" : "") + std::to_string(abs / 100) + "." + fraction;
}

std::string period_label(const ReportFilter& filter) {
  if (filter.from.empty() && filter.to.empty()) return "全部期间";
  return (filter.from.empty() ? std::string("起始不限") : filter.from) + " 至 " +
         (filter.to.empty() ? std::string("截止不限") : filter.to);
}

std::string dimension_label(std::int64_t id, const char* name) {
  return id == 0 ? std::string(name) + " 全部"
                 : std::string(name) + " ID " + std::to_string(id);
}

void csv_preamble(std::string& out, const std::string& title, const std::string& scope) {
  csv_row(out, {"报表", title});
  csv_row(out, {"筛选条件", scope});
  csv_row(out, {"导出时间", now_text()});
  csv_row(out, {"金额说明",
                "金额单位为元，由整数分按 1 元 = 100 分换算并保留两位小数，无浮点误差"});
  out += "\r\n";
}

void send_csv(httplib::Response& res, const std::string& filename, const std::string& body) {
  res.set_header("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  res.set_content("\xEF\xBB\xBF" + body, "text/csv; charset=utf-8");
}

void xlsx_preamble(XlsxSheet& sheet, const std::string& title, const std::string& scope) {
  sheet.add_text_row({"报表", title});
  sheet.add_text_row({"筛选条件", scope});
  sheet.add_text_row({"导出时间", now_text()});
  sheet.add_text_row({"金额说明",
                      "金额单位为元，由整数分按 1 元 = 100 分换算并保留两位小数，无浮点误差"});
  sheet.add_text_row({});
}

void send_xlsx(httplib::Response& res, const std::string& filename, const std::string& body) {
  res.set_header("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  res.set_content(body,
                  "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet");
}

std::string date_stamp() {
  std::string stamp = now_text().substr(0, 10);
  stamp.erase(std::remove(stamp.begin(), stamp.end(), '-'), stamp.end());
  return stamp;
}

nlohmann::json balance_to_json(const StockBalanceRow& row) {
  return nlohmann::json{
      {"item_id", row.item_id},
      {"item_code", row.item_code},
      {"item_name", row.item_name},
      {"unit", row.unit},
      {"warehouse_code", row.warehouse_code},
      {"warehouse_name", row.warehouse_name},
      {"qty", qty_text(row.qty)},
      {"amount_cents", row.amount_cents},
  };
}

nlohmann::json ledger_to_json(const LedgerDetailRow& row) {
  return nlohmann::json{
      {"id", row.id},
      {"biz_time", row.biz_time},
      {"biz_type", row.biz_type},
      {"biz_category", row.biz_category},
      {"doc_no", row.doc_no},
      {"item_code", row.item_code},
      {"item_name", row.item_name},
      {"warehouse_name", row.warehouse_name},
      {"qty", qty_text(row.qty)},
      {"amount_cents", row.amount_cents},
  };
}

nlohmann::json flow_to_json(const GoodsFlowRow& row) {
  return nlohmann::json{
      {"item_id", row.item_id},
      {"item_code", row.item_code},
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
  };
}

nlohmann::json summary_to_json(const InventorySummaryRow& row) {
  return nlohmann::json{
      {"item_id", row.item_id},
      {"item_code", row.item_code},
      {"item_name", row.item_name},
      {"unit", row.unit},
      {"opening_qty", qty_text(row.opening_qty)},
      {"in_qty", qty_text(row.in_qty)},
      {"out_qty", qty_text(row.out_qty)},
      {"closing_qty", qty_text(row.closing_qty)},
      {"opening_amount_cents", row.opening_amount_cents},
      {"in_amount_cents", row.in_amount_cents},
      {"out_amount_cents", row.out_amount_cents},
      {"closing_amount_cents", row.closing_amount_cents},
  };
}

nlohmann::json aging_to_json(const AgingRow& row) {
  return nlohmann::json{
      {"partner_key", row.partner_key},
      {"partner_name", row.partner_name},
      {"direction", row.direction},
      {"total_cents", row.total_cents},
      {"not_due_cents", row.not_due_cents},
      {"d1_30_cents", row.d1_30_cents},
      {"d31_60_cents", row.d31_60_cents},
      {"d61_90_cents", row.d61_90_cents},
      {"d90_plus_cents", row.d90_plus_cents},
  };
}

std::string method_label(const std::string& method) {
  if (method == "CASH") return "现金";
  if (method == "BANK") return "银行";
  if (method == "WECHAT") return "微信";
  if (method == "ALIPAY") return "支付宝";
  if (method == "OTHER") return "其他";
  return method.empty() ? "未记录" : method;
}

std::string margin_text(std::int64_t revenue_cents, std::int64_t profit_cents) {
  if (revenue_cents == 0) return "—";
  // 毛利率保留一位小数，用整数千分比换算，展示层也不引入浮点。
  const std::int64_t direction = revenue_cents < 0 ? -1 : 1;
  const std::int64_t revenue = revenue_cents < 0 ? -revenue_cents : revenue_cents;
  const std::int64_t profit = profit_cents * direction;
  const std::int64_t negative = profit < 0 ? -1 : 1;
  const std::int64_t absolute = profit < 0 ? -profit : profit;
  const std::int64_t tenths = (absolute * 1000 + revenue / 2) / revenue;
  return (negative < 0 ? "-" : "") + std::to_string(tenths / 10) + "." +
         std::to_string(tenths % 10) + "%";
}

nlohmann::json profit_to_json(const SalesProfitRow& row) {
  return nlohmann::json{
      {"item_id", row.item_id},
      {"item_code", row.item_code},
      {"item_name", row.item_name},
      {"unit", row.unit},
      {"qty", qty_text(row.qty)},
      {"revenue_cents", row.revenue_cents},
      {"cost_cents", row.cost_cents},
      {"profit_cents", row.profit_cents},
  };
}

std::string biz_category_label(const std::string& category) {
  if (category == "GIFT") return "赠品";
  if (category == "USAGE") return "领用";
  if (category == "LOSS") return "报损";
  if (category == "OPENING") return "期初";
  if (category == "OTHER") return "其他";
  return category;
}

// 采购报表口径说明：入库与退货同表，退货按负数冲减，只统计已审核单据。
std::string purchase_scope(const ReportFilter& filter) {  return period_label(filter) + "；" + dimension_label(filter.supplier_id, "供应商") + "；" +
         dimension_label(filter.item_id, "商品") + "；" +
         dimension_label(filter.warehouse_id, "仓库") +
         "；包含采购入库与采购退货，退货数量与金额按负数冲减，仅统计已审核单据";
}

nlohmann::json other_flow_to_json(const OtherFlowSummaryRow& row) {
  return nlohmann::json{
      {"category", row.category},
      {"in_qty", qty_text(row.in_qty)},
      {"in_amount_cents", row.in_amount_cents},
      {"out_qty", qty_text(row.out_qty)},
      {"out_amount_cents", row.out_amount_cents},
  };
}

std::string other_flow_scope(const ReportFilter& filter) {
  return period_label(filter) + "；" +
         dimension_label(filter.warehouse_id, "仓库") + "；业务类型 " +
         (filter.biz_category.empty() ? std::string("全部")
                                      : biz_category_label(filter.biz_category)) +
         "；仅统计已审核的其他出入库，出库数量与金额取绝对值";
}

nlohmann::json purchase_detail_to_json(const PurchaseDetailRow& row) {
  return nlohmann::json{
      {"date", row.date},
      {"doc_no", row.doc_no},
      {"kind", row.kind},
      {"supplier", row.supplier},
      {"item_id", row.item_id},
      {"item_code", row.item_code},
      {"item_name", row.item_name},
      {"spec", row.spec},
      {"unit", row.unit},
      {"warehouse_name", row.warehouse_name},
      {"qty", qty_text(row.qty)},
      {"price_cents", row.price_cents},
      {"amount_cents", row.amount_cents},
  };
}

nlohmann::json purchase_supplier_to_json(const PurchaseSupplierSummaryRow& row) {
  return nlohmann::json{
      {"partner_id", row.partner_id},
      {"supplier", row.supplier},
      {"qty", qty_text(row.qty)},
      {"amount_cents", row.amount_cents},
      {"avg_price_cents", row.avg_price_cents},
  };
}

nlohmann::json purchase_item_to_json(const PurchaseItemSummaryRow& row) {
  return nlohmann::json{
      {"item_id", row.item_id},
      {"item_code", row.item_code},
      {"item_name", row.item_name},
      {"spec", row.spec},
      {"unit", row.unit},
      {"qty", qty_text(row.qty)},
      {"amount_cents", row.amount_cents},
      {"avg_price_cents", row.avg_price_cents},
  };
}

std::string doc_type_label(const std::string& doc_type) {
  if (doc_type == "SAL_OUT") return "销售出库";
  if (doc_type == "SAL_RET") return "销售退货";
  if (doc_type == "PUR_IN") return "采购入库";
  if (doc_type == "PUR_RET") return "采购退货";
  return doc_type;
}

// 库存流水的类型标签：盘盈/盘亏与各种 *_REVERSE 统一转成中文，导出与页面口径一致。
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

std::string statement_kind_label(const StatementResult& result, const StatementLine& line) {
  if (line.kind == "DOC") return "单据";
  return result.direction == -1 ? "付款" : "收款";
}

std::string statement_summary(const StatementResult& result, const StatementLine& line) {
  if (line.kind == "DOC") return doc_type_label(line.doc_type);
  if (!line.note.empty()) return line.note;
  return statement_kind_label(result, line);
}

nlohmann::json statement_to_json(const StatementResult& result) {
  nlohmann::json lines = nlohmann::json::array();
  for (const auto& line : result.lines) {
    lines.push_back(nlohmann::json{
        {"date", line.date},
        {"kind", line.kind},
        {"kind_label", statement_kind_label(result, line)},
        {"doc_no", line.doc_no},
        {"summary", statement_summary(result, line)},
        {"increase_cents", line.increase_cents},
        {"decrease_cents", line.decrease_cents},
        {"balance_cents", line.balance_cents},
    });
  }
  return nlohmann::json{
      {"partner_key", result.partner_key},
      {"partner_name", result.partner_name},
      {"direction", result.direction},
      {"from", result.from},
      {"to", result.to},
      {"opening_cents", result.opening_cents},
      {"increase_cents", result.increase_cents},
      {"decrease_cents", result.decrease_cents},
      {"closing_cents", result.closing_cents},
      {"lines", lines},
  };
}

}  // namespace

void register_report_routes(httplib::Server& server, Services& services) {
  server.Get("/api/reports/stock-balance",
             guard("stock.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const auto rows = services.stock.balances();
                     if (csv_requested(req)) {
                       std::string csv;
                       csv_preamble(csv, "库存余额表", "全部仓库 / 全部商品");
                       csv_row(csv, {"商品编码", "商品名称", "单位", "仓库编码", "仓库名称",
                                     "数量", "金额（元）"});
                       for (const auto& row : rows) {
                         csv_row(csv, {row.item_code, row.item_name, row.unit,
                                       row.warehouse_code, row.warehouse_name,
                                       qty_text(row.qty), yuan(row.amount_cents)});
                       }
                       send_csv(res, "stock-balance-" + date_stamp() + ".csv", csv);
                       return;
                     }
                     if (xlsx_requested(req)) {
                       XlsxSheet sheet("库存余额表");
                       xlsx_preamble(sheet, "库存余额表", "全部仓库 / 全部商品");
                       sheet.add_header_row({"商品编码", "商品名称", "单位", "仓库编码",
                                             "仓库名称", "数量", "金额（元）"});
                       for (const auto& row : rows) {
                         sheet.add_row({XlsxCell::text(row.item_code),
                                        XlsxCell::text(row.item_name),
                                        XlsxCell::text(row.unit),
                                        XlsxCell::text(row.warehouse_code),
                                        XlsxCell::text(row.warehouse_name),
                                        XlsxCell::text(qty_text(row.qty)),
                                        XlsxCell::money(row.amount_cents)});
                       }
                       send_xlsx(res, "stock-balance-" + date_stamp() + ".xlsx", sheet.build());
                       return;
                     }
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : rows) {
                       list.push_back(balance_to_json(row));
                     }
                     json_ok(res, list);
                   }));

  server.Get("/api/reports/inventory-summary",
             guard("stock.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const ReportFilter filter = parse_filter(req);
                     const auto rows = services.reports.inventory_summary(filter);
                     if (csv_requested(req)) {
                       std::string csv;
                       csv_preamble(csv, "进销存汇总",
                                    period_label(filter) + "；" +
                                        dimension_label(filter.item_id, "商品") + "；" +
                                        dimension_label(filter.warehouse_id, "仓库") +
                                        "；按商品跨仓库汇总");
                       csv_row(csv, {"商品编码", "商品名称", "单位", "期初数量",
                                     "期初金额（元）", "入库数量", "入库金额（元）",
                                     "出库数量", "出库金额（元）", "期末数量",
                                     "期末金额（元）"});
                       for (const auto& row : rows) {
                         csv_row(csv, {row.item_code, row.item_name, row.unit,
                                       qty_text(row.opening_qty),
                                       yuan(row.opening_amount_cents),
                                       qty_text(row.in_qty), yuan(row.in_amount_cents),
                                       qty_text(row.out_qty), yuan(row.out_amount_cents),
                                       qty_text(row.closing_qty),
                                       yuan(row.closing_amount_cents)});
                       }
                       send_csv(res, "inventory-summary-" + date_stamp() + ".csv", csv);
                       return;
                     }
                     if (xlsx_requested(req)) {
                       XlsxSheet sheet("进销存汇总");
                       xlsx_preamble(sheet, "进销存汇总",
                                     period_label(filter) + "；" +
                                         dimension_label(filter.item_id, "商品") + "；" +
                                         dimension_label(filter.warehouse_id, "仓库") +
                                         "；按商品跨仓库汇总");
                       sheet.add_header_row({"商品编码", "商品名称", "单位", "期初数量",
                                             "期初金额（元）", "入库数量", "入库金额（元）",
                                             "出库数量", "出库金额（元）", "期末数量",
                                             "期末金额（元）"});
                       for (const auto& row : rows) {
                         sheet.add_row({XlsxCell::text(row.item_code),
                                        XlsxCell::text(row.item_name),
                                        XlsxCell::text(row.unit),
                                        XlsxCell::text(qty_text(row.opening_qty)),
                                        XlsxCell::money(row.opening_amount_cents),
                                        XlsxCell::text(qty_text(row.in_qty)),
                                        XlsxCell::money(row.in_amount_cents),
                                        XlsxCell::text(qty_text(row.out_qty)),
                                        XlsxCell::money(row.out_amount_cents),
                                        XlsxCell::text(qty_text(row.closing_qty)),
                                        XlsxCell::money(row.closing_amount_cents)});
                       }
                       send_xlsx(res, "inventory-summary-" + date_stamp() + ".xlsx",
                                 sheet.build());
                       return;
                     }
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : rows) {
                       list.push_back(summary_to_json(row));
                     }
                     json_ok(res, list);
                   }));

  server.Get("/api/reports/stock-ledger",
             guard("stock.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     ReportFilter filter = parse_filter(req);
                     const std::string flow =
                         req.has_param("flow") ? req.get_param_value("flow") : "";
                     if (flow == "in") filter.qty_direction = 1;
                     if (flow == "out") filter.qty_direction = -1;
                     if (req.has_param("biz_category")) {
                       filter.biz_category = req.get_param_value("biz_category");
                     }
                     if (csv_requested(req) || xlsx_requested(req)) filter.limit = 100000;
                     const auto rows = services.reports.stock_ledger(filter);
                     const std::string scope =
                         period_label(filter) + "；" +
                         dimension_label(filter.item_id, "商品") + "；" +
                         dimension_label(filter.warehouse_id, "仓库") + "；方向 " +
                         qty_direction_label(filter.qty_direction) + "；业务类型 " +
                         (filter.biz_category.empty() ? std::string("全部")
                                                      : biz_category_label(filter.biz_category));
                     if (csv_requested(req)) {
                       std::string csv;
                       csv_preamble(csv, "库存流水明细", scope);
                       csv_row(csv, {"时间", "类型", "业务类型", "单据号", "商品编码", "商品名称",
                                     "仓库", "数量", "金额（元）"});
                       for (const auto& row : rows) {
                         csv_row(csv, {row.biz_time, stock_biz_label(row.biz_type),
                                       biz_category_label(row.biz_category), row.doc_no,
                                       row.item_code, row.item_name, row.warehouse_name,
                                       qty_text(row.qty), yuan(row.amount_cents)});
                       }
                       send_csv(res, "stock-ledger-" + date_stamp() + ".csv", csv);
                       return;
                     }
                     if (xlsx_requested(req)) {
                       XlsxSheet sheet("库存流水明细");
                       xlsx_preamble(sheet, "库存流水明细", scope);
                       sheet.add_header_row({"时间", "类型", "业务类型", "单据号", "商品编码",
                                             "商品名称", "仓库", "数量", "金额（元）"});
                       for (const auto& row : rows) {
                         sheet.add_row({XlsxCell::text(row.biz_time),
                                        XlsxCell::text(stock_biz_label(row.biz_type)),
                                        XlsxCell::text(biz_category_label(row.biz_category)),
                                        XlsxCell::text(row.doc_no),
                                        XlsxCell::text(row.item_code),
                                        XlsxCell::text(row.item_name),
                                        XlsxCell::text(row.warehouse_name),
                                        XlsxCell::text(qty_text(row.qty)),
                                        XlsxCell::money(row.amount_cents)});
                       }
                       send_xlsx(res, "stock-ledger-" + date_stamp() + ".xlsx", sheet.build());
                       return;
                     }
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : rows) {
                       list.push_back(ledger_to_json(row));
                     }
                     json_ok(res, list);
                   }));

  server.Get("/api/reports/goods-flow",
             guard("stock.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const ReportFilter filter = parse_filter(req);
                     const auto rows = services.reports.goods_flow(filter);
                     if (csv_requested(req)) {
                       std::string csv;
                       csv_preamble(csv, "收发明细",
                                    period_label(filter) + "；" +
                                        dimension_label(filter.warehouse_id, "仓库"));
                       csv_row(csv, {"商品编码", "商品名称", "单位", "仓库编码", "仓库名称",
                                     "期初数量", "收入数量", "发出数量", "期末数量",
                                     "收入金额（元）", "发出金额（元）"});
                       for (const auto& row : rows) {
                         csv_row(csv, {row.item_code, row.item_name, row.unit,
                                       row.warehouse_code, row.warehouse_name,
                                       qty_text(row.opening_qty),
                                       qty_text(row.in_qty), qty_text(row.out_qty),
                                       qty_text(row.closing_qty),
                                       yuan(row.in_amount_cents), yuan(row.out_amount_cents)});
                       }
                       send_csv(res, "goods-flow-" + date_stamp() + ".csv", csv);
                       return;
                     }
                     if (xlsx_requested(req)) {
                       XlsxSheet sheet("收发明细");
                       xlsx_preamble(sheet, "收发明细",
                                     period_label(filter) + "；" +
                                         dimension_label(filter.warehouse_id, "仓库"));
                       sheet.add_header_row({"商品编码", "商品名称", "单位", "仓库编码",
                                             "仓库名称", "期初数量", "收入数量", "发出数量",
                                             "期末数量", "收入金额（元）", "发出金额（元）"});
                       for (const auto& row : rows) {
                         sheet.add_row({XlsxCell::text(row.item_code),
                                        XlsxCell::text(row.item_name),
                                        XlsxCell::text(row.unit),
                                        XlsxCell::text(row.warehouse_code),
                                        XlsxCell::text(row.warehouse_name),
                                        XlsxCell::text(qty_text(row.opening_qty)),
                                        XlsxCell::text(qty_text(row.in_qty)),
                                        XlsxCell::text(qty_text(row.out_qty)),
                                        XlsxCell::text(qty_text(row.closing_qty)),
                                        XlsxCell::money(row.in_amount_cents),
                                        XlsxCell::money(row.out_amount_cents)});
                       }
                       send_xlsx(res, "goods-flow-" + date_stamp() + ".xlsx", sheet.build());
                       return;
                     }
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : rows) {
                       list.push_back(flow_to_json(row));
                     }
                     json_ok(res, list);
                   }));

  server.Get("/api/reports/sales-profit",
             guard("finance.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const ReportFilter filter = parse_filter(req);
                     const auto rows = services.reports.sales_profit(filter);
                     if (csv_requested(req)) {
                       std::string csv;
                       csv_preamble(csv, "销售毛利",
                                    period_label(filter) + "；" +
                                        dimension_label(filter.item_id, "商品") +
                                        "；客户 " +
                                        (filter.customer_id == 0
                                             ? std::string("全部")
                                             : "ID " + std::to_string(filter.customer_id)) +
                                        "；收入取单据金额，成本取审核写入的库存流水，"
                                        "已反审核单据不计入");
                       csv_row(csv, {"商品编码", "商品名称", "单位", "销售数量",
                                     "销售收入（元）", "销售成本（元）", "毛利（元）", "毛利率"});
                       for (const auto& row : rows) {
                         csv_row(csv, {row.item_code, row.item_name, row.unit,
                                       qty_text(row.qty), yuan(row.revenue_cents),
                                       yuan(row.cost_cents), yuan(row.profit_cents),
                                       margin_text(row.revenue_cents, row.profit_cents)});
                       }
                       send_csv(res, "sales-profit-" + date_stamp() + ".csv", csv);
                       return;
                     }
                     if (xlsx_requested(req)) {
                       XlsxSheet sheet("销售毛利");
                       xlsx_preamble(sheet, "销售毛利",
                                     period_label(filter) + "；" +
                                         dimension_label(filter.item_id, "商品") + "；客户 " +
                                         (filter.customer_id == 0
                                              ? std::string("全部")
                                              : "ID " + std::to_string(filter.customer_id)) +
                                         "；收入取单据金额，成本取审核写入的库存流水，"
                                         "已反审核单据不计入");
                       sheet.add_header_row({"商品编码", "商品名称", "单位", "销售数量",
                                             "销售收入（元）", "销售成本（元）", "毛利（元）",
                                             "毛利率"});
                       for (const auto& row : rows) {
                         sheet.add_row({XlsxCell::text(row.item_code),
                                        XlsxCell::text(row.item_name),
                                        XlsxCell::text(row.unit),
                                        XlsxCell::text(qty_text(row.qty)),
                                        XlsxCell::money(row.revenue_cents),
                                        XlsxCell::money(row.cost_cents),
                                        XlsxCell::money(row.profit_cents),
                                        XlsxCell::text(
                                            margin_text(row.revenue_cents, row.profit_cents))});
                       }
                       send_xlsx(res, "sales-profit-" + date_stamp() + ".xlsx", sheet.build());
                       return;
                     }
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : rows) {
                       list.push_back(profit_to_json(row));
                     }
                     json_ok(res, list);
                   }));

  server.Get("/api/reports/purchase-detail",
             guard("finance.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     ReportFilter filter = parse_filter(req);
                     if (csv_requested(req) || xlsx_requested(req)) filter.limit = 100000;
                     const auto rows = services.reports.purchase_detail(filter);
                     if (csv_requested(req)) {
                       std::string csv;
                       csv_preamble(csv, "采购明细", purchase_scope(filter));
                       csv_row(csv, {"日期", "单号", "类型", "供应商", "商品编码", "商品名称",
                                     "规格", "单位", "仓库", "数量", "单价（元）", "金额（元）"});
                       for (const auto& row : rows) {
                         csv_row(csv, {row.date, row.doc_no, row.kind, row.supplier,
                                       row.item_code, row.item_name, row.spec, row.unit,
                                       row.warehouse_name, qty_text(row.qty),
                                       yuan(row.price_cents), yuan(row.amount_cents)});
                       }
                       send_csv(res, "purchase-detail-" + date_stamp() + ".csv", csv);
                       return;
                     }
                     if (xlsx_requested(req)) {
                       XlsxSheet sheet("采购明细");
                       xlsx_preamble(sheet, "采购明细", purchase_scope(filter));
                       sheet.add_header_row({"日期", "单号", "类型", "供应商", "商品编码",
                                             "商品名称", "规格", "单位", "仓库", "数量",
                                             "单价（元）", "金额（元）"});
                       for (const auto& row : rows) {
                         sheet.add_row({XlsxCell::text(row.date),
                                        XlsxCell::text(row.doc_no),
                                        XlsxCell::text(row.kind),
                                        XlsxCell::text(row.supplier),
                                        XlsxCell::text(row.item_code),
                                        XlsxCell::text(row.item_name),
                                        XlsxCell::text(row.spec),
                                        XlsxCell::text(row.unit),
                                        XlsxCell::text(row.warehouse_name),
                                        XlsxCell::text(qty_text(row.qty)),
                                        XlsxCell::money(row.price_cents),
                                        XlsxCell::money(row.amount_cents)});
                       }
                       send_xlsx(res, "purchase-detail-" + date_stamp() + ".xlsx",
                                 sheet.build());
                       return;
                     }
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : rows) {
                       list.push_back(purchase_detail_to_json(row));
                     }
                     json_ok(res, list);
                   }));

  // 采购汇总：by=supplier（默认）按供应商名称升序，by=item 按商品编码升序。
  server.Get("/api/reports/purchase-summary",
             guard("finance.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const ReportFilter filter = parse_filter(req);
                     const std::string by =
                         req.has_param("by") ? req.get_param_value("by") : std::string("supplier");
                     const bool by_item = by == "item";
                     const std::string scope = purchase_scope(filter) + "；维度 " +
                                               (by_item ? "按商品" : "按供应商") +
                                               "；均价 = 净金额 ÷ 净数量";

                     if (by_item) {
                       const auto rows = services.reports.purchase_summary_by_item(filter);
                       if (csv_requested(req)) {
                         std::string csv;
                         csv_preamble(csv, "采购汇总（按商品）", scope);
                         csv_row(csv, {"商品编码", "商品名称", "规格", "单位", "数量",
                                       "金额（元）", "均价（元）"});
                         for (const auto& row : rows) {
                           csv_row(csv, {row.item_code, row.item_name, row.spec, row.unit,
                                         qty_text(row.qty), yuan(row.amount_cents),
                                         yuan(row.avg_price_cents)});
                         }
                         send_csv(res, "purchase-summary-item-" + date_stamp() + ".csv", csv);
                         return;
                       }
                       if (xlsx_requested(req)) {
                         XlsxSheet sheet("采购汇总（按商品）");
                         xlsx_preamble(sheet, "采购汇总（按商品）", scope);
                         sheet.add_header_row({"商品编码", "商品名称", "规格", "单位", "数量",
                                               "金额（元）", "均价（元）"});
                         for (const auto& row : rows) {
                           sheet.add_row({XlsxCell::text(row.item_code),
                                          XlsxCell::text(row.item_name),
                                          XlsxCell::text(row.spec),
                                          XlsxCell::text(row.unit),
                                          XlsxCell::text(qty_text(row.qty)),
                                          XlsxCell::money(row.amount_cents),
                                          XlsxCell::money(row.avg_price_cents)});
                         }
                         send_xlsx(res, "purchase-summary-item-" + date_stamp() + ".xlsx",
                                   sheet.build());
                         return;
                       }
                       nlohmann::json list = nlohmann::json::array();
                       for (const auto& row : rows) {
                         list.push_back(purchase_item_to_json(row));
                       }
                       json_ok(res, list);
                       return;
                     }

                     const auto rows = services.reports.purchase_summary_by_supplier(filter);
                     if (csv_requested(req)) {
                       std::string csv;
                       csv_preamble(csv, "采购汇总（按供应商）", scope);
                       csv_row(csv, {"供应商", "数量", "金额（元）", "均价（元）"});
                       for (const auto& row : rows) {
                         csv_row(csv, {row.supplier, qty_text(row.qty),
                                       yuan(row.amount_cents), yuan(row.avg_price_cents)});
                       }
                       send_csv(res, "purchase-summary-supplier-" + date_stamp() + ".csv", csv);
                       return;
                     }
                     if (xlsx_requested(req)) {
                       XlsxSheet sheet("采购汇总（按供应商）");
                       xlsx_preamble(sheet, "采购汇总（按供应商）", scope);
                       sheet.add_header_row({"供应商", "数量", "金额（元）", "均价（元）"});
                       for (const auto& row : rows) {
                         sheet.add_row({XlsxCell::text(row.supplier),
                                        XlsxCell::text(qty_text(row.qty)),
                                        XlsxCell::money(row.amount_cents),
                                        XlsxCell::money(row.avg_price_cents)});
                       }
                       send_xlsx(res, "purchase-summary-supplier-" + date_stamp() + ".xlsx",
                                 sheet.build());
                       return;
                     }
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : rows) {
                       list.push_back(purchase_supplier_to_json(row));
                     }
                     json_ok(res, list);
                   }));

  // 其他出入库按业务类型汇总：入库与出库分列，固定顺序赠品/期初/领用/报损/其他。
  server.Get("/api/reports/other-flow-summary",
             guard("stock.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     ReportFilter filter = parse_filter(req);
                     if (req.has_param("biz_category")) {
                       filter.biz_category = req.get_param_value("biz_category");
                     }
                     const auto rows = services.reports.other_flow_summary(filter);
                     const std::string scope = other_flow_scope(filter);
                     if (csv_requested(req)) {
                       std::string csv;
                       csv_preamble(csv, "其他出入库汇总", scope);
                       csv_row(csv, {"业务类型", "入库数量", "入库金额（元）", "出库数量",
                                     "出库金额（元）"});
                       for (const auto& row : rows) {
                         csv_row(csv, {biz_category_label(row.category), qty_text(row.in_qty),
                                       yuan(row.in_amount_cents), qty_text(row.out_qty),
                                       yuan(row.out_amount_cents)});
                       }
                       send_csv(res, "other-flow-summary-" + date_stamp() + ".csv", csv);
                       return;
                     }
                     if (xlsx_requested(req)) {
                       XlsxSheet sheet("其他出入库汇总");
                       xlsx_preamble(sheet, "其他出入库汇总", scope);
                       sheet.add_header_row({"业务类型", "入库数量", "入库金额（元）", "出库数量",
                                             "出库金额（元）"});
                       for (const auto& row : rows) {
                         sheet.add_row({XlsxCell::text(biz_category_label(row.category)),
                                        XlsxCell::text(qty_text(row.in_qty)),
                                        XlsxCell::money(row.in_amount_cents),
                                        XlsxCell::text(qty_text(row.out_qty)),
                                        XlsxCell::money(row.out_amount_cents)});
                       }
                       send_xlsx(res, "other-flow-summary-" + date_stamp() + ".xlsx",
                                 sheet.build());
                       return;
                     }
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : rows) {
                       list.push_back(other_flow_to_json(row));
                     }
                     json_ok(res, list);
                   }));

  server.Get("/api/reports/statement",
             guard("finance.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const std::string partner_key =
                         req.has_param("partner_key") ? req.get_param_value("partner_key") : "";
                     if (partner_key.empty()) {
                       throw AppError("INVALID_INPUT", "请指定往来单位");
                     }
                     int direction = partner_key.rfind("S:", 0) == 0 ? -1 : 1;
                     if (req.has_param("direction")) {
                       const std::string value = req.get_param_value("direction");
                       direction = value == "-1" || value == "AP" ? -1 : 1;
                     }
                     const std::string from =
                         req.has_param("from") ? req.get_param_value("from") : "";
                     const std::string to = req.has_param("to") ? req.get_param_value("to") : "";
                     const StatementResult result =
                         services.reports.partner_statement(partner_key, direction, from, to);
                     res.set_header(kArApScopeHeader, kArApScope);
                     if (csv_requested(req)) {
                       const std::string period = from.empty() && to.empty()
                                                      ? "全部期间"
                                                      : (from.empty() ? std::string("起始不限") : from) +
                                                            " 至 " +
                                                            (to.empty() ? std::string("截止不限") : to);
                       const std::string partner =
                           (result.partner_name.empty() ? std::string("未命名")
                                                        : result.partner_name) +
                           "（" + result.partner_key + "）";
                       std::string csv;
                       csv_preamble(csv, "对账单",
                                    std::string("往来单位 ") + partner + "；方向 " +
                                        (direction == -1 ? "应付" : "应收") + "；期间 " + period +
                                        "；仅统计 v7 迁移后审核的单据，已反审核单据不计入");
                       csv_row(csv, {"日期", "类型", "单号", "摘要", "增加（元）", "减少（元）",
                                     "余额（元）"});
                       csv_row(csv, {"期初余额", "", "", "", "", "",
                                     yuan(result.opening_cents)});
                       for (const auto& line : result.lines) {
                         csv_row(csv, {line.date, statement_kind_label(result, line),
                                       line.doc_no, statement_summary(result, line),
                                       yuan(line.increase_cents), yuan(line.decrease_cents),
                                       yuan(line.balance_cents)});
                       }
                       csv_row(csv, {"本期合计", "", "", "", yuan(result.increase_cents),
                                     yuan(result.decrease_cents), yuan(result.closing_cents)});
                       send_csv(res,
                                std::string("statement-") + (direction == -1 ? "ap" : "ar") + "-" +
                                    date_stamp() + ".csv",
                                csv);
                       return;
                     }
                     if (xlsx_requested(req)) {
                       const std::string period = from.empty() && to.empty()
                                                      ? "全部期间"
                                                      : (from.empty() ? std::string("起始不限") : from) +
                                                            " 至 " +
                                                            (to.empty() ? std::string("截止不限") : to);
                       const std::string partner =
                           (result.partner_name.empty() ? std::string("未命名")
                                                        : result.partner_name) +
                           "（" + result.partner_key + "）";
                       XlsxSheet sheet("对账单");
                       xlsx_preamble(sheet, "对账单",
                                     std::string("往来单位 ") + partner + "；方向 " +
                                         (direction == -1 ? "应付" : "应收") + "；期间 " + period +
                                         "；仅统计 v7 迁移后审核的单据，已反审核单据不计入");
                       sheet.add_header_row({"日期", "类型", "单号", "摘要", "增加（元）",
                                             "减少（元）", "余额（元）"});
                       sheet.add_row({XlsxCell::text("期初余额"), XlsxCell::text(""),
                                      XlsxCell::text(""), XlsxCell::text(""),
                                      XlsxCell::text(""), XlsxCell::text(""),
                                      XlsxCell::money(result.opening_cents)});
                       for (const auto& line : result.lines) {
                         sheet.add_row({XlsxCell::text(line.date),
                                        XlsxCell::text(statement_kind_label(result, line)),
                                        XlsxCell::text(line.doc_no),
                                        XlsxCell::text(statement_summary(result, line)),
                                        XlsxCell::money(line.increase_cents),
                                        XlsxCell::money(line.decrease_cents),
                                        XlsxCell::money(line.balance_cents)});
                       }
                       sheet.add_row({XlsxCell::text("本期合计"), XlsxCell::text(""),
                                      XlsxCell::text(""), XlsxCell::text(""),
                                      XlsxCell::money(result.increase_cents),
                                      XlsxCell::money(result.decrease_cents),
                                      XlsxCell::money(result.closing_cents)});
                       send_xlsx(res,
                                 std::string("statement-") + (direction == -1 ? "ap" : "ar") + "-" +
                                     date_stamp() + ".xlsx",
                                 sheet.build());
                       return;
                     }
                     nlohmann::json body = statement_to_json(result);
                     body["data_scope"] = kArApScope;
                     json_ok(res, body);
                   }));

  server.Get("/api/reports/ar-ap-aging",
             guard("finance.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     int direction = 0;
                     if (req.has_param("direction")) {
                       const std::string value = req.get_param_value("direction");
                       direction = value == "-1" || value == "AP" ? -1 : 1;
                     }
                     const std::string as_of = req.has_param("as_of")
                                                   ? req.get_param_value("as_of")
                                                   : today_text();
                     const auto rows = services.reports.ar_ap_aging(direction, as_of);
                     res.set_header(kArApScopeHeader, kArApScope);
                     if (csv_requested(req)) {
                       std::string csv;
                       csv_preamble(csv, "应收应付余额与账龄",
                                    std::string("方向 ") + (direction == -1 ? "应付" : "应收") +
                                        "；截止日 " + as_of +
                                        "；仅统计 v7 迁移后审核的单据");
                       csv_row(csv, {"往来单位", "方向", "合计（元）", "未到期（元）", "1-30 天（元）",
                                     "31-60 天（元）", "61-90 天（元）", "90 天以上（元）"});
                       for (const auto& row : rows) {
                         csv_row(csv, {row.partner_name.empty() ? row.partner_key
                                                               : row.partner_name,
                                       row.direction == -1 ? "应付" : "应收",
                                       yuan(row.total_cents), yuan(row.not_due_cents),
                                       yuan(row.d1_30_cents), yuan(row.d31_60_cents),
                                       yuan(row.d61_90_cents), yuan(row.d90_plus_cents)});
                       }
                       send_csv(res, "ar-ap-aging-" + date_stamp() + ".csv", csv);
                       return;
                     }
                     if (xlsx_requested(req)) {
                       XlsxSheet sheet("应收应付余额与账龄");
                       xlsx_preamble(sheet, "应收应付余额与账龄",
                                     std::string("方向 ") + (direction == -1 ? "应付" : "应收") +
                                         "；截止日 " + as_of +
                                         "；仅统计 v7 迁移后审核的单据");
                       sheet.add_header_row({"往来单位", "方向", "合计（元）", "未到期（元）",
                                             "1-30 天（元）", "31-60 天（元）", "61-90 天（元）",
                                             "90 天以上（元）"});
                       for (const auto& row : rows) {
                         sheet.add_row(
                             {XlsxCell::text(row.partner_name.empty() ? row.partner_key
                                                                      : row.partner_name),
                              XlsxCell::text(row.direction == -1 ? "应付" : "应收"),
                              XlsxCell::money(row.total_cents),
                              XlsxCell::money(row.not_due_cents),
                              XlsxCell::money(row.d1_30_cents),
                              XlsxCell::money(row.d31_60_cents),
                              XlsxCell::money(row.d61_90_cents),
                              XlsxCell::money(row.d90_plus_cents)});
                       }
                       send_xlsx(res, "ar-ap-aging-" + date_stamp() + ".xlsx", sheet.build());
                       return;
                     }
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : rows) {
                       list.push_back(aging_to_json(row));
                     }
                     json_ok(res, list);
                   }));

  // 资金流水（现金日记账）：包含期初行与逐行余额，按账户 + 日期筛选。
  server.Get("/api/reports/fund-ledger",
             guard("finance.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const std::int64_t account_id =
                         req.has_param("account_id") ? std::stoll(req.get_param_value("account_id"))
                                                     : 0;
                     const std::string from =
                         req.has_param("from") ? req.get_param_value("from") : "";
                     const std::string to =
                         req.has_param("to") ? req.get_param_value("to") : "";
                     const std::string format =
                         req.has_param("format") ? req.get_param_value("format") : "json";
                     const auto rows = services.settlements.ledger(account_id, from, to);
                     const auto direction_text = [](const FundLedgerRow& row) -> std::string {
                       if (row.opening) return "期初";
                       return row.direction == 1 ? "收款" : "付款";
                     };

                     if (format == "csv") {
                       std::string csv = "\xEF\xBB\xBF";
                       csv_row(csv, {"日期", "单号", "类型", "往来单位", "结算方式", "经办人",
                                     "摘要", "收入（元）", "支出（元）", "余额（元）"});
                       for (const auto& row : rows) {
                         csv_row(csv, {row.settled_on, row.settle_no, direction_text(row),
                                       row.partner_name, method_label(row.method),
                                       row.operator_name, row.note,
                                       row.opening ? std::string{}
                                                   : yuan(row.income_cents),
                                       row.opening ? std::string{}
                                                   : yuan(row.expense_cents),
                                       yuan(row.balance_cents)});
                       }
                       send_csv(res, "fund-ledger-" + date_stamp() + ".csv", csv);
                       return;
                     }

                     if (format == "xlsx") {
                       XlsxSheet sheet("资金流水");
                       sheet.add_header_row({"日期", "单号", "类型", "往来单位", "结算方式", "经办人",
                                             "摘要", "收入（元）", "支出（元）", "余额（元）"});
                       for (const auto& row : rows) {
                         sheet.add_row(
                             {XlsxCell::text(row.settled_on),
                              XlsxCell::text(row.settle_no),
                              XlsxCell::text(direction_text(row)),
                              XlsxCell::text(row.partner_name),
                              XlsxCell::text(method_label(row.method)),
                              XlsxCell::text(row.operator_name),
                              XlsxCell::text(row.note),
                              row.opening ? XlsxCell::text("") : XlsxCell::money(row.income_cents),
                              row.opening ? XlsxCell::text("") : XlsxCell::money(row.expense_cents),
                              XlsxCell::money(row.balance_cents)});
                       }
                       send_xlsx(res, "fund-ledger-" + date_stamp() + ".xlsx", sheet.build());
                       return;
                     }

                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : rows) {
                       list.push_back({
                           {"opening", row.opening},
                           {"settled_on", row.settled_on},
                           {"settle_no", row.settle_no},
                           {"direction", row.opening ? 0 : row.direction},
                           {"direction_text", direction_text(row)},
                           {"partner_name", row.partner_name},
                           {"method", row.method},
                           {"method_text", method_label(row.method)},
                           {"operator", row.operator_name},
                           {"note", row.note},
                           {"income_cents", row.income_cents},
                           {"expense_cents", row.expense_cents},
                           {"balance_cents", row.balance_cents},
                       });
                     }
                     json_ok(res, list);
                   }));

  // 账户余额一览：期初 + 累计收入 − 累计支出。
  server.Get("/api/reports/fund-balances",
             guard("finance.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const std::string as_of =
                         req.has_param("as_of") ? req.get_param_value("as_of") : "";
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : services.settlements.account_balances(as_of)) {
                       list.push_back({
                           {"id", row.id},
                           {"code", row.code},
                           {"name", row.name},
                           {"kind", row.kind},
                           {"kind_text", method_label(row.kind)},
                           {"opening_cents", row.opening_cents},
                           {"inflow_cents", row.inflow_cents},
                           {"outflow_cents", row.outflow_cents},
                           {"balance_cents", row.balance_cents},
                       });
                     }
                     json_ok(res, list);
                   }));

  // 成本重算只读核对：按流水重放移动加权平均，与账面结存比对，不写回。
  server.Get("/api/stock/cost-recalc",
             guard("stock.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const std::int64_t item_id =
                         req.has_param("item_id") ? std::stoll(req.get_param_value("item_id")) : 0;
                     const std::int64_t warehouse_id =
                         req.has_param("warehouse_id")
                             ? std::stoll(req.get_param_value("warehouse_id"))
                             : 0;
                     const CostRecalcResult result =
                         services.cost_recalc.recalculate(item_id, warehouse_id);
                     nlohmann::json differences = nlohmann::json::array();
                     for (const auto& row : result.differences) {
                       differences.push_back(
                           {{"item_id", row.item_id},
                            {"item_code", row.item_code},
                            {"item_name", row.item_name},
                            {"warehouse_id", row.warehouse_id},
                            {"warehouse_name", row.warehouse_name},
                            {"book_qty", qty_text(row.book_qty)},
                            {"book_amount_cents", row.book_amount_cents},
                            {"replay_qty", qty_text(row.replay_qty)},
                            {"replay_amount_cents", row.replay_amount_cents},
                            {"qty_diff", qty_text(row.qty_diff)},
                            {"amount_diff_cents", row.amount_diff_cents}});
                     }
                     json_ok(res, nlohmann::json{
                                      {"ledger_rows", result.ledger_rows},
                                      {"item_count", result.item_count},
                                      {"difference_count", result.difference_count},
                                      {"qty_difference_count", result.qty_difference_count},
                                      {"differences", differences},
                                  });
                   }));
}

}  // namespace xiaotu
