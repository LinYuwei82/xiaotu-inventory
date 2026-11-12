#include "xiaotu/setting_service.h"

#include <sqlite3.h>

#include <string>

#include "xiaotu/audit.h"
#include "xiaotu/context.h"
#include "xiaotu/error.h"

namespace xiaotu {
namespace {

std::string read_value(sqlite3* conn, const char* key) {
  Stmt stmt(conn, "SELECT value FROM app_setting WHERE key = ?;");
  stmt.bind(1, key);
  return stmt.step() ? stmt.text(0) : std::string{};
}

void check_length(const std::string& value, std::size_t limit, const char* label) {
  if (value.size() > limit) {
    throw AppError("INVALID_INPUT",
                   std::string(label) + "不能超过 " + std::to_string(limit) + " 个字符");
  }
}

// 审计 detail 是 JSON，值里的引号与反斜杠要转义；换行压成空格便于在列表里看。
std::string json_escape(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (const char ch : text) {
    switch (ch) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
      case '\r':
        out += ' ';
        break;
      default:
        out += ch;
    }
  }
  return out;
}

void save_one(sqlite3* conn, const char* key, const std::string& value) {
  const std::string previous = read_value(conn, key);
  if (previous == value) {
    return;
  }
  Stmt stmt(conn,
            "UPDATE app_setting SET value = ?, updated_at = ?, updated_by = ? WHERE key = ?;");
  stmt.bind(1, value).bind(2, now_text()).bind(3, current_user()).bind(4, key);
  stmt.run();
  audit_write(conn, "update", "setting", 0, key,
              "{\"from\":\"" + json_escape(previous) + "\",\"to\":\"" +
                  json_escape(value) + "\"}");
}

}  // namespace

CompanySettings SettingService::company() const {
  CompanySettings out;
  db_.read([&](sqlite3* conn) {
    out.name = read_value(conn, "company_name");
    out.phone = read_value(conn, "company_phone");
    out.address = read_value(conn, "company_address");
  });
  return out;
}

CompanySettings SettingService::update_company(const CompanySettings& input) {
  check_length(input.name, 100, "公司名称");
  check_length(input.phone, 50, "联系电话");
  check_length(input.address, 200, "公司地址");
  db_.write([&](sqlite3* conn) {
    save_one(conn, "company_name", input.name);
    save_one(conn, "company_phone", input.phone);
    save_one(conn, "company_address", input.address);
  });
  return company();
}

}  // namespace xiaotu
