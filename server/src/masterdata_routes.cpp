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
#include "xiaotu/item_category.h"
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
  sheet.add_header_row(table.header);
  for (const auto& row : table.rows) sheet.add_text_row(row);
  return sheet.build();
}

std::int64_t param_int(const httplib::Request& req, const char* name, std::int64_t fallback) {
  if (!req.has_param(name)) return fallback;
  try {
    return std::stoll(req.get_param_value(name));
  } catch (...) {
    return fallback;
  }
}

std::string like_pattern(const std::string& value) {
  // 用户输入里的 % 与 _ 按字面匹配，避免「%」把整表搜出来。
  std::string escaped;
  escaped.reserve(value.size() + 2);
  for (const char ch : value) {
    if (ch == '%' || ch == '_' || ch == '\\') escaped.push_back('\\');
    escaped.push_back(ch);
  }
  return "%" + escaped + "%";
}

nlohmann::json category_json(const ItemCategoryRow& row, const std::string& full_name) {
  return nlohmann::json{{"id", row.id},
                        {"parent_id", row.parent_id},
                        {"name", row.name},
                        {"full_name", full_name},
                        {"path", row.path},
                        {"sort_order", row.sort_order},
                        {"status", row.status},
                        {"depth", row.depth},
                        {"item_count", row.item_count}};
}

std::vector<std::string> category_names(const std::vector<ItemCategoryRow>& rows) {
  std::vector<std::string> names(rows.size());
  for (std::size_t i = 0; i < rows.size(); ++i) {
    names[i] = rows[i].name;
    std::string path = rows[i].path;
    // 去掉自己的 id，剩余前缀逐级找父分类。
    const std::size_t last = path.find_last_of('/', path.size() - 2);
    if (last == std::string::npos || last == 0) continue;
    path = path.substr(0, last + 1);
    for (std::size_t j = 0; j < rows.size(); ++j) {
      if (rows[j].path == path) {
        names[i] = names[j] + "/" + rows[i].name;
        break;
      }
    }
  }
  return names;
}

}  // namespace

