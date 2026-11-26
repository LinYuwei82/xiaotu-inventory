#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>

#include "xiaotu/document.h"
#include "xiaotu/context.h"
#include "xiaotu/csv.h"
#include "xiaotu/http_guard.h"
#include "xiaotu/http_json.h"
#include "xiaotu/permissions.h"
#include "xiaotu/qty.h"
#include "xiaotu/services.h"
#include "xiaotu/xlsx.h"

namespace xiaotu {
namespace {

// 成本调整单的审核 / 反审核用 cost.adjust；路由先做 document.write 粗筛，
// 这里再按单据类型补一次权限校验。起草与改草稿仍走 document.write，
// 等于「操作员提申请、管理员批」。
void require_doc_audit(const std::string& doc_type) {
  const DocumentDefinition* definition = find_document_definition(doc_type);
  if (definition != nullptr && definition->write_permission != "document.write" &&
      !role_has_permission(current_context().role, definition->write_permission)) {
    throw AppError("FORBIDDEN", "没有权限执行该操作");
  }
}

// 数量按十进制字符串传入（如 "12.3456"），避免 JSON 数字被解析成浮点。
// 兼容旧客户端：整数数字按「单位个数」理解，浮点数字直接拒绝。
Qty parse_qty_field(const nlohmann::json& value) {
  if (value.is_string()) {
    const auto parsed = parse_qty(value.get_ref<const std::string&>());
    if (!parsed.has_value()) {
      throw AppError("INVALID_INPUT", "数量格式不正确，最多 4 位小数：" + value.get<std::string>());
    }
    return *parsed;
  }
  if (value.is_number_integer()) {
    const std::int64_t units = value.get<std::int64_t>();
    if (units < 0) {
      throw AppError("INVALID_INPUT", "数量不能为负数");
    }
    // 整数同样受 100 万单位上限约束：字符串形态已由 parse_qty 拦截，
    // 这里不拦会让 qty*price 溢出，写出错误的金额与结存。
    if (units > kQtyLimit / kQtyScale) {
      throw AppError("INVALID_INPUT",
                     "数量超出上限（最多 1000000 个单位）：" + std::to_string(units));
    }
    return qty_from_int(units);
  }
  throw AppError("INVALID_INPUT", "数量请使用十进制字符串，例如 \"1.25\"");
}

nlohmann::json summary_to_json(const DocSummary& summary) {
  return nlohmann::json{
      {"id", summary.id},
      {"doc_no", summary.doc_no},
      {"doc_date", summary.doc_date},
      {"warehouse_name", summary.warehouse_name},
      {"customer", summary.customer},
      {"biz_category", summary.biz_category},
      {"salesperson_id", summary.salesperson_id},
      {"salesperson_name", summary.salesperson_name},
      {"remark", summary.remark},
      {"total_qty", qty_text(summary.total_qty)},
      {"converted_qty", qty_text(summary.converted_qty)},
      {"total_amount_cents", summary.total_amount_cents},
      {"status", summary.status},
      {"order_status", summary.order_status},
      {"version", summary.version},
      {"created_at", summary.created_at},
  };
}

nlohmann::json detail_to_json(const DocDetail& detail) {
  nlohmann::json lines = nlohmann::json::array();
  for (const auto& line : detail.lines) {
    lines.push_back(nlohmann::json{
        {"id", line.id},
        {"item_id", line.item_id},
        {"qty", qty_text(line.qty)},
        {"price_cents", line.price_cents},
        {"converted_qty", qty_text(line.converted_qty)},
        {"remaining_qty", qty_text(line.qty - line.converted_qty)},
        {"source_order_line_id", line.source_order_line_id},
        {"warehouse_id", line.warehouse_id},
        {"adjust_cents", line.adjust_cents},
        {"adjust_reason", line.adjust_reason},
        {"tax_rate_bp", line.tax_rate_bp},
        {"price_includes_tax", line.price_includes_tax},
        {"net_cents", line.net_cents},
        {"tax_cents", line.tax_cents},
    });
  }
  return nlohmann::json{
      {"id", detail.id},
      {"doc_no", detail.doc_no},
      {"doc_type", detail.doc_type},
      {"doc_date", detail.doc_date},
      {"warehouse_id", detail.warehouse_id},
      {"target_warehouse_id", detail.target_warehouse_id},
      {"partner_id", detail.partner_id},
      {"partner", detail.partner},
      {"biz_category", detail.biz_category},
      {"salesperson_id", detail.salesperson_id},
      {"salesperson_name", detail.salesperson_name},
      {"remark", detail.remark},
      {"status", detail.status},
      {"order_status", detail.order_status},
      {"version", detail.version},
      {"total_qty", qty_text(detail.total_qty)},
      {"total_amount_cents", detail.total_amount_cents},
      {"total_net_cents", detail.total_net_cents},
      {"total_tax_cents", detail.total_tax_cents},
      {"total_with_tax_cents", detail.total_with_tax_cents},
      {"lines", lines},
  };
}

// 乐观锁版本：老客户端不带 version，返回 -1 表示跳过校验（core 会写日志）。
std::int64_t request_version(const httplib::Request& req) {
  if (req.has_param("version")) {
    return std::stoll(req.get_param_value("version"));
  }
  if (req.body.empty()) return -1;
  try {
    const nlohmann::json body = nlohmann::json::parse(req.body);
    if (body.contains("version") && body["version"].is_number()) {
      return body["version"].get<std::int64_t>();
    }
  } catch (const nlohmann::json::exception&) {
    // 没有合法 JSON body：按老客户端处理
  }
  return -1;
}

DocInput input_from_json(const nlohmann::json& body) {
  DocInput input;
  input.doc_type = body.value("doc_type", std::string{});
  input.warehouse_id = body.value("warehouse_id", std::int64_t{0});
  input.doc_date = body.value("doc_date", std::string{});
  input.target_warehouse_id = body.value("target_warehouse_id", std::int64_t{0});
  input.partner_id = body.value("partner_id", std::int64_t{0});
  input.partner = body.value("partner", std::string{});
  input.biz_category = body.value("biz_category", std::string{});
  input.salesperson_id = body.value("salesperson_id", std::int64_t{0});
  input.remark = body.value("remark", std::string{});
  // 幂等键与乐观锁版本：老客户端不传时保持空串 / -1，按无幂等、无版本校验放行。
  input.request_id = body.value("request_id", std::string{});
  if (body.contains("version") && !body["version"].is_null()) {
    input.version = body.value("version", std::int64_t{-1});
  }
  if (body.contains("lines") && body["lines"].is_array()) {
    for (const auto& item : body["lines"]) {
      DocLineInput line;
      line.item_id = item.value("item_id", std::int64_t{0});
      if (item.contains("qty")) {
        line.qty = parse_qty_field(item["qty"]);
      }
      line.price_cents = item.value("price_cents", std::int64_t{0});
      line.warehouse_id = item.value("warehouse_id", std::int64_t{0});
      line.adjust_cents = item.value("adjust_cents", std::int64_t{0});
      line.adjust_reason = item.value("adjust_reason", std::string{});
      if (item.contains("tax_rate_bp") && !item["tax_rate_bp"].is_null()) {
        line.tax_rate_bp = item.value("tax_rate_bp", std::int64_t{-1});
      }
      line.price_includes_tax = item.value("price_includes_tax", false);
      input.lines.push_back(line);
    }
  }
  return input;
}

// 单据导出与打印共用同一批列，金额由整数分换算成元，两位小数。
std::string export_yuan(std::int64_t cents) {
  const bool negative = cents < 0;
  const std::int64_t abs = negative ? -cents : cents;
  std::string fraction = std::to_string(abs % 100);
  if (fraction.size() < 2) fraction.insert(0, 2 - fraction.size(), '0');
  return (negative ? "-" : "") + std::to_string(abs / 100) + "." + fraction;
}

std::string rate_text(std::int64_t bp) {
  std::string text = std::to_string(bp / 100);
  if (bp % 100 != 0) {
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), ".%02lld", static_cast<long long>(bp % 100));
    std::string fraction = buffer;
    while (!fraction.empty() && fraction.back() == '0') fraction.pop_back();
    text += fraction;
  }
  return text + "%";
}

