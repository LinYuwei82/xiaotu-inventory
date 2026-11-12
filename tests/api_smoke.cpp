// HTTP 冒烟：网络版走口令登录，单机版走一次性令牌换取会话；两套形态都验证
// 健康检查、鉴权拦截、主数据读写、导出与审计埋点。
#include <httplib.h>
#include <sqlite3.h>

#include <filesystem>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "xiaotu/db.h"
#include "xiaotu/http_server.h"
#include "xiaotu/migrations.h"
#include "xiaotu/permissions.h"
#include "xiaotu/services.h"

namespace {

int g_failures = 0;

void check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << std::endl;
    ++g_failures;
  }
}

std::string session_cookie(const httplib::Result& result) {
  const std::string header = result->get_header_value("Set-Cookie");
  const std::size_t end = header.find(';');
  return header.substr(0, end == std::string::npos ? header.size() : end);
}

std::string body_field(const httplib::Result& result, const char* key) {
  const auto body = nlohmann::json::parse(result->body);
  return body.value(key, std::string{});
}

std::int64_t audit_count(xiaotu::Db& db, const std::string& action,
                         const std::string& entity = {},
                         const std::string& detail_prefix = {}) {
  std::int64_t count = 0;
  db.read([&](sqlite3* conn) {
    std::string sql = "SELECT COUNT(*) FROM audit_log WHERE action = ?";
    if (!entity.empty()) {
      sql += " AND entity = ?";
    }
    if (!detail_prefix.empty()) {
      sql += " AND detail LIKE ?";
    }
    sql += ";";
    xiaotu::Stmt stmt(conn, sql);
    int index = 1;
    stmt.bind(index++, action);
    if (!entity.empty()) {
      stmt.bind(index++, entity);
    }
    if (!detail_prefix.empty()) {
      stmt.bind(index++, detail_prefix + "%");
    }
    if (stmt.step()) {
      count = stmt.i64(0);
    }
  });
  return count;
}

void open_database(const std::filesystem::path& dir) {
  std::filesystem::create_directories(dir);
}

