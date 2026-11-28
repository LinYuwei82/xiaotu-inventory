#include <httplib.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

#include "xiaotu/error.h"
#include "xiaotu/http_guard.h"
#include "xiaotu/http_json.h"
#include "xiaotu/qty.h"
#include "xiaotu/report_service.h"
#include "xiaotu/services.h"

#include "report_common.h"

namespace xiaotu {
namespace {

using namespace report_common;

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
  const std::string base = result.direction == -1 ? "付款" : "收款";
  return line.is_reverse ? base + "撤销" : base;
}

std::string statement_summary(const StatementResult& result, const StatementLine& line) {
  if (line.kind == "DOC") return doc_type_label(line.doc_type);
  if (!line.is_reverse && !line.note.empty()) return line.note;
  std::string summary;
  if (line.is_reverse) {
    summary = std::string("撤销") + (result.direction == -1 ? "付款" : "收款");
    if (!line.reverses_no.empty()) summary += "（原单 " + line.reverses_no + "）";
  } else {
    summary = statement_kind_label(result, line);
  }
  if (!line.note.empty()) summary += "；" + line.note;
  return summary;
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
        {"is_reverse", line.is_reverse},
        {"reverses_no", line.reverses_no},
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
      {"settle_net_cents", result.settle_net_cents},
      {"reverse_cents", result.reverse_cents},
      {"reverse_count", result.reverse_count},
      {"lines", lines},
  };
}

}  // namespace

