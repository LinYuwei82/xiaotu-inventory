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

int read_int(sqlite3* conn, const char* key, int fallback) {
  const std::string text = read_value(conn, key);
  if (text.empty()) {
    return fallback;
  }
  try {
    return std::stoi(text);
  } catch (const std::exception&) {
    return fallback;
  }
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

SystemSettings SettingService::load() const {
  SystemSettings out;
  db_.read([&](sqlite3* conn) {
    out.company.name = read_value(conn, "company_name");
    out.company.phone = read_value(conn, "company_phone");
    out.company.address = read_value(conn, "company_address");
    out.allow_negative_stock = read_value(conn, "allow_negative_stock") == "1";
    out.session_idle_minutes = read_int(conn, "session_idle_minutes", 30);
    out.session_absolute_hours = read_int(conn, "session_absolute_hours", 12);
    out.cost_basis = read_value(conn, "cost_basis");
    if (out.cost_basis != "NET") {
      // 缺失或脏值一律回落到默认的含税口径。
      out.cost_basis = "INCLUSIVE";
    }
    out.cost_basis_changed_at = read_value(conn, "cost_basis_changed_at");
  });
  return out;
}

SessionPolicy SettingService::session_policy() const {
  SessionPolicy out;
  db_.read([&](sqlite3* conn) {
    out.idle_minutes = read_int(conn, "session_idle_minutes", 30);
    out.absolute_hours = read_int(conn, "session_absolute_hours", 12);
  });
  return out;
}

CompanySettings SettingService::company() const { return load().company; }

SystemSettings SettingService::update(const SystemSettings& input) {
  check_length(input.company.name, 100, "公司名称");
  check_length(input.company.phone, 50, "联系电话");
  check_length(input.company.address, 200, "公司地址");
  if (input.session_idle_minutes < 1 || input.session_idle_minutes > 1440) {
    throw AppError("INVALID_INPUT", "会话空闲超时应在 1–1440 分钟之间");
  }
  if (input.session_absolute_hours < 1 || input.session_absolute_hours > 720) {
    throw AppError("INVALID_INPUT", "会话绝对上限应在 1–720 小时之间");
  }
  if (input.session_absolute_hours * 60 <= input.session_idle_minutes) {
    throw AppError("INVALID_INPUT", "会话绝对上限必须大于空闲超时");
  }
  if (input.cost_basis != "INCLUSIVE" && input.cost_basis != "NET") {
    throw AppError("INVALID_INPUT", "成本口径只支持 INCLUSIVE 或 NET");
  }
  db_.write([&](sqlite3* conn) {
    save_one(conn, "company_name", input.company.name);
    save_one(conn, "company_phone", input.company.phone);
    save_one(conn, "company_address", input.company.address);
    save_one(conn, "allow_negative_stock", input.allow_negative_stock ? "1" : "0");
    save_one(conn, "session_idle_minutes", std::to_string(input.session_idle_minutes));
    save_one(conn, "session_absolute_hours", std::to_string(input.session_absolute_hours));
    // 口径真正发生变化时记录生效时间；重复保存同一值不刷新切换日。
    const bool basis_changed = read_value(conn, "cost_basis") != input.cost_basis;
    save_one(conn, "cost_basis", input.cost_basis);
    if (basis_changed) {
      Stmt stmt(conn,
                "UPDATE app_setting SET value = ?, updated_at = ?, updated_by = ?"
                " WHERE key = 'cost_basis_changed_at';");
      stmt.bind(1, now_text()).bind(2, now_text()).bind(3, current_user()).run();
    }
  });
  return load();
}

}  // namespace xiaotu
