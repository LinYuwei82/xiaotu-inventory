#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "xiaotu/error.h"
#include "xiaotu/http_guard.h"
#include "xiaotu/http_json.h"
#include "xiaotu/masterdata_io.h"
#include "xiaotu/services.h"
#include "xiaotu/xlsx.h"

namespace xiaotu {
namespace {

struct MasterRoute {
  MasterKind kind;
  const char* base;      // /api/items
  const char* write;     // item.write
  const char* read;      // item.read
  const char* name;      // items（导出文件名）
  const char* sheet;     // 商品
};

constexpr MasterRoute kRoutes[] = {
    {MasterKind::Item, "/api/items", "item.write", "item.read", "items", "商品"},
    {MasterKind::Supplier, "/api/suppliers", "supplier.write", "supplier.read", "suppliers",
     "供应商"},
    {MasterKind::Customer, "/api/customers", "customer.write", "customer.read", "customers",
     "客户"},
};

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

void csv_row(std::string& out, const std::vector<std::string>& fields) {
  bool first = true;
  for (const auto& field : fields) {
    if (!first) out += ',';
    first = false;
    out += csv_escape(field);
  }
  out += "\r\n";
}

void send_xlsx(httplib::Response& res, const std::string& filename, const std::string& body) {
  res.set_header("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  res.set_content(body,
                  "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet");
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

int hex_value(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

// 前端用 encodeURIComponent 传文件名，避免 HTTP 头里的非 ASCII 被改写。
std::string decode_percent(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '%' && i + 2 < value.size()) {
      const int high = hex_value(value[i + 1]);
      const int low = hex_value(value[i + 2]);
      if (high >= 0 && low >= 0) {
        out.push_back(static_cast<char>(high * 16 + low));
        i += 2;
        continue;
      }
    }
    out.push_back(value[i]);
  }
  return out;
}

std::string import_filename(const httplib::Request& req) {
  std::string name = decode_percent(req.get_header_value("X-File-Name"));
  if (name.empty()) name = "import.xlsx";
  return name;
}

nlohmann::json issues_json(const std::vector<ImportIssue>& issues) {
  nlohmann::json list = nlohmann::json::array();
  for (const auto& issue : issues) {
    list.push_back(
        {{"row", issue.row}, {"column", issue.column}, {"reason", issue.reason}});
  }
  return list;
}

nlohmann::json import_json(const ImportReport& report) {
  if (!report.ok()) {
    nlohmann::json body;
    body["ok"] = false;
    body["code"] = "IMPORT_INVALID";
    body["message"] =
        "共 " + std::to_string(report.issues.size()) + " 行有问题，未导入任何数据";
    body["errors"] = issues_json(report.issues);
    return body;
  }
  nlohmann::json body;
  body["ok"] = true;
  body["inserted"] = report.outcome.inserted;
  body["updated"] = report.outcome.updated;
  body["backup"] = report.outcome.backup;
  return body;
}

std::string build_csv(const MasterExport& table) {
  std::string csv;
  csv_row(csv, table.header);
  for (const auto& row : table.rows) csv_row(csv, row);
  return csv;
}

std::string build_xlsx(const std::string& sheet_name, const MasterExport& table) {
  XlsxSheet sheet(sheet_name);
  if (table.header.size() == 5) {
    sheet.add_header_row({table.header[0], table.header[1], table.header[2], table.header[3],
                          table.header[4]});
    for (const auto& row : table.rows) {
      sheet.add_text_row({row[0], row[1], row[2], row[3], row[4]});
    }
  } else {
    sheet.add_header_row({table.header[0], table.header[1], table.header[2], table.header[3],
                          table.header[4], table.header[5]});
    for (const auto& row : table.rows) {
      sheet.add_text_row({row[0], row[1], row[2], row[3], row[4], row[5]});
    }
  }
  return sheet.build();
}

}  // namespace

void register_masterdata_routes(httplib::Server& server, Services& services) {
  for (const auto& route : kRoutes) {
    const std::string base = route.base;
    const MasterKind kind = route.kind;
    const std::string sheet = route.sheet;
    const std::string name = route.name;

    server.Post(base + "/import",
                guard(route.write, [&services, kind](const httplib::Request& req,
                                                     httplib::Response& res) {
                  const ImportReport report =
                      import_master(services.db, kind, req.body, import_filename(req));
                  if (!report.ok()) {
                    res.status = 400;
                    res.set_content(import_json(report).dump(), "application/json; charset=utf-8");
                    return;
                  }
                  json_ok(res, import_json(report));
                }));

    server.Get(base + "/export",
               guard(route.read, [&services, kind, sheet, name](const httplib::Request& req,
                                                                httplib::Response& res) {
                 const MasterExport table = export_master(services.db, kind);
                 const bool csv =
                     req.has_param("format") && req.get_param_value("format") == "csv";
                 if (csv) {
                   send_csv(res, name + "-" + date_stamp() + ".csv", build_csv(table));
                   return;
                 }
                 send_xlsx(res, name + "-" + date_stamp() + ".xlsx",
                           build_xlsx(sheet, table));
               }));

    server.Get(base + "/template",
               guard(route.read, [&services, kind, sheet](const httplib::Request&,
                                                          httplib::Response& res) {
                 MasterExport table;
                 table.header = master_columns(kind);
                 send_xlsx(res, master_key(kind) + "-import-template.xlsx",
                           build_xlsx(sheet, table));
               }));
  }
}

}  // namespace xiaotu