void append_csv_row(std::string& out, const std::vector<std::string>& fields) {
  for (std::size_t i = 0; i < fields.size(); ++i) {
    if (i != 0) out += ',';
    out += csv_escape(fields[i]);
  }
  out += "\r\n";
}

// 单据列表面板的新参数：任一出现即走「筛选 + 分页 + 合计」分支，
// 完全不带时仍返回原来的 JSON 数组，老前端不受影响。
bool has_list_filter(const httplib::Request& req) {
  static const char* const kNames[] = {"page",       "page_size",  "keyword",
                                       "date_from",  "date_to",    "status",
                                       "amount_min", "amount_max"};
  for (const char* name : kNames) {
    if (req.has_param(name)) return true;
  }
  return false;
}

std::string param_or_empty(const httplib::Request& req, const char* name) {
  return req.has_param(name) ? req.get_param_value(name) : std::string{};
}

// 分页参数按十进制整数解析，非法值明确报错而不是静默取默认。
std::int64_t parse_int_param(const httplib::Request& req, const char* name,
                             std::int64_t fallback) {
  if (!req.has_param(name)) return fallback;
  const std::string raw = req.get_param_value(name);
  if (raw.empty()) return fallback;
  try {
    std::size_t consumed = 0;
    const std::int64_t value = std::stoll(raw, &consumed, 10);
    if (consumed != raw.size()) throw std::invalid_argument(raw);
    return value;
  } catch (const std::exception&) {
    throw AppError("INVALID_INPUT", std::string("参数格式不正确: ") + name);
  }
}