// 网络版：口令登录成功/失败、权限拦截、主数据写入、CSV/XLSX 导出与审计。
void run_server_mode(const std::filesystem::path& dir) {
  open_database(dir);
  xiaotu::Db db(dir / "app.db", 2);
  xiaotu::migrate(db);
  xiaotu::load_role_permissions(db);
  xiaotu::Services services(db);
  services.users.set_password(1, "Passw0rd!");

  xiaotu::HttpServer http(
      services,
      xiaotu::ServerInfo{.mode = "server",
                         .app_name = "小兔进销存",
                         .version = "test",
                         .data_dir = dir.string(),
                         .database_path = (dir / "app.db").string(),
                         .listen_host = "127.0.0.1"},
      "");
  const int port = http.bind("127.0.0.1", 0);
  http.serve_async();
  httplib::Client client("127.0.0.1", port);

  const auto health = client.Get("/api/health");
  check(health && health->status == 200, "网络版 /api/health 应返回 200");
  check(health && nlohmann::json::parse(health->body).contains("data_dir_warning"),
        "/api/health 应返回数据目录告警字段");

  const auto anonymous = client.Get("/api/items");
  check(anonymous && anonymous->status == 401, "未登录访问主数据应返回 401");

  const auto failed = client.Post("/api/login",
                                  R"({"username":"admin","password":"wrong"})",
                                  "application/json");
  check(failed && failed->status == 401, "错误口令应返回 401");
  check(audit_count(db, "login_failed") == 1, "登录失败应写一条审计");

  const auto login = client.Post("/api/login",
                                 R"({"username":"admin","password":"Passw0rd!"})",
                                 "application/json");
  check(login && login->status == 200, "正确口令应登录成功");
  const std::string cookie = login ? session_cookie(login) : std::string{};
  const std::string csrf = login ? body_field(login, "csrf") : std::string{};
  check(cookie.rfind("sid=", 0) == 0 && !csrf.empty(), "登录应返回会话 Cookie 与 CSRF 令牌");
  check(audit_count(db, "login") == 1, "登录成功应写一条审计");

  client.set_default_headers({{"Cookie", cookie}, {"X-CSRF-Token", csrf}});

  const auto me = client.Get("/api/me");
  check(me && me->status == 200, "登录后 /api/me 应返回 200");

  const auto created = client.Post(
      "/api/items", R"({"code":"SMOKE1","name":"冒烟商品","unit_id":1})", "application/json");
  check(created && created->status == 200, "创建商品应成功");
  const auto item_body =
      created ? nlohmann::json::parse(created->body) : nlohmann::json::object();
  check(item_body.value("id", 0LL) > 0 && item_body.value("code", "") == "SMOKE1",
        "创建商品应返回 id 与编码");

  const auto listed = client.Get("/api/items?q=SMOKE1");
  check(listed && listed->status == 200, "按关键字查询商品应返回 200");
  if (listed) {
    const auto rows = nlohmann::json::parse(listed->body);
    check(rows.is_array() && rows.size() == 1, "按关键字查询应命中一条商品");
  }

  const auto csv = client.Get("/api/reports/stock-balance?format=csv");
  check(csv && csv->status == 200 && csv->get_header_value("Content-Type").rfind("text/csv", 0) == 0,
        "库存余额导出 CSV 应返回 200");
  const auto xlsx = client.Get("/api/items/export?format=xlsx");
  check(xlsx && xlsx->status == 200, "主数据导出 XLSX 应返回 200");
  check(audit_count(db, "export") == 2, "两次导出应各写一条审计");

  // 系统参数：默认三项为空，保存后回读一致，每个变化的键各一条审计。
  const auto settings = client.Get("/api/settings");
  check(settings && settings->status == 200, "系统参数读取应返回 200");
  if (settings) {
    const auto company = nlohmann::json::parse(settings->body).value("company", nlohmann::json::object());
    check(company.contains("name") && company.contains("phone") && company.contains("address"),
          "系统参数应返回公司抬头三项");
    check(company.value("name", "").empty(), "公司名称初始应为空");
  }
  const auto saved = client.Put(
      "/api/settings",
      R"({"company":{"name":"冒烟公司","phone":"010-1234","address":"测试路 1 号"}})",
      "application/json");
  check(saved && saved->status == 200, "保存系统参数应返回 200");
  if (saved) {
    const auto company = nlohmann::json::parse(saved->body).value("company", nlohmann::json::object());
    check(company.value("name", "") == "冒烟公司" && company.value("phone", "") == "010-1234" &&
              company.value("address", "") == "测试路 1 号",
          "保存后应返回新的公司抬头");
  }
  check(audit_count(db, "update", "setting") == 3, "三个变化的参数键应各写一条审计");

  const auto toggled = client.Put("/api/settings", R"({"allow_negative_stock":true})",
                                  "application/json");
  check(toggled && toggled->status == 200 &&
            nlohmann::json::parse(toggled->body).value("allow_negative_stock", false),
        "开启负库存开关后应返回 true");
  const auto settings_after = client.Get("/api/settings");
  check(settings_after && settings_after->status == 200 &&
            nlohmann::json::parse(settings_after->body).value("allow_negative_stock", false),
        "重新读取应看到负库存开关为开启");
  check(audit_count(db, "update", "setting") == 4, "开关变化应再写一条审计");
  client.Put("/api/settings", R"({"allow_negative_stock":false})", "application/json");
  check(audit_count(db, "update", "setting") == 5, "关回开关也应写一条审计");

  const auto timeout_saved = client.Put(
      "/api/settings",
      R"({"session_idle_minutes":45,"session_absolute_hours":10})", "application/json");
  check(timeout_saved && timeout_saved->status == 200, "保存会话超时应返回 200");
  if (timeout_saved) {
    const auto data = nlohmann::json::parse(timeout_saved->body);
    check(data.value("session_idle_minutes", 0) == 45 &&
              data.value("session_absolute_hours", 0) == 10,
          "应返回新的会话超时值");
  }
  check(audit_count(db, "update", "setting") == 7, "两个会话超时键应各写一条审计");

  // 模板抬头为空时，打印用公司名称兜底。
  const auto printed = client.Get("/api/print/render?target_type=REPORT&target_key=sales-profit");
  check(printed && printed->status == 200, "打印渲染应返回 200");
  if (printed) {
    const auto render = nlohmann::json::parse(printed->body);
    check(render.value("header_text", "") == "冒烟公司",
          "模板抬头为空时应默认打印公司名称");
  }

  // 诊断信息：只读、无敏感字段、数值与当前库一致。
  const auto diagnostic = client.Get("/api/diagnostics");
  check(diagnostic && diagnostic->status == 200, "诊断信息应返回 200");
  if (diagnostic) {
    const auto data = nlohmann::json::parse(diagnostic->body);
    check(data.value("schema_version", 0) == 24, "诊断信息里的 schema 应为 24");
    check(data.value("migrations", 0) == 24, "诊断信息里的迁移条数应为 24");
    check(data.value("mode", "") == "server", "网络版诊断信息 mode 应为 server");
    check(!data.value("data_dir", "").empty(), "诊断信息应包含数据目录");
    check(data.value("database_bytes", 0LL) > 0, "诊断信息应包含大于 0 的数据库大小");
    check(data.value("listen", "").rfind("127.0.0.1:", 0) == 0, "诊断信息应包含监听地址");
    check(data.value("user", "") == "admin" && data.value("role", "") == "admin",
          "诊断信息应包含当前用户与角色");
    check(data.contains("build") && data["build"].contains("compiler"),
          "诊断信息应包含编译器与构建时间");
    check(!data.contains("password") && !data.contains("token") && !data.contains("csrf"),
          "诊断信息不应包含口令、令牌或 CSRF");
  }

  http.stop();
}

