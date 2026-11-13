#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <string>

#include "xiaotu/audit.h"
#include "xiaotu/db.h"
#include "xiaotu/http_guard.h"
#include "xiaotu/http_json.h"
#include "xiaotu/services.h"
#include "xiaotu/xlsx.h"

namespace xiaotu {
namespace {

// 参数缺失或格式不对时按「不限」处理，不让一个手写的查询串把页面变成 500。
std::int64_t param_int(const httplib::Request& req, const char* name,
                       std::int64_t fallback) {
  if (!req.has_param(name)) {
    return fallback;
  }
  try {
    return std::stoll(req.get_param_value(name));
  } catch (const std::exception&) {
    return fallback;
  }
}

AuditFilter filter_from_request(const httplib::Request& req) {
  AuditFilter filter;
  filter.from = req.has_param("from") ? req.get_param_value("from") : "";
  filter.to = req.has_param("to") ? req.get_param_value("to") : "";
  filter.entity = req.has_param("entity") ? req.get_param_value("entity") : "";
  filter.entity_id = param_int(req, "entity_id", 0);
  filter.actor = req.has_param("actor") ? req.get_param_value("actor") : "";
  filter.client_ip = req.has_param("client_ip") ? req.get_param_value("client_ip") : "";
  filter.request_id = req.has_param("request_id") ? req.get_param_value("request_id") : "";
  filter.page = static_cast<int>(param_int(req, "page", 1));
  filter.page_size = static_cast<int>(param_int(req, "page_size", 50));
  return filter;
}

nlohmann::json audit_to_json(const AuditRow& row) {
  return nlohmann::json{
      {"id", row.id},
      {"at", row.at},
      {"actor", row.actor},
      {"action", row.action},
      {"entity", row.entity},
      {"entity_id", row.entity_id},
      {"entity_no", row.entity_no},
      {"detail", row.detail},
      {"client_ip", row.client_ip},
      {"request_id", row.request_id},
  };
}

// ---------- 导出 ----------

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

std::string date_stamp() {
  std::string stamp = now_text().substr(0, 10);
  stamp.erase(std::remove(stamp.begin(), stamp.end(), '-'), stamp.end());
  return stamp;
}

std::string scope_text(const AuditFilter& filter) {
  std::string scope;
  const auto append = [&scope](const std::string& label, const std::string& value) {
    if (value.empty()) return;
    if (!scope.empty()) scope += "；";
    scope += label + " " + value;
  };
  append("开始", filter.from);
  append("结束", filter.to);
  append("对象类型", filter.entity);
  if (filter.entity_id > 0) append("对象 id", std::to_string(filter.entity_id));
  append("操作人", filter.actor);
  append("来源 IP", filter.client_ip);
  append("请求号", filter.request_id);
  return scope.empty() ? "全部记录" : scope;
}

constexpr const char* kAuditExportHeader =
    "时间,操作人,动作,对象类型,对象 id,来源 IP,请求号,摘要\r\n";

void send_csv(httplib::Response& res, const std::string& filename, const std::string& body) {
  res.set_header("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  res.set_content("\xEF\xBB\xBF" + body, "text/csv; charset=utf-8");
}

}  // namespace

void register_audit_routes(httplib::Server& server, Services& services) {
  server.Get("/api/audit-logs",
             guard("audit.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const AuditPage page = audit_query(services.db, filter_from_request(req));
                     nlohmann::json items = nlohmann::json::array();
                     for (const auto& row : page.rows) {
                       items.push_back(audit_to_json(row));
                     }
                     nlohmann::json body;
                     body["items"] = items;
                     body["total"] = page.total;
                     body["page"] = page.page;
                     body["page_size"] = page.page_size;
                     json_ok(res, body);
                   }));

  // 审计导出：与查询同一套筛选条件，不分页；CSV / XLSX 都带来源 IP 与请求号。
  server.Get("/api/audit-logs/export",
             guard("audit.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const AuditFilter filter = filter_from_request(req);
                     const auto rows = audit_all(services.db, filter);
                     const bool xlsx =
                         req.has_param("format") && req.get_param_value("format") == "xlsx";
                     const std::string filename =
                         "audit-logs-" + date_stamp() + (xlsx ? ".xlsx" : ".csv");
                     const std::string scope = scope_text(filter);
                     if (xlsx) {
                       XlsxSheet sheet("审计日志");
                       sheet.add_text_row({"报表", "审计日志"});
                       sheet.add_text_row({"筛选条件", scope});
                       sheet.add_text_row({"导出时间", now_text()});
                       sheet.add_text_row({"共", std::to_string(rows.size()) + " 条"});
                       sheet.add_text_row({});
                       sheet.add_header_row({"时间", "操作人", "动作", "对象类型", "对象 id",
                                             "来源 IP", "请求号", "摘要"});
                       for (const auto& row : rows) {
                         sheet.add_row({XlsxCell::text(row.at),
                                        XlsxCell::text(row.actor),
                                        XlsxCell::text(row.action),
                                        XlsxCell::text(row.entity),
                                        XlsxCell::integer(row.entity_id),
                                        XlsxCell::text(row.client_ip),
                                        XlsxCell::text(row.request_id),
                                        XlsxCell::text(row.detail)});
                       }
                       res.set_header("Content-Disposition",
                                      "attachment; filename=\"" + filename + "\"");
                       res.set_content(
                           sheet.build(),
                           "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet");
                       return;
                     }
                     std::string csv;
                     csv_row(csv, {"报表", "审计日志"});
                     csv_row(csv, {"筛选条件", scope});
                     csv_row(csv, {"导出时间", now_text()});
                     csv_row(csv, {"共", std::to_string(rows.size()) + " 条"});
                     csv += "\r\n";
                     csv += kAuditExportHeader;
                     for (const auto& row : rows) {
                       csv_row(csv, {row.at, row.actor, row.action, row.entity,
                                     std::to_string(row.entity_id), row.client_ip,
                                     row.request_id, row.detail});
                     }
                     send_csv(res, filename, csv);
                   }));
}

}  // namespace xiaotu