// 金额按「元」字符串接收（最多两位小数），全程整数解析后换算成分，不用浮点累加。
std::int64_t parse_yuan_cents(const std::string& raw) {
  std::string text;
  for (const char ch : raw) {
    if (ch != ' ' && ch != '\t') text.push_back(ch);
  }
  if (text.empty()) return 0;
  bool negative = false;
  std::size_t index = 0;
  if (text[0] == '+' || text[0] == '-') {
    negative = text[0] == '-';
    index = 1;
  }
  std::string whole;
  std::string fraction;
  bool has_dot = false;
  for (; index < text.size(); ++index) {
    const char ch = text[index];
    if (ch == '.') {
      if (has_dot) throw AppError("INVALID_INPUT", "金额格式不正确: " + raw);
      has_dot = true;
      continue;
    }
    if (ch < '0' || ch > '9') throw AppError("INVALID_INPUT", "金额格式不正确: " + raw);
    (has_dot ? fraction : whole).push_back(ch);
  }
  if (whole.empty() && fraction.empty()) {
    throw AppError("INVALID_INPUT", "金额格式不正确: " + raw);
  }
  if (fraction.size() > 2) {
    throw AppError("INVALID_INPUT", "金额最多两位小数: " + raw);
  }
  if (negative) throw AppError("INVALID_INPUT", "金额不能为负数: " + raw);
  while (fraction.size() < 2) fraction.push_back('0');
  try {
    return std::stoll((whole.empty() ? "0" : whole) + fraction, nullptr, 10);
  } catch (const std::exception&) {
    throw AppError("INVALID_INPUT", "金额超出范围: " + raw);
  }
}

}  // namespace

