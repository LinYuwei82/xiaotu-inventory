#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "xiaotu/error.h"
#include "xiaotu/http_guard.h"
#include "xiaotu/http_json.h"
#include "xiaotu/models.h"
#include "xiaotu/report_service.h"
#include "xiaotu/services.h"

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
  if (req.has_param("limit")) filter.limit = std::stoi(req.get_param_value("limit"));
  return filter;
}

// ---------- CSV 导出 ----------

bool csv_requested(const httplib::Request& req) {
  return req.has_param("format") && req.get_param_value("format") == "csv";
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
      {"qty", row.qty},
      {"amount_cents", row.amount_cents},
  };
}

nlohmann::json ledger_to_json(const LedgerDetailRow& row) {
  return nlohmann::json{
      {"id", row.id},
      {"biz_time", row.biz_time},
      {"biz_type", row.biz_type},
      {"doc_no", row.doc_no},
      {"item_code", row.item_code},
      {"item_name", row.item_name},
      {"warehouse_name", row.warehouse_name},
      {"qty", row.qty},
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
      {"opening_qty", row.opening_qty},
      {"in_qty", row.in_qty},
      {"out_qty", row.out_qty},
      {"closing_qty", row.closing_qty},
      {"in_amount_cents", row.in_amount_cents},
      {"out_amount_cents", row.out_amount_cents},
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

std::string doc_type_label(const std::string& doc_type) {
  if (doc_type == "SAL_OUT") return "销售出库";
  if (doc_type == "SAL_RET") return "销售退货";
  if (doc_type == "PUR_IN") return "采购入库";
  if (doc_type == "PUR_RET") return "采购退货";
  return doc_type;
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
                                       std::to_string(row.qty), yuan(row.amount_cents)});
                       }
                       send_csv(res, "stock-balance-" + date_stamp() + ".csv", csv);
                       return;
                     }
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : rows) {
                       list.push_back(balance_to_json(row));
                     }
                     json_ok(res, list);
                   }));

  server.Get("/api/reports/stock-ledger",
             guard("stock.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     ReportFilter filter = parse_filter(req);
                     if (csv_requested(req)) filter.limit = 100000;
                     const auto rows = services.reports.stock_ledger(filter);
                     if (csv_requested(req)) {
                       std::string csv;
                       csv_preamble(csv, "库存流水明细",
                                    period_label(filter) + "；" +
                                        dimension_label(filter.item_id, "商品") + "；" +
                                        dimension_label(filter.warehouse_id, "仓库"));
                       csv_row(csv, {"时间", "类型", "单据号", "商品编码", "商品名称", "仓库",
                                     "数量", "金额（元）"});
                       for (const auto& row : rows) {
                         csv_row(csv, {row.biz_time, row.biz_type, row.doc_no, row.item_code,
                                       row.item_name, row.warehouse_name,
                                       std::to_string(row.qty), yuan(row.amount_cents)});
                       }
                       send_csv(res, "stock-ledger-" + date_stamp() + ".csv", csv);
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
                                       std::to_string(row.opening_qty),
                                       std::to_string(row.in_qty), std::to_string(row.out_qty),
                                       std::to_string(row.closing_qty),
                                       yuan(row.in_amount_cents), yuan(row.out_amount_cents)});
                       }
                       send_csv(res, "goods-flow-" + date_stamp() + ".csv", csv);
                       return;
                     }
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : rows) {
                       list.push_back(flow_to_json(row));
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
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : rows) {
                       list.push_back(aging_to_json(row));
                     }
                     json_ok(res, list);
                   }));
}

}  // namespace xiaotu