// 单机版：一次性令牌换会话；令牌只能用一次。
void run_local_mode(const std::filesystem::path& dir) {
  open_database(dir);
  xiaotu::Db db(dir / "app.db", 2);
  xiaotu::migrate(db);
  xiaotu::load_role_permissions(db);
  xiaotu::Services services(db);

  xiaotu::HttpServer http(
      services,
      xiaotu::ServerInfo{.mode = "local",
                         .app_name = "小兔进销存",
                         .version = "test",
                         .data_dir = dir.string(),
                         .database_path = (dir / "app.db").string()},
      "local-smoke-token");
  const int port = http.bind("127.0.0.1", 0);
  http.serve_async();
  httplib::Client client("127.0.0.1", port);

  const auto session = client.Post("/api/session", R"({"token":"local-smoke-token"})",
                                   "application/json");
  check(session && session->status == 200, "单机版令牌应能换取会话");
  const std::string cookie = session ? session_cookie(session) : std::string{};
  const std::string csrf = session ? body_field(session, "csrf") : std::string{};
  check(!cookie.empty() && !csrf.empty(), "令牌登录应返回会话与 CSRF");
  check(audit_count(db, "login", "", "单机令牌") == 1, "单机令牌登录应写一条审计");

  const auto replay = client.Post("/api/session", R"({"token":"local-smoke-token"})",
                                  "application/json");
  check(replay && replay->status == 401, "一次性令牌不能重复使用");

  client.set_default_headers({{"Cookie", cookie}, {"X-CSRF-Token", csrf}});
  const auto me = client.Get("/api/me");
  check(me && me->status == 200, "单机版登录后 /api/me 应返回 200");
  const auto items = client.Get("/api/items");
  check(items && items->status == 200, "单机版主数据列表应返回 200");

  const auto diagnostic = client.Get("/api/diagnostics");
  check(diagnostic && diagnostic->status == 200, "单机版诊断信息应返回 200");
  if (diagnostic) {
    const auto data = nlohmann::json::parse(diagnostic->body);
    check(data.value("mode", "") == "local", "单机版诊断信息 mode 应为 local");
    check(data.value("user", "") == "admin", "单机版诊断信息应包含当前用户");
    check(!data.value("listen", "").empty(), "单机版诊断信息应包含自动分配的监听地址");
  }

  http.stop();
}

}  // namespace

int main() {
  namespace fs = std::filesystem;
  const fs::path root = fs::temp_directory_path() / "xiaotu-api-smoke";
  try {
    fs::remove_all(root);
    run_server_mode(root / "server");
    run_local_mode(root / "local");
    fs::remove_all(root);
  } catch (const std::exception& error) {
    std::cerr << "异常: " << error.what() << std::endl;
    ++g_failures;
  }

  if (g_failures != 0) {
    std::cerr << g_failures << " 项失败" << std::endl;
    return 1;
  }
  std::cout << "api_smoke 通过" << std::endl;
  return 0;
}