void register_masterdata_routes(httplib::Server& server, Services& services) {
  // 商品列表：分类子树、条码精确匹配、编码/名称/条码关键字。
  server.Get("/api/items",
             guard("item.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     ItemCategoryService categories(services.db);
                     std::vector<std::int64_t> category_ids;
                     const bool has_category = req.has_param("category_id");
                     if (has_category) {
                       const std::int64_t category_id =
                           param_int(req, "category_id", 0);
                       const bool include_children =
                           param_int(req, "include_children", 1) != 0;
                       if (category_id > 0 && include_children) {
                         category_ids = categories.subtree_ids(category_id);
                         if (category_ids.empty()) {
                           json_ok(res, nlohmann::json::array());
                           return;
                         }
                       } else {
                         category_ids.push_back(category_id);
                       }
                     }
                     std::string sql =
                         "SELECT i.id, i.code, i.name, i.spec, i.unit, i.unit_id,"
                         " i.category_id, COALESCE(c.name, ''), i.tax_rate_bp, i.barcode,"
                         " i.status FROM item i"
                         " LEFT JOIN item_category c ON c.id = i.category_id"
                         " WHERE i.is_deleted = 0";
                     std::vector<std::string> bindings;
                     if (has_category) {
                       if (category_ids.size() == 1) {
                         sql += " AND i.category_id = " + std::to_string(category_ids.front());
                       } else {
                         sql += " AND i.category_id IN (";
                         for (std::size_t i = 0; i < category_ids.size(); ++i) {
                           if (i != 0) sql += ", ";
                           sql += std::to_string(category_ids[i]);
                         }
                         sql += ")";
                       }
                     }
                     if (req.has_param("barcode")) {
                       const std::string barcode = req.get_param_value("barcode");
                       if (barcode.empty()) {
                         json_ok(res, nlohmann::json::array());
                         return;
                       }
                       sql += " AND i.barcode = ?";
                       bindings.push_back(barcode);
                     }
                     if (req.has_param("q")) {
                       sql +=
                           " AND (i.code LIKE ? ESCAPE '\\' OR i.name LIKE ? ESCAPE '\\'"
                           " OR i.barcode LIKE ? ESCAPE '\\')";
                       const std::string pattern = like_pattern(req.get_param_value("q"));
                       bindings.push_back(pattern);
                       bindings.push_back(pattern);
                       bindings.push_back(pattern);
                     }
                     sql += " ORDER BY i.code;";
                     nlohmann::json list = nlohmann::json::array();
                     services.db.read([&](sqlite3* conn) {
                       Stmt stmt(conn, sql);
                       for (std::size_t i = 0; i < bindings.size(); ++i) {
                         stmt.bind(static_cast<std::int64_t>(i + 1), bindings[i]);
                       }
                       while (stmt.step()) {
                         list.push_back(nlohmann::json{
                             {"id", stmt.i64(0)},
                             {"code", stmt.text(1)},
                             {"name", stmt.text(2)},
                             {"spec", stmt.text(3)},
                             {"unit", stmt.text(4)},
                             {"unit_id", stmt.i64(5)},
                             {"category_id", stmt.i64(6)},
                             {"category_name", stmt.text(7)},
                             {"tax_rate_bp", stmt.i64(8)},
                             {"barcode", stmt.text(9)},
                             {"status", stmt.i64(10) != 0},
                         });
                       }
                     });
                     json_ok(res, list);
                   }));

  server.Get("/api/item-categories",
             guard("item.read",
                   [&services](const httplib::Request&, httplib::Response& res) {
                     ItemCategoryService categories(services.db);
                     const auto rows = categories.list();
                     const auto names = category_names(rows);
                     nlohmann::json list = nlohmann::json::array();
                     for (std::size_t i = 0; i < rows.size(); ++i) {
                       list.push_back(category_json(rows[i], names[i]));
                     }
                     json_ok(res, list);
                   }));

  server.Post("/api/item-categories",
              guard("item.write",
                    [&services](const httplib::Request& req, httplib::Response& res) {
                      const auto body = json_body(req);
                      ItemCategoryService categories(services.db);
                      const auto row = categories.create(
                          body.value("parent_id", std::int64_t{0}),
                          body.value("name", std::string{}),
                          body.value("sort_order", std::int64_t{0}),
                          body.value("status", true));
                      const auto rows = categories.list();
                      const auto names = category_names(rows);
                      for (std::size_t i = 0; i < rows.size(); ++i) {
                        if (rows[i].id == row.id) {
                          json_ok(res, category_json(rows[i], names[i]));
                          return;
                        }
                      }
                      json_ok(res, category_json(row, row.name));
                    }));

  server.Put(R"(/api/item-categories/(\d+))",
             guard("item.write",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const std::int64_t id = std::stoll(req.matches[1].str());
                     const auto body = json_body(req);
                     ItemCategoryService categories(services.db);
                     categories.update(id, body.value("parent_id", std::int64_t{0}),
                                       body.value("name", std::string{}),
                                       body.value("sort_order", std::int64_t{0}),
                                       body.value("status", true));
                     const auto rows = categories.list();
                     const auto names = category_names(rows);
                     for (std::size_t i = 0; i < rows.size(); ++i) {
                       if (rows[i].id == id) {
                         json_ok(res, category_json(rows[i], names[i]));
                         return;
                       }
                     }
                     json_ok(res, nlohmann::json{{"id", id}});
                   }));

  server.Delete(R"(/api/item-categories/(\d+))",
                guard("item.write",
                      [&services](const httplib::Request& req, httplib::Response& res) {
                        const std::int64_t id = std::stoll(req.matches[1].str());
                        ItemCategoryService categories(services.db);
                        categories.remove(id);
                        json_ok(res, nlohmann::json{{"ok", true}});
                      }));

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
