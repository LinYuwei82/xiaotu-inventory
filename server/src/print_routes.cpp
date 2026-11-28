#include <httplib.h>
#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

#include "print_render.h"
#include "xiaotu/context.h"
#include "xiaotu/error.h"
#include "xiaotu/http_guard.h"
#include "xiaotu/http_json.h"
#include "xiaotu/permissions.h"
#include "xiaotu/print_template.h"
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
      {"paper_kind", tmpl.paper_kind},
      {"scale_mode", tmpl.scale_mode},
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
  tmpl.paper_kind = body.value("paper_kind", std::string{"SHEET"});
  tmpl.scale_mode = body.value("scale_mode", std::string{"FIT_WIDTH"});
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
    const ReportFilter filter = report_filter(req);
    PrintReportContext context{services, *target, req, filter, render};
    render_report(context);
  }

  render.placeholders["title"] = render.title;
  render.placeholders["date"] = today_text();
  render.placeholders["printed_at"] = now_text();
  render.placeholders["period"] = render.subtitle;
  // 公司抬头来自系统参数，模板里可用 {{company_name}} / {{company_phone}} / {{company_address}}。
  const CompanySettings company = services.settings.company();
  render.placeholders["company_name"] = company.name;
  render.placeholders["company_phone"] = company.phone;
  render.placeholders["company_address"] = company.address;

  const std::vector<PrintRenderColumn> resolved = resolved_print_columns(tmpl);
  nlohmann::json columns = nlohmann::json::array();
  for (const auto& column : resolved) {
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

  // 打印布局：纸型、可打印宽度、表宽、缩放与告警一次算清，页面 / 打印 / PDF 共用。
  const PrintLayoutPlan plan = plan_print_layout(tmpl, resolved);
  const nlohmann::json layout = {
      {"page_size", tmpl.page_size},
      {"orientation", tmpl.orientation},
      {"paper_kind", tmpl.paper_kind},
      {"scale_mode", tmpl.scale_mode},
      {"width_mm", plan.width_mm},
      {"height_mm", plan.height_mm},
      {"margin_mm", tmpl.margin_mm},
      {"printable_width_mm", plan.printable_width_mm},
      {"table_width_mm", plan.table_width_mm},
      {"auto_columns", plan.auto_columns},
      {"overflow_mm", plan.overflow_mm},
      {"scale_percent", plan.scale_percent},
      {"effective_font_pt", plan.effective_font_pt},
      {"suggest_landscape", plan.suggest_landscape},
      {"warn", plan.warn},
      {"warn_text", plan.warn_text},
  };

  nlohmann::json body;
  body["target"] = {{"type", target->type}, {"key", target->key}, {"name", target->name}};
  body["template"] = template_to_json(tmpl);
  body["template_source"] = source;
  body["columns"] = columns;
  body["layout"] = layout;
  body["title"] = render.title;
  body["subtitle"] = render.subtitle;
  body["printed_at"] = now_text();
  std::string header_text = apply_placeholders(tmpl.header_text, render.placeholders);
  // 模板没写抬头时用公司名称兜底，避免配了公司信息却不出现在单据上；
  // 电话与地址留给模板按需组合，不强行塞进默认抬头。
  if (header_text.empty() && !company.name.empty()) {
    header_text = company.name;
  }
  body["header_text"] = header_text;
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
                     const auto id = parse_id(req.matches[1].str());
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
                     const auto id = parse_id(req.matches[1].str());
                     const auto updated = services.print_templates.update(
                         id, template_from_json(json_body(req)));
                     json_ok(res, template_to_json(updated));
                   }));

  server.Delete(R"(/api/print-templates/(\d+))",
                guard("print.template.write",
                      [&services](const httplib::Request& req, httplib::Response& res) {
                        const auto id = parse_id(req.matches[1].str());
                        services.print_templates.remove(id);
                        json_ok(res, nlohmann::json{{"ok", true}});
                      }));

  server.Post(R"(/api/print-templates/(\d+)/default)",
              guard("print.template.write",
                    [&services](const httplib::Request& req, httplib::Response& res) {
                      const auto id = parse_id(req.matches[1].str());
                      json_ok(res, template_to_json(services.print_templates.set_default(id)));
                    }));

  // 出数入口：权限按目标数据本身判定，模板管理权限不参与。
  server.Get("/api/print/render", [&services](const httplib::Request& req,
                                              httplib::Response& res) {
    respond_render(services, req, res);
  });
}

}  // namespace xiaotu