void register_document_routes(httplib::Server& server, Services& services) {
  server.Get("/api/documents",
             guard("document.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const std::string doc_type = req.has_param("doc_type")
                                                      ? req.get_param_value("doc_type")
                                                      : "SAL_OUT";
                     if (!has_list_filter(req)) {
                       // 兼容分支：没有任何筛选参数时保持原来的数组响应。
                       nlohmann::json list = nlohmann::json::array();
                       for (const auto& summary : services.docs.list(doc_type)) {
                         list.push_back(summary_to_json(summary));
                       }
                       json_ok(res, list);
                       return;
                     }
                     DocListQuery query;
                     query.doc_type = doc_type;
                     query.keyword = param_or_empty(req, "keyword");
                     query.date_from = param_or_empty(req, "date_from");
                     query.date_to = param_or_empty(req, "date_to");
                     query.status = param_or_empty(req, "status");
                     // 金额按「元」字符串接收，服务端整数换算成分。
                     query.amount_min_cents = parse_yuan_cents(param_or_empty(req, "amount_min"));
                     query.amount_max_cents = parse_yuan_cents(param_or_empty(req, "amount_max"));
                     query.page = static_cast<int>(std::clamp<std::int64_t>(
                         parse_int_param(req, "page", 1), 1, 1000000));
                     query.page_size = static_cast<int>(std::clamp<std::int64_t>(
                         parse_int_param(req, "page_size", 20), 1, 100));
                     const DocListPage page = services.docs.list_page(query);
                     nlohmann::json rows = nlohmann::json::array();
                     for (const auto& summary : page.rows) {
                       rows.push_back(summary_to_json(summary));
                     }
                     json_ok(res, nlohmann::json{{"rows", rows},
                                                 {"total_count", page.total_count},
                                                 {"total_amount_cents", page.total_amount_cents},
                                                 {"page", page.page},
                                                 {"page_size", page.page_size}});
                   }));

  server.Get(R"(/api/documents/(\d+))",
             guard("document.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const auto id = std::stoll(req.matches[1].str());
                     json_ok(res, detail_to_json(services.docs.detail(id)));
                   }));

  // 单据导出：逐行带不含税净额与税额，末尾给出「合计金额 / 税额 / 价税合计」，
  // 与打印、页面同源（都来自 doc_line 的 net_cents / tax_cents）。
  server.Get(R"(/api/documents/(\d+)/export)",
             guard("document.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const std::int64_t id = std::stoll(req.matches[1].str());
                     const DocDetail detail = services.docs.detail(id);
                     std::map<std::int64_t, gen::ItemRow> items;
                     for (const auto& row : services.item.list()) items[row.id] = row;
                     const bool want_xlsx =
                         req.has_param("format") && req.get_param_value("format") == "xlsx";
                     const std::vector<std::string> header = {
                         "行号", "商品编码", "商品名称", "规格", "单位", "数量",
                         "单价（元）", "金额（录入价）", "税率", "含税", "净额（元）",
                         "税额（元）", "价税合计（元）"};
                     if (want_xlsx) {
                       XlsxSheet sheet(detail.doc_no);
                       sheet.add_text_row({"单号", detail.doc_no});
                       sheet.add_text_row({"日期", detail.doc_date});
                       sheet.add_text_row({"往来单位", detail.partner});
                       sheet.add_text_row({"备注", detail.remark});
                       sheet.add_header_row(header);
                       int line_no = 0;
                       for (const auto& line : detail.lines) {
                         const auto found = items.find(line.item_id);
                         const bool missing = found == items.end();
                         sheet.add_row(
                             {XlsxCell::text(std::to_string(++line_no)),
                              XlsxCell::text(missing ? "" : found->second.code),
                              XlsxCell::text(missing ? "" : found->second.name),
                              XlsxCell::text(missing ? "" : found->second.spec),
                              XlsxCell::text(missing ? "" : found->second.unit),
                              XlsxCell::text(qty_text(line.qty)),
                              XlsxCell::money(line.price_cents),
                              XlsxCell::money(mul_qty_price(line.qty, line.price_cents)),
                              XlsxCell::text(rate_text(line.tax_rate_bp)),
                              XlsxCell::text(line.price_includes_tax ? "是" : "否"),
                              XlsxCell::money(line.net_cents),
                              XlsxCell::money(line.tax_cents),
                              XlsxCell::money(line.net_cents + line.tax_cents)});
                       }
                       sheet.add_row({XlsxCell::text("合计金额"), XlsxCell::text(""),
                                      XlsxCell::text(""), XlsxCell::text(""),
                                      XlsxCell::text(""), XlsxCell::text(""),
                                      XlsxCell::text(""),
                                      XlsxCell::money(detail.total_amount_cents),
                                      XlsxCell::text(""), XlsxCell::text(""),
                                      XlsxCell::money(detail.total_net_cents),
                                      XlsxCell::money(detail.total_tax_cents),
                                      XlsxCell::money(detail.total_with_tax_cents)});
                       sheet.add_row({XlsxCell::text("税额"), XlsxCell::text(""),
                                      XlsxCell::text(""), XlsxCell::text(""),
                                      XlsxCell::text(""), XlsxCell::text(""),
                                      XlsxCell::text(""), XlsxCell::text(""),
                                      XlsxCell::text(""), XlsxCell::text(""),
                                      XlsxCell::text(""),
                                      XlsxCell::money(detail.total_tax_cents),
                                      XlsxCell::text("")});
                       sheet.add_row({XlsxCell::text("价税合计"), XlsxCell::text(""),
                                      XlsxCell::text(""), XlsxCell::text(""),
                                      XlsxCell::text(""), XlsxCell::text(""),
                                      XlsxCell::text(""), XlsxCell::text(""),
                                      XlsxCell::text(""), XlsxCell::text(""),
                                      XlsxCell::text(""), XlsxCell::text(""),
                                      XlsxCell::money(detail.total_with_tax_cents)});
                       res.set_header("Content-Disposition",
                                      "attachment; filename=\"" + detail.doc_no + ".xlsx\"");
                       res.set_content(sheet.build(),
                                       "application/vnd.openxmlformats-officedocument."
                                       "spreadsheetml.sheet");
                       return;
                     }
                     std::string csv;
                     append_csv_row(csv, {"单号", detail.doc_no});
                     append_csv_row(csv, {"日期", detail.doc_date});
                     append_csv_row(csv, {"往来单位", detail.partner});
                     append_csv_row(csv, {"备注", detail.remark});
                     append_csv_row(csv, {});
                     append_csv_row(csv, header);
                     int line_no = 0;
                     for (const auto& line : detail.lines) {
                       const auto found = items.find(line.item_id);
                       const bool missing = found == items.end();
                       append_csv_row(
                           csv,
                           {std::to_string(++line_no),
                            missing ? "" : found->second.code,
                            missing ? "" : found->second.name,
                            missing ? "" : found->second.spec,
                            missing ? "" : found->second.unit,
                            qty_text(line.qty), export_yuan(line.price_cents),
                            export_yuan(mul_qty_price(line.qty, line.price_cents)),
                            rate_text(line.tax_rate_bp),
                            line.price_includes_tax ? "是" : "否",
                            export_yuan(line.net_cents), export_yuan(line.tax_cents),
                            export_yuan(line.net_cents + line.tax_cents)});
                     }
                     append_csv_row(csv, {});
                     append_csv_row(csv, {"合计金额", "", "", "", "", "", "",
                                          export_yuan(detail.total_amount_cents), "", "",
                                          export_yuan(detail.total_net_cents),
                                          export_yuan(detail.total_tax_cents),
                                          export_yuan(detail.total_with_tax_cents)});
                     append_csv_row(csv, {"税额", "", "", "", "", "", "", "", "", "", "",
                                          export_yuan(detail.total_tax_cents), ""});
                     append_csv_row(csv, {"价税合计", "", "", "", "", "", "", "", "", "", "",
                                          "", export_yuan(detail.total_with_tax_cents)});
                     res.set_header("Content-Disposition",
                                    "attachment; filename=\"" + detail.doc_no + ".csv\"");
                     res.set_content("\xEF\xBB\xBF" + csv, "text/csv; charset=utf-8");
                   }));

  server.Post("/api/documents",
              guard("document.write",
                    [&services](const httplib::Request& req, httplib::Response& res) {
                      const nlohmann::json body = json_body(req);
                      const DocInput input = input_from_json(body);
                      // 审计请求号用客户端带来的幂等键；没有就留空，不另生成。
                      if (!input.request_id.empty()) {
                        current_context().audit_request_id = input.request_id;
                      }
                      const bool draft = body.value("save_as_draft", false);
                      const bool confirm_credit = body.value("confirm_credit", false);
                      if (!draft) require_doc_audit(input.doc_type);
                      const auto created =
                          draft ? services.docs.save_draft(input)
                                : services.docs.save_and_approve(input, confirm_credit);
                      nlohmann::json result;
                      result["id"] = created.id;
                      result["doc_no"] = created.doc_no;
                      result["version"] = created.version;
                      json_ok(res, result);
                    }));

  server.Put(R"(/api/documents/(\d+))",
             guard("document.write",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const auto id = std::stoll(req.matches[1].str());
                     const DocInput input = input_from_json(json_body(req));
                     if (!input.request_id.empty()) {
                       current_context().audit_request_id = input.request_id;
                     }
                     services.docs.update_draft(id, input);
                     json_ok(res, detail_to_json(services.docs.detail(id)));
                   }));

  server.Delete(R"(/api/documents/(\d+))",
                guard("document.write",
                      [&services](const httplib::Request& req, httplib::Response& res) {
                        const auto id = std::stoll(req.matches[1].str());
                        // DELETE 带不了 body，版本走查询参数；不传按老客户端放行。
                        const std::int64_t version =
                            req.has_param("version")
                                ? std::stoll(req.get_param_value("version"))
                                : -1;
                        services.docs.delete_draft(id, version);
                        nlohmann::json body;
                        body["ok"] = true;
                        json_ok(res, body);
                      }));

  server.Post(R"(/api/documents/(\d+)/approve)",
              guard("document.write",
                    [&services](const httplib::Request& req, httplib::Response& res) {
                      const auto id = std::stoll(req.matches[1].str());
                      require_doc_audit(services.docs.detail(id).doc_type);
                      const bool confirm_credit =
                          req.has_param("confirm_credit") &&
                          req.get_param_value("confirm_credit") == "1";
                      services.docs.approve(id, confirm_credit, request_version(req));
                      json_ok(res, detail_to_json(services.docs.detail(id)));
                    }));

  server.Post(R"(/api/documents/(\d+)/reverse)",
              guard("document.write",
                    [&services](const httplib::Request& req, httplib::Response& res) {
                      const auto id = std::stoll(req.matches[1].str());
                      require_doc_audit(services.docs.detail(id).doc_type);
                      services.docs.reverse(id, request_version(req));
                      nlohmann::json body;
                      body["ok"] = true;
                      json_ok(res, body);
                    }));

  server.Post(R"(/api/documents/(\d+)/convert)",
              guard("document.write",
                    [&services](const httplib::Request& req, httplib::Response& res) {
                      const auto id = std::stoll(req.matches[1].str());
                      const nlohmann::json body = json_body(req);
                      OrderConvertInput input;
                      input.warehouse_id = body.value("warehouse_id", std::int64_t{0});
                      input.doc_date = body.value("doc_date", std::string{});
                      input.confirm_credit = body.value("confirm_credit", false);
                      if (body.contains("lines") && body["lines"].is_array()) {
                        for (const auto& item : body["lines"]) {
                          input.lines.push_back(OrderConvertLine{
                              .order_line_id = item.value("order_line_id", std::int64_t{0}),
                              .qty = item.contains("qty") ? parse_qty_field(item["qty"])
                                                          : std::int64_t{0},
                              .price_cents = item.value("price_cents", std::int64_t{-1}),
                          });
                        }
                      }
                      const OrderConversion conversion = services.docs.convert_order(id, input);
                      json_ok(res, nlohmann::json{{"doc_id", conversion.doc_id},
                                                  {"doc_no", conversion.doc_no},
                                                  {"order_status", conversion.order_status}});
                    }));

  // 成本重算页「仅金额差异」一键生成草稿：金额以服务端重算结果为准，客户端只传商品与仓库。
  server.Post("/api/cost-adjustments/drafts",
              guard("document.write",
                    [&services](const httplib::Request& req, httplib::Response& res) {
                      const nlohmann::json body = json_body(req);
                      if (!body.contains("lines") || !body["lines"].is_array() ||
                          body["lines"].empty()) {
                        throw AppError("INVALID_INPUT", "请选择要生成调整单的差异行");
                      }
                      DocInput input;
                      input.doc_type = "CST_ADJ";
                      for (const auto& item : body["lines"]) {
                        const std::int64_t item_id =
                            item.value("item_id", std::int64_t{0});
                        const std::int64_t warehouse_id =
                            item.value("warehouse_id", std::int64_t{0});
                        const auto result =
                            services.cost_recalc.recalculate(item_id, warehouse_id);
                        const CostRecalcRow* found = nullptr;
                        for (const auto& row : result.differences) {
                          if (row.item_id == item_id && row.warehouse_id == warehouse_id) {
                            found = &row;
                            break;
                          }
                        }
                        if (found == nullptr) {
                          throw AppError("NO_DIFFERENCE", "该商品与仓库没有成本差异");
                        }
                        if (found->qty_diff != 0) {
                          throw AppError("QTY_DIFF", "数量差异请使用盘点单处理");
                        }
                        if (found->amount_diff_cents == 0) {
                          throw AppError("NO_DIFFERENCE", "该商品与仓库没有金额差异");
                        }
                        DocLineInput line;
                        line.item_id = item_id;
                        line.qty = 0;
                        line.price_cents = 0;
                        line.warehouse_id = warehouse_id;
                        line.adjust_cents = found->amount_diff_cents;
                        line.adjust_reason =
                            item.value("reason", std::string("成本重算差异"));
                        input.lines.push_back(line);
                      }
                      const auto created = services.docs.save_draft(input);
                      json_ok(res, nlohmann::json{{"id", created.id},
                                                  {"doc_no", created.doc_no}});
                    }));

  server.Post(R"(/api/documents/(\d+)/close)",
              guard("document.write",
                    [&services](const httplib::Request& req, httplib::Response& res) {
                      const auto id = std::stoll(req.matches[1].str());
                      services.docs.close_order(id);
                      json_ok(res, nlohmann::json{{"ok", true}});
                    }));
}

}  // namespace xiaotu
