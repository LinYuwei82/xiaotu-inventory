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

void register_report_statement_routes(httplib::Server& server, Services& services) {
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


}

}  // namespace xiaotu