void register_report_finance_routes(httplib::Server& server, Services& services) {
  server.Get("/api/reports/statement",
             guard("finance.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const std::string partner_key =
                         req.has_param("partner_key") ? req.get_param_value("partner_key") : "";
                     if (partner_key.empty()) {
                       throw AppError("INVALID_INPUT", "请指定往来单位");
                     }
                     // 方向：显式参数优先；未传时按伙伴角色（P:）或台账方向推断，C:/T: 应收、S: 应付。
                     int direction = 1;
                     const std::string direction_param =
                         req.has_param("direction") ? req.get_param_value("direction") : "";
                     if (!direction_param.empty()) {
                       direction = direction_param == "-1" || direction_param == "AP" ? -1 : 1;
                     } else {
                       services.db.read([&](Conn& conn) {
                         direction = partner_key_direction(conn, partner_key);
                       });
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
                                        "；仅统计 v7 迁移后审核的单据，已反审核单据不计入；"
                                        "撤销收付款按撤销当天入账，标注原单并单列净额");
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
                       csv_row(csv, {"其中：撤销收付款", "",
                                     std::to_string(result.reverse_count) + " 笔", "",
                                     yuan(result.reverse_cents), "", ""});
                       csv_row(csv, {"期间核销净额", "", "",
                                     (direction == -1 ? "付款" : "收款") + std::string(" − 撤销回冲"),
                                     "", yuan(result.settle_net_cents), ""});
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
                                         "；仅统计 v7 迁移后审核的单据，已反审核单据不计入；"
                                         "撤销收付款按撤销当天入账，标注原单并单列净额");
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
                       sheet.add_row({XlsxCell::text("其中：撤销收付款"), XlsxCell::text(""),
                                      XlsxCell::text(std::to_string(result.reverse_count) + " 笔"),
                                      XlsxCell::text(""),
                                      XlsxCell::money(result.reverse_cents),
                                      XlsxCell::text(""), XlsxCell::text("")});
                       sheet.add_row({XlsxCell::text("期间核销净额"), XlsxCell::text(""),
                                      XlsxCell::text(""),
                                      XlsxCell::text((direction == -1 ? "付款" : "收款") +
                                                     std::string(" − 撤销回冲")),
                                      XlsxCell::text(""),
                                      XlsxCell::money(result.settle_net_cents),
                                      XlsxCell::text("")});
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
                     // 合计行按当前截止日的全量分组求和。
                     std::int64_t aging_total = 0;
                     std::int64_t aging_not_due = 0;
                     std::int64_t aging_d1_30 = 0;
                     std::int64_t aging_d31_60 = 0;
                     std::int64_t aging_d61_90 = 0;
                     std::int64_t aging_d90_plus = 0;
                     for (const auto& row : rows) {
                       aging_total += row.total_cents;
                       aging_not_due += row.not_due_cents;
                       aging_d1_30 += row.d1_30_cents;
                       aging_d31_60 += row.d31_60_cents;
                       aging_d61_90 += row.d61_90_cents;
                       aging_d90_plus += row.d90_plus_cents;
                     }
                     res.set_header(kArApScopeHeader, kArApScope);
                     if (csv_requested(req)) {
                       std::string csv;
                       csv_preamble(csv, "应收应付余额与账龄",
                                    std::string("方向 ") + (direction == -1 ? "应付" : "应收") +
                                        "；截止日 " + as_of +
                                        "；仅统计 v7 迁移后审核的单据"
                                        "；按截止日时点还原：业务日期晚于截止日不计入，"
                                        "核销按核销日期还原");
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
                       csv_row(csv, {"合计", "", yuan(aging_total), yuan(aging_not_due),
                                     yuan(aging_d1_30), yuan(aging_d31_60),
                                     yuan(aging_d61_90), yuan(aging_d90_plus)});
                       send_csv(res, "ar-ap-aging-" + date_stamp() + ".csv", csv);
                       return;
                     }
                     if (xlsx_requested(req)) {
                       XlsxSheet sheet("应收应付余额与账龄");
                       xlsx_preamble(sheet, "应收应付余额与账龄",
                                     std::string("方向 ") + (direction == -1 ? "应付" : "应收") +
                                         "；截止日 " + as_of +
                                         "；仅统计 v7 迁移后审核的单据"
                                         "；按截止日时点还原：业务日期晚于截止日不计入，"
                                         "核销按核销日期还原");
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
                       sheet.add_row({XlsxCell::text("合计"), XlsxCell::text(""),
                                      XlsxCell::money(aging_total),
                                      XlsxCell::money(aging_not_due),
                                      XlsxCell::money(aging_d1_30),
                                      XlsxCell::money(aging_d31_60),
                                      XlsxCell::money(aging_d61_90),
                                      XlsxCell::money(aging_d90_plus)});
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
                     // account_id：数字为指定账户，0 为历史「未指定账户」，all 为全部账户。
                     std::int64_t account_id = 0;
                     if (req.has_param("account_id")) {
                       const std::string value = req.get_param_value("account_id");
                       account_id = value == "all" ? -1 : parse_id(value);
                     }
                     const std::string from =
                         req.has_param("from") ? req.get_param_value("from") : "";
                     const std::string to =
                         req.has_param("to") ? req.get_param_value("to") : "";
                     const std::string format =
                         req.has_param("format") ? req.get_param_value("format") : "json";
                     const auto rows = services.settlements.ledger(account_id, from, to);
                     const auto direction_text = [](const FundLedgerRow& row) -> std::string {
                       if (row.opening) return "期初";
                       // 撤销行金额为负，列内只显示绝对值，靠类型区分「撤销收款/付款」。
                       const bool reversed = row.note.rfind("撤销", 0) == 0;
                       if (row.direction == 1) return reversed ? "撤销收款" : "收款";
                       return reversed ? "撤销付款" : "付款";
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
                     const auto rows = services.settlements.account_balances(as_of);
                     const std::string scope =
                         "截至 " + (as_of.empty() ? std::string("今天") : as_of) +
                         "；期初 + 累计收入 − 累计支出";
                     if (csv_requested(req)) {
                       std::string csv;
                       csv_preamble(csv, "账户余额一览", scope);
                       csv_row(csv, {"账户编码", "账户名称", "类型", "期初（元）",
                                     "累计收入（元）", "累计支出（元）", "余额（元）"});
                       for (const auto& row : rows) {
                         csv_row(csv, {row.code, row.name, method_label(row.kind),
                                       yuan(row.opening_cents), yuan(row.inflow_cents),
                                       yuan(row.outflow_cents), yuan(row.balance_cents)});
                       }
                       send_csv(res, "fund-balances-" + date_stamp() + ".csv", csv);
                       return;
                     }
                     if (xlsx_requested(req)) {
                       XlsxSheet sheet("账户余额一览");
                       xlsx_preamble(sheet, "账户余额一览", scope);
                       sheet.add_header_row({"账户编码", "账户名称", "类型", "期初（元）",
                                             "累计收入（元）", "累计支出（元）", "余额（元）"});
                       for (const auto& row : rows) {
                         sheet.add_row({XlsxCell::text(row.code), XlsxCell::text(row.name),
                                        XlsxCell::text(method_label(row.kind)),
                                        XlsxCell::money(row.opening_cents),
                                        XlsxCell::money(row.inflow_cents),
                                        XlsxCell::money(row.outflow_cents),
                                        XlsxCell::money(row.balance_cents)});
                       }
                       send_xlsx(res, "fund-balances-" + date_stamp() + ".xlsx", sheet.build());
                       return;
                     }
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : rows) {
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
}

}  // namespace xiaotu
