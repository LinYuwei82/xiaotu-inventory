#include <httplib.h>
#include <nlohmann/json.hpp>

#include <string>

#include "xiaotu/document.h"
#include "xiaotu/context.h"
#include "xiaotu/http_guard.h"
#include "xiaotu/http_json.h"
#include "xiaotu/permissions.h"
#include "xiaotu/qty.h"
#include "xiaotu/services.h"

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
      {"total_qty", qty_text(summary.total_qty)},
      {"converted_qty", qty_text(summary.converted_qty)},
      {"total_amount_cents", summary.total_amount_cents},
      {"status", summary.status},
      {"order_status", summary.order_status},
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
      {"status", detail.status},
      {"order_status", detail.order_status},
      {"total_qty", qty_text(detail.total_qty)},
      {"total_amount_cents", detail.total_amount_cents},
      {"lines", lines},
  };
}

DocInput input_from_json(const nlohmann::json& body) {
  DocInput input;
  input.doc_type = body.value("doc_type", std::string{});
  input.warehouse_id = body.value("warehouse_id", std::int64_t{0});
  input.doc_date = body.value("doc_date", std::string{});
  input.target_warehouse_id = body.value("target_warehouse_id", std::int64_t{0});
  input.partner_id = body.value("partner_id", std::int64_t{0});
  input.partner = body.value("partner", std::string{});
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
      input.lines.push_back(line);
    }
  }
  return input;
}

}  // namespace

void register_document_routes(httplib::Server& server, Services& services) {
  server.Get("/api/documents",
             guard("document.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const std::string doc_type = req.has_param("doc_type")
                                                      ? req.get_param_value("doc_type")
                                                      : "SAL_OUT";
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& summary : services.docs.list(doc_type)) {
                       list.push_back(summary_to_json(summary));
                     }
                     json_ok(res, list);
                   }));

  server.Get(R"(/api/documents/(\d+))",
             guard("document.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const auto id = std::stoll(req.matches[1].str());
                     json_ok(res, detail_to_json(services.docs.detail(id)));
                   }));

  server.Post("/api/documents",
              guard("document.write",
                    [&services](const httplib::Request& req, httplib::Response& res) {
                      const nlohmann::json body = json_body(req);
                      const DocInput input = input_from_json(body);
                      const bool draft = body.value("save_as_draft", false);
                      if (!draft) require_doc_audit(input.doc_type);
                      const auto created =
                          draft ? services.docs.save_draft(input)
                                : services.docs.save_and_approve(input);
                      nlohmann::json result;
                      result["id"] = created.id;
                      result["doc_no"] = created.doc_no;
                      json_ok(res, result);
                    }));

  server.Put(R"(/api/documents/(\d+))",
             guard("document.write",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const auto id = std::stoll(req.matches[1].str());
                     const DocInput input = input_from_json(json_body(req));
                     services.docs.update_draft(id, input);
                     json_ok(res, detail_to_json(services.docs.detail(id)));
                   }));

  server.Delete(R"(/api/documents/(\d+))",
                guard("document.write",
                      [&services](const httplib::Request& req, httplib::Response& res) {
                        const auto id = std::stoll(req.matches[1].str());
                        services.docs.delete_draft(id);
                        nlohmann::json body;
                        body["ok"] = true;
                        json_ok(res, body);
                      }));

  server.Post(R"(/api/documents/(\d+)/approve)",
              guard("document.write",
                    [&services](const httplib::Request& req, httplib::Response& res) {
                      const auto id = std::stoll(req.matches[1].str());
                      require_doc_audit(services.docs.detail(id).doc_type);
                      services.docs.approve(id);
                      json_ok(res, detail_to_json(services.docs.detail(id)));
                    }));

  server.Post(R"(/api/documents/(\d+)/reverse)",
              guard("document.write",
                    [&services](const httplib::Request& req, httplib::Response& res) {
                      const auto id = std::stoll(req.matches[1].str());
                      require_doc_audit(services.docs.detail(id).doc_type);
                      services.docs.reverse(id);
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
