// HTTP 冒烟：网络版走口令登录，单机版走一次性令牌换取会话；两套形态都验证
// 健康检查、鉴权拦截、主数据读写、导出与审计埋点。
#include <httplib.h>
#include <sqlite3.h>

#include <filesystem>
#include <fstream>
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
  services.users.create(xiaotu::UserInput{.username = "backup_viewer",
                                          .display_name = "备份只读",
                                          .role = "viewer",
                                          .active = true,
                                          .password = "Viewer0rd!"});

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
  const nlohmann::json health_body =
      health ? nlohmann::json::parse(health->body) : nlohmann::json::object();
  check(health_body.contains("backup") && health_body["backup"].contains("last_success") &&
            health_body["backup"].contains("last_failure") && health_body["backup"].contains("due"),
        "/api/health 应返回备份成功/失败状态");
  check(health_body.contains("backup") && health_body["backup"].value("count", -1) == 0 &&
            health_body["backup"]["last_success"].is_null() &&
            health_body["backup"]["last_failure"].is_null(),
        "/api/health 初始应无备份记录");
  check(health_body.contains("backup") &&
            health_body["backup"].contains("database_writable") &&
            health_body["backup"]["database_writable"] == true,
        "/api/health 应返回数据库可写状态");
  check(health && health->body.find(dir.string()) == std::string::npos,
        "/api/health 不应返回绝对路径");

  // 写入一份同时包含成功与失败的状态文件，验证两类状态可区分且不泄漏路径。
  std::filesystem::create_directories(dir / "backups");
  {
    std::ofstream status(dir / "backups" / "backup-status.json");
    status << R"({"last_success":{"at":"2026-11-13 09:00:00",)"
              R"("file":"app-20261113-090000-daily.db","bytes":4096},)"
              R"("last_failure":{"at":"2026-11-12 02:30:00","reason":"磁盘已满"},)"
              R"("schema_version":26})";
  }
  {
    std::ofstream fake(dir / "backups" / "app-20261113-090000-daily.db");
    fake << "";
  }
  const auto health_after = client.Get("/api/health");
  const nlohmann::json backup_after =
      health_after ? nlohmann::json::parse(health_after->body)["backup"] : nlohmann::json{};
  check(backup_after["last_success"].is_object() && backup_after["last_failure"].is_object(),
        "health 应分别返回最近成功与最近失败");
  check(backup_after["last_success"].value("file", "").find('/') == std::string::npos,
        "备份文件名不应包含目录");
  check(backup_after.value("count", -1) == 1, "health 应统计可用备份数");
  check(health_after && health_after->body.find(dir.string()) == std::string::npos,
        "health 备份状态不应包含数据目录绝对路径");

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

  // 手动备份接口：管理员可执行，返回文件名/大小/时间且不含绝对路径。
  const auto backup_done = client.Post("/api/backup", "", "application/json");
  check(backup_done && backup_done->status == 200, "管理员手动备份应返回 200");
  if (backup_done) {
    const auto data = nlohmann::json::parse(backup_done->body);
    check(data.value("file", "").find('/') == std::string::npos &&
              data.value("file", "").find("-manual") != std::string::npos,
          "手动备份应返回文件名且不含目录");
    check(data.value("bytes", 0LL) > 0, "手动备份应返回文件大小");
    check(data.contains("at"), "手动备份应返回完成时间");
    check(backup_done->body.find(dir.string()) == std::string::npos,
          "手动备份响应不应包含绝对路径");
  }
  check(audit_count(db, "backup", "database") >= 1, "手动备份应写审计");

  // 同一目录已有备份任务时返回 409，而不是静默成功。
  std::filesystem::create_directories(dir / "backups" / ".backup.lock");
  const auto backup_busy = client.Post("/api/backup", "", "application/json");
  check(backup_busy && backup_busy->status == 409, "备份目录被占用时应返回 409");
  if (backup_busy) {
    check(nlohmann::json::parse(backup_busy->body).value("code", "") == "BACKUP_BUSY",
          "备份占用应返回 BACKUP_BUSY");
  }
  // 抢锁失败是拒绝执行而非备份失败，不应写「备份失败」审计或污染状态。
  check(audit_count(db, "backup", "database", "失败：") == 0,
        "抢锁失败不应写备份失败审计");
  std::filesystem::remove_all(dir / "backups" / ".backup.lock");

  // 未登录 401；无 setting.write 的只读角色 403。
  httplib::Client anonymous_backup("127.0.0.1", port);
  const auto anonymous_result = anonymous_backup.Post("/api/backup", "", "application/json");
  check(anonymous_result && anonymous_result->status == 401, "未登录手动备份应返回 401");

  httplib::Client viewer("127.0.0.1", port);
  const auto viewer_login = viewer.Post("/api/login",
                                        R"({"username":"backup_viewer","password":"Viewer0rd!"})",
                                        "application/json");
  check(viewer_login && viewer_login->status == 200, "只读账号应能登录");
  if (viewer_login) {
    viewer.set_default_headers({{"Cookie", session_cookie(viewer_login)},
                                {"X-CSRF-Token", body_field(viewer_login, "csrf")}});
    const auto viewer_backup = viewer.Post("/api/backup", "", "application/json");
    check(viewer_backup && viewer_backup->status == 403, "无权限角色手动备份应返回 403");
  }

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

  // 一商品多码：主码 + 备用码，扫码命中副码，列表回传全部码。
  const std::string item_path = "/api/items/" + std::to_string(item_body.value("id", 0LL)) +
                                "/barcodes";
  const auto codes_saved = client.Put(item_path,
                                      R"({"barcodes":["6900000000012","6900000000029"]})",
                                      "application/json");
  check(codes_saved && codes_saved->status == 200, "保存多个条码应返回 200");
  if (codes_saved) {
    const auto codes = nlohmann::json::parse(codes_saved->body)["barcodes"];
    check(codes.size() == 2 && codes[0].value("primary", false) &&
              codes[0].value("barcode", "") == "6900000000012",
          "第一个条码应为主码");
  }
  const auto scanned = client.Get("/api/items?barcode=6900000000029");
  check(scanned && scanned->status == 200, "按备用码扫码应返回 200");
  if (scanned) {
    const auto rows = nlohmann::json::parse(scanned->body);
    check(rows.is_array() && rows.size() == 1 &&
              rows[0].value("code", "") == "SMOKE1",
          "按备用码扫码应命中同一商品");
  }
  const auto listed_codes = client.Get("/api/items?q=SMOKE1");
  if (listed_codes) {
    const auto rows = nlohmann::json::parse(listed_codes->body);
    check(rows.size() == 1 && rows[0]["barcodes"].size() == 2,
          "列表应回传主码与备用码");
  }

  const auto created2 = client.Post(
      "/api/items", R"({"code":"SMOKE2","name":"冒烟商品二","unit_id":1})", "application/json");
  const auto item2 =
      created2 ? nlohmann::json::parse(created2->body) : nlohmann::json::object();
  const auto conflict = client.Put(
      "/api/items/" + std::to_string(item2.value("id", 0LL)) + "/barcodes",
      R"({"barcodes":["6900000000012"]})", "application/json");
  check(conflict && conflict->status != 200 &&
            conflict->body.find("条码已被商品") != std::string::npos,
        "与其他商品条码冲突应被拒绝");
  const auto duplicated = client.Put(item_path,
                                     R"({"barcodes":["6900000000036","6900000000036"]})",
                                     "application/json");
  check(duplicated && duplicated->status != 200 &&
            duplicated->body.find("条码重复") != std::string::npos,
        "同一商品重复条码应被拒绝");

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
    // 版式布局由服务端一次算清，页面 / 打印 / PDF 共用同一份数字。
    check(render.contains("layout"), "打印渲染应返回共享的版式布局");
    if (render.contains("layout")) {
      const auto& layout = render["layout"];
      check(layout.value("width_mm", 0) == 210 && layout.value("height_mm", 0) == 297 &&
                layout.value("page_size", "") == "A4",
            "内置模板的布局应为 A4 210×297mm");
      check(layout.value("table_width_mm", 0) == 150 &&
                layout.value("printable_width_mm", 0) == 186 &&
                layout.value("scale_percent", 0) == 100,
            "销售毛利 150mm 在 A4 上应 100% 输出");
      check(layout.value("warn", "") == "none", "宽度足够时不应告警");
    }
  }

  // 诊断信息：只读、无敏感字段、数值与当前库一致。
  const auto diagnostic = client.Get("/api/diagnostics");
  check(diagnostic && diagnostic->status == 200, "诊断信息应返回 200");
  if (diagnostic) {
    const auto data = nlohmann::json::parse(diagnostic->body);
    check(data.value("schema_version", 0) == 30, "诊断信息里的 schema 应为 30");
    check(data.value("migrations", 0) == 30, "诊断信息里的迁移条数应为 30");
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
