// HTTP 冒烟：网络版走口令登录，单机版走一次性令牌换取会话；两套形态都验证
// 健康检查、鉴权拦截、主数据读写、导出与审计埋点。
#include <httplib.h>
#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#include "xiaotu/audit.h"
#include "xiaotu/db.h"
#include "xiaotu/document.h"
#include "xiaotu/http_server.h"
#include "xiaotu/migrations.h"
#include "xiaotu/permissions.h"
#include "xiaotu/qty.h"
#include "xiaotu/services.h"
#include "xiaotu/xlsx_reader.h"

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

// 发一帧不带 Content-Length 的 POST，返回耗时（毫秒）与响应首行。
// 服务端应在读完请求头后立即按空 body 处理，而不是等满读超时。
std::pair<long long, std::string> post_without_content_length(int port, const std::string& path) {
  const auto started = std::chrono::steady_clock::now();
#if defined(_WIN32)
  WSADATA data{};
  WSAStartup(MAKEWORD(2, 2), &data);
  const SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
  const auto close_socket = [](SOCKET handle) { closesocket(handle); };
  const bool valid = sock != INVALID_SOCKET;
#else
  const int sock = socket(AF_INET, SOCK_STREAM, 0);
  const auto close_socket = [](int handle) { ::close(handle); };
  const bool valid = sock >= 0;
#endif
  std::string first_line;
  if (valid) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<unsigned short>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
      const std::string request = "POST " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
      ::send(sock, request.data(), static_cast<int>(request.size()), 0);
      char buffer[256] = {};
      const int received = static_cast<int>(::recv(sock, buffer, sizeof(buffer) - 1, 0));
      if (received > 0) {
        buffer[received] = '\0';
        first_line.assign(buffer);
        const auto end = first_line.find("\r\n");
        if (end != std::string::npos) first_line.resize(end);
      }
    }
    close_socket(sock);
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();
  return {elapsed, first_line};
}

std::int64_t audit_count(xiaotu::Db& db, const std::string& action,
                         const std::string& entity = {},
                         const std::string& detail_prefix = {}) {  std::int64_t count = 0;
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

std::string scalar_text(xiaotu::Db& db, const std::string& sql) {
  std::string value;
  db.read([&](sqlite3* conn) {
    xiaotu::Stmt stmt(conn, sql);
    if (stmt.step()) {
      value = stmt.text(0);
    }
  });
  return value;
}

// 测试夹具：直接建库时不希望触发「必须改密」，只重置登录标记。
void clear_must_change(xiaotu::Db& db, std::int64_t id) {
  db.write([&](sqlite3* conn) {
    xiaotu::exec_sql(conn, "UPDATE app_user SET must_change_password = 0 WHERE id = " +
                               std::to_string(id) + ";");
  });
}

// 网络版：口令登录成功/失败、权限拦截、主数据写入、CSV/XLSX 导出与审计。
void run_server_mode(const std::filesystem::path& dir) {
  open_database(dir);
  xiaotu::Db db(dir / "app.db", 2);
  xiaotu::migrate(db);
  xiaotu::load_role_permissions(db);
  xiaotu::Services services(db);
  services.users.set_password(1, "Passw0rd!");
  clear_must_change(db, 1);
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
  const bool community = health_body.value("edition", "") == "community";

  // 缺少 Content-Length 的 POST 必须立即按空 body 处理（回归：曾等满 15s 读超时）。
  const auto [no_length_ms, no_length_line] = post_without_content_length(port, "/api/session");
  check(no_length_ms < 2000,
        "缺少 Content-Length 的 POST 应立即处理，实际耗时 " + std::to_string(no_length_ms) + "ms");
  check(no_length_line.rfind("HTTP/1.1", 0) == 0,
        "缺少 Content-Length 的 POST 应返回 HTTP 响应");
  check(health_body.contains("api_version") && health_body.contains("capabilities") &&
            health_body["capabilities"].contains("features") &&
            health_body["capabilities"].contains("quotas"),
        "/api/health 应返回 API 版本与能力表");
  {
    // 前端能力文件由同一份 editions.json 生成，必须与 /api/health 完全一致。
    const auto caps_js = client.Get("/gen/edition_caps.js");
    check(caps_js && caps_js->status == 200, "前端能力文件应可访问");
    if (caps_js) {
      const auto begin = caps_js->body.find('{');
      const auto end = caps_js->body.rfind('}');
      check(begin != std::string::npos && end != std::string::npos, "能力文件应内嵌 JSON");
      if (begin != std::string::npos && end != std::string::npos) {
        const auto caps = nlohmann::json::parse(caps_js->body.substr(begin, end - begin + 1));
        const auto code = health_body.value("edition", "");
        const auto edition_entry =
            caps["editions"].contains(code) ? caps["editions"][code] : nlohmann::json::object();
        check(edition_entry.contains("features") &&
                  edition_entry["features"] == health_body["capabilities"]["features"],
              "前端能力表应与 /api/health 一致（同一源头）");
        check(caps.value("api_version", 0) == health_body.value("api_version", 0),
              "能力文件与 health 的 API 版本应一致");
      }
    }
  }
  {
    // 能力协商：不兼容版本回 426，health 始终放行便于客户端协商。
    httplib::Client versioned("127.0.0.1", port);
    versioned.set_default_headers({{"X-API-Version", "999"}});
    const auto rejected = versioned.Get("/api/items");
    check(rejected && rejected->status == 426 &&
              rejected->body.find("API_VERSION_UNSUPPORTED") != std::string::npos,
          "不兼容的 API 版本应返回 426");
    const auto probe = versioned.Get("/api/health");
    check(probe && probe->status == 200, "health 应始终可访问以便协商");
  }
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
  check(health_body["backup"].value("backup_dir_is_custom", true) == false,
        "/api/health 应说明备份目录是否单独指定（默认未指定）");
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

  // 登录失败锁定：连续 5 次错口令后，第 6 次即使口令正确也被拒绝。
  const auto lock_user = services.users.create(xiaotu::UserInput{.username = "lock_user",
                                                                 .display_name = "锁定测试",
                                                                 .role = "viewer",
                                                                 .active = true,
                                                                 .password = "LockPassw0rd!"});
  const std::string lock_login_ok =
      R"({"username":"lock_user","password":"LockPassw0rd!"})";
  const std::string lock_login_bad =
      R"({"username":"lock_user","password":"bad-password"})";
  httplib::Client locker("127.0.0.1", port);
  for (int attempt = 1; attempt <= 5; ++attempt) {
    const auto result = locker.Post("/api/login", lock_login_bad, "application/json");
    check(result && result->status == (attempt < 5 ? 401 : 429),
          "连续错口令第 " + std::to_string(attempt) + " 次的响应状态应正确");
  }
  // 之前 admin 的一次错口令也算在内，共 6 条「口令错误」。
  check(audit_count(db, "login_failed", "auth", "口令错误") == 6,
        "每次口令错误都应写一条登录失败审计");
  check(audit_count(db, "account_locked") == 1, "达到阈值应写一条账号锁定审计");
  check(scalar_text(db, "SELECT locked_until FROM app_user WHERE username = 'lock_user';") != "",
        "达到阈值后应写入锁定截止时间");

  const auto still_locked = locker.Post("/api/login", lock_login_ok, "application/json");
  check(still_locked && still_locked->status == 429 &&
            still_locked->body.find("账号已锁定") != std::string::npos,
        "锁定期间即使口令正确也应拒绝并给出可读提示");
  check(audit_count(db, "login_failed", "auth", "账号锁定") == 1,
        "锁定期间的登录尝试应写一条账号锁定原因的失败审计");

  // 锁定到期后可登录，且成功登录清零计数与锁定时间。
  db.write([&](sqlite3* conn) {
    xiaotu::exec_sql(conn,
                     "UPDATE app_user SET locked_until = datetime('now', 'localtime',"
                     " '-1 minute') WHERE username = 'lock_user';");
  });
  const auto recovered = locker.Post("/api/login", lock_login_ok, "application/json");
  check(recovered && recovered->status == 200, "锁定到期后正确口令应能登录");
  check(scalar_text(db, "SELECT CAST(failed_count AS TEXT) FROM app_user"
                        " WHERE username = 'lock_user';") == "0",
        "登录成功应清零失败计数");
  check(scalar_text(db, "SELECT locked_until FROM app_user WHERE username = 'lock_user';").empty(),
        "登录成功应清除锁定截止时间");

  // 管理员重置口令：清零计数并强制改密，改密前访问业务接口被拒。
  const auto reset = client.Put(
      "/api/users/" + std::to_string(lock_user.id),
      R"({"username":"lock_user","display_name":"锁定测试","role":"viewer","active":true,)"
      R"("password":"ResetPassw0rd!"})",
      "application/json");
  check(reset && reset->status == 200, "管理员重置口令应返回 200");
  check(scalar_text(db, "SELECT CAST(must_change_password AS TEXT) FROM app_user"
                        " WHERE username = 'lock_user';") == "1",
        "管理员重置口令后应置强制改密标记");

  httplib::Client forced("127.0.0.1", port);
  const auto forced_login = forced.Post(
      "/api/login", R"({"username":"lock_user","password":"ResetPassw0rd!"})",
      "application/json");
  check(forced_login && forced_login->status == 200, "重置后的临时口令应能登录");
  check(forced_login &&
            nlohmann::json::parse(forced_login->body).value("must_change_password", false),
        "登录响应应告知前端必须改密");
  forced.set_default_headers({{"Cookie", forced_login ? session_cookie(forced_login) : ""},
                              {"X-CSRF-Token", body_field(forced_login, "csrf")}});
  const auto forced_items = forced.Get("/api/items");
  check(forced_items && forced_items->status == 403 &&
            nlohmann::json::parse(forced_items->body).value("code", "") == "MUST_CHANGE_PASSWORD",
        "改密前访问业务接口应返回 MUST_CHANGE_PASSWORD");
  const auto forced_me = forced.Get("/api/me");
  check(forced_me && forced_me->status == 200 &&
            nlohmann::json::parse(forced_me->body).value("must_change_password", false),
        "改密前仍应能读取自身信息");

  const auto wrong_old = forced.Post(
      "/api/password/change",
      R"({"old_password":"not-the-old-one","new_password":"ChangedPassw0rd!"})",
      "application/json");
  check(wrong_old && wrong_old->status != 200, "原口令不正确时应拒绝改密");

  const auto changed = forced.Post(
      "/api/password/change",
      R"({"old_password":"ResetPassw0rd!","new_password":"ChangedPassw0rd!"})",
      "application/json");
  check(changed && changed->status == 200, "正确原口令应能改密");
  check(changed && !body_field(changed, "csrf").empty(), "改密成功应换发新的 CSRF 令牌");
  check(audit_count(db, "change_password", "user", "口令已修改") == 1,
        "改密成功应写一条审计");
  forced.set_default_headers({{"Cookie", changed ? session_cookie(changed) : ""},
                              {"X-CSRF-Token", body_field(changed, "csrf")}});
  const auto items_after = forced.Get("/api/items");
  check(items_after && items_after->status == 200, "改密后业务接口应恢复正常");

  httplib::Client relogin("127.0.0.1", port);
  const auto new_login = relogin.Post(
      "/api/login", R"({"username":"lock_user","password":"ChangedPassw0rd!"})",
      "application/json");
  check(new_login && new_login->status == 200, "改密后新口令应能登录");
  check(new_login &&
            !nlohmann::json::parse(new_login->body).value("must_change_password", true),
        "改密后登录不应再要求改密");

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

  // 非法查询参数应报 400，而不是让 stoll/stoi 抛成 500；limit 溢出也不能回绕成「不限行数」。
  {
    const auto bad_id = client.Get("/api/reports/stock-ledger?item_id=abc");
    check(bad_id && bad_id->status == 400, "非法 item_id 应返回 400");
    const auto bad_limit = client.Get("/api/reports/stock-ledger?limit=4294967296");
    check(bad_limit && bad_limit->status == 400, "limit 超出 int 范围应返回 400");
  }

  const auto csv = client.Get("/api/reports/stock-balance?format=csv");
  check(csv && csv->status == 200 && csv->get_header_value("Content-Type").rfind("text/csv", 0) == 0,
        "库存余额导出 CSV 应返回 200");
  const auto xlsx = client.Get("/api/items/export?format=xlsx");
  check(xlsx && xlsx->status == 200, "主数据导出 XLSX 应返回 200");
  check(audit_count(db, "export") == 2, "两次导出应各写一条审计");

  // CSV 公式注入：= 开头的名称必须转成文本，否则在表格软件里会被当公式执行。
  {
    const auto danger = client.Post(
        "/api/items", R"({"code":"SMOKE3","name":"=1+1","unit_id":1})", "application/json");
    check(danger && danger->status == 200, "公式样名称应能正常入库");
    const auto danger_csv = client.Get("/api/items/export?format=csv");
    check(danger_csv && danger_csv->status == 200, "主数据导出 CSV 应返回 200");
    if (danger_csv) {
      check(danger_csv->body.find("'=1+1") != std::string::npos,
            "CSV 中公式样文本应加前导单引号");
    }
  }

  // 报表导出流式化：不再有 100000 行截断，导出忽略分页参数并按全量输出。
  {
    xiaotu::DocumentEngine engine(db);
    xiaotu::DocInput smoke_in;
    smoke_in.doc_type = "OTH_IN";
    smoke_in.warehouse_id = 1;
    smoke_in.doc_date = xiaotu::today_text();
    smoke_in.partner = "冒烟期初";
    smoke_in.lines.push_back(xiaotu::DocLineInput{.item_id = item_body.value("id", 0LL),
                                                  .qty = xiaotu::qty_from_int(5),
                                                  .price_cents = 1000});
    engine.save_and_approve(smoke_in);

    const auto ledger_csv = client.Get("/api/reports/stock-ledger?format=csv&limit=1");
    check(ledger_csv && ledger_csv->status == 200 &&
              ledger_csv->get_header_value("Content-Type").rfind("text/csv", 0) == 0,
          "库存流水流式 CSV 应返回 200");
    if (ledger_csv) {
      check(ledger_csv->body.rfind("\xEF\xBB\xBF", 0) == 0, "CSV 应带 UTF-8 BOM");
      check(ledger_csv->body.find("100000") == std::string::npos,
            "流式导出不应再出现 100000 行上限提示");
      check(ledger_csv->body.find("其他入库") != std::string::npos,
            "库存流水 CSV 应包含导出期间的流水行");
      check(ledger_csv->body.find("合计") != std::string::npos, "CSV 应包含合计行");
    }

    const auto ledger_xlsx = client.Get("/api/reports/stock-ledger?format=xlsx");
    check(ledger_xlsx && ledger_xlsx->status == 200 &&
              ledger_xlsx->get_header_value("Content-Type").find("spreadsheetml") !=
                  std::string::npos,
          "库存流水流式 XLSX 应返回 200");
    if (ledger_xlsx) {
      const auto table = xiaotu::read_first_sheet(ledger_xlsx->body);
      check(!table.rows.empty(), "XLSX 应包含表头与数据行");
      bool has_header = false;
      bool has_total = false;
      for (const auto& row : table.rows) {
        if (!row.cells.empty() && row.cells[0] == "时间") has_header = true;
        if (!row.cells.empty() && row.cells[0] == "合计") has_total = true;
      }
      check(has_header && has_total, "XLSX 应包含表头与合计行");
    }

    const auto purchase_csv = client.Get("/api/reports/purchase-detail?format=csv");
    check(purchase_csv && purchase_csv->status == 200 &&
              purchase_csv->body.find("采购明细") != std::string::npos,
          "采购明细导出应走流式且包含标题");
    const auto sales_csv = client.Get("/api/reports/sales-detail?format=csv");
    check(sales_csv && sales_csv->status == 200 &&
              sales_csv->body.find("销售明细") != std::string::npos,
          "销售明细导出应走流式且包含标题");
  }

  // 单据列表：不带新参数仍返回数组；带 page_size / status / 金额 / 日期时返回
  // {rows,total_count,total_amount_cents,page,page_size}，合计不受分页影响。
  {
    xiaotu::DocumentEngine engine(db);
    const std::int64_t item_id = item_body.value("id", 0LL);
    engine.save_and_approve(xiaotu::DocInput{
        .doc_type = "PUR_IN",
        .warehouse_id = 1,
        .doc_date = "2026-01-05",
        .partner = "列表供应商",
        .lines = {xiaotu::DocLineInput{.item_id = item_id, .qty = xiaotu::qty_from_int(2),
                                       .price_cents = 1234}}});
    engine.save_and_approve(xiaotu::DocInput{
        .doc_type = "PUR_IN",
        .warehouse_id = 1,
        .doc_date = "2026-02-10",
        .partner = "列表供应商",
        .lines = {xiaotu::DocLineInput{.item_id = item_id, .qty = xiaotu::qty_from_int(1),
                                       .price_cents = 5000}}});
    engine.save_draft(xiaotu::DocInput{
        .doc_type = "PUR_IN",
        .warehouse_id = 1,
        .doc_date = "2026-02-11",
        .partner = "列表供应商",
        .lines = {xiaotu::DocLineInput{.item_id = item_id, .qty = xiaotu::qty_from_int(1),
                                       .price_cents = 1500}}});

    const auto legacy = client.Get("/api/documents?doc_type=PUR_IN");
    check(legacy && legacy->status == 200, "无筛选参数时应返回 200");
    if (legacy) {
      const auto body = nlohmann::json::parse(legacy->body);
      check(body.is_array() && body.size() == 3,
            "无新参数时应保持数组响应且包含三张采购入库单");
    }

    const auto paged = client.Get("/api/documents?doc_type=PUR_IN&page_size=2");
    check(paged && paged->status == 200, "带 page_size 应返回 200");
    if (paged) {
      const auto body = nlohmann::json::parse(paged->body);
      check(body.is_object() && body.contains("rows"), "带 page_size 应返回对象");
      check(body["rows"].size() == 2, "page_size=2 应只返回两行");
      check(body.value("total_count", 0LL) == 3, "total_count 应为筛选后的总数");
      check(body.value("total_amount_cents", 0LL) == 2468 + 5000 + 1500,
            "total_amount_cents 应为筛选结果逐行之和");
      check(body.value("page", 0) == 1 && body.value("page_size", 0) == 2,
            "应回传当前页码与每页条数");
    }

    const auto second = client.Get("/api/documents?doc_type=PUR_IN&page=2&page_size=2");
    if (second) {
      const auto body = nlohmann::json::parse(second->body);
      check(body["rows"].size() == 1 && body.value("total_count", 0LL) == 3,
            "第二页应只返回一行且 total_count 不变");
    }

    const auto drafts = client.Get("/api/documents?doc_type=PUR_IN&status=draft");
    if (drafts) {
      const auto body = nlohmann::json::parse(drafts->body);
      check(body.value("total_count", 0LL) == 1 && body["rows"].size() == 1 &&
                body["rows"][0].value("status", "") == "DRAFT",
            "status=draft 应只返回草稿");
    }

    const auto combined = client.Get(
        "/api/documents?doc_type=PUR_IN&status=approved&date_from=2026-02-01"
        "&date_to=2026-02-28");
    if (combined) {
      const auto body = nlohmann::json::parse(combined->body);
      check(body.value("total_count", 0LL) == 1 &&
                body.value("total_amount_cents", 0LL) == 5000,
            "日期与状态组合应只命中 2 月已审核单据");
    }

    const auto amount = client.Get(
        "/api/documents?doc_type=PUR_IN&amount_min=20.00&amount_max=50.00");
    if (amount) {
      const auto body = nlohmann::json::parse(amount->body);
      check(body.value("total_count", 0LL) == 2 &&
                body.value("total_amount_cents", 0LL) == 2468 + 5000,
            "金额按元接收并换算成分，闭区间应命中两张单据");
    }

    const auto bad_status = client.Get("/api/documents?doc_type=PUR_IN&status=ghost");
    check(bad_status && bad_status->status == 400, "未知状态筛选应明确拒绝");
  }

  // 整数形态的数量也要受 100 万单位上限约束：否则 qty*price 溢出会写出错误金额。
  {
    nlohmann::json huge{
        {"doc_type", "PUR_IN"},
        {"warehouse_id", 1},
        {"doc_date", "2026-03-05"},
        {"partner", "溢出供应商"},
        {"save_as_draft", true},
        {"lines", nlohmann::json::array({{{"item_id", item_body.value("id", 0LL)},
                                          {"qty", 100000000000000LL},
                                          {"price_cents", 999999999999LL}}})},
    };
    const auto created = client.Post("/api/documents", huge.dump(), "application/json");
    check(created && created->status == 400, "整数数量超出上限应被拒绝");
  }

  // 单据表头备注：创建 / 详情 / 旧数组响应 / 关键字命中 / 导出列 / 超长拒绝。
  {
    nlohmann::json remark_doc{
        {"doc_type", "PUR_IN"},
        {"warehouse_id", 1},
        {"doc_date", "2026-03-05"},
        {"partner", "备注供应商"},
        {"remark", "加急 URGENT-7788"},
        {"save_as_draft", true},
        {"lines", nlohmann::json::array({{{"item_id", item_body.value("id", 0LL)},
                                          {"qty", 1},
                                          {"price_cents", 100}}})},
    };
    const auto remark_created = client.Post("/api/documents", remark_doc.dump(), "application/json");
    check(remark_created && remark_created->status == 200, "带备注的草稿应创建成功");
    const std::int64_t remark_id =
        remark_created ? nlohmann::json::parse(remark_created->body).value("id", 0LL) : 0;
    check(remark_id > 0, "带备注的草稿应返回 id");
    const std::string remark_path = "/api/documents/" + std::to_string(remark_id);

    const auto remark_detail = client.Get(remark_path);
    check(remark_detail && remark_detail->status == 200 &&
              nlohmann::json::parse(remark_detail->body).value("remark", "") == "加急 URGENT-7788",
          "单据详情应带回表头备注");

    const auto remark_legacy = client.Get("/api/documents?doc_type=PUR_IN");
    if (remark_legacy) {
      bool found = false;
      for (const auto& row : nlohmann::json::parse(remark_legacy->body)) {
        if (row.value("id", 0LL) == remark_id) {
          found = row.value("remark", "") == "加急 URGENT-7788";
        }
      }
      check(found, "旧数组响应的列表行也应带表头备注");
    }

    const auto remark_search = client.Get("/api/documents?doc_type=PUR_IN&keyword=URGENT-7788");
    if (remark_search) {
      const auto body = nlohmann::json::parse(remark_search->body);
      check(body.value("total_count", 0LL) == 1 && body["rows"].size() == 1 &&
                body["rows"][0].value("remark", "") == "加急 URGENT-7788",
            "关键字应能命中表头备注");
    }

    const auto remark_csv = client.Get(remark_path + "/export");
    check(remark_csv && remark_csv->status == 200 &&
              remark_csv->body.find("备注,加急 URGENT-7788") != std::string::npos,
          "CSV 导出应带备注列与内容");
    const auto remark_xlsx = client.Get(remark_path + "/export?format=xlsx");
    check(remark_xlsx && remark_xlsx->status == 200 &&
              remark_xlsx->body.rfind("PK", 0) == 0,
          "XLSX 导出应正常生成");

    remark_doc["remark"] = std::string(201, 'x');
    const auto remark_too_long =
        client.Post("/api/documents", remark_doc.dump(), "application/json");
    check(remark_too_long && remark_too_long->status == 400 &&
              nlohmann::json::parse(remark_too_long->body).value("code", "") == "INVALID_INPUT",
          "超过 200 个字符的备注应返回 INVALID_INPUT");
  }

  // 审计来源：HTTP 请求带来源 IP，单据请求号可一起筛出；历史行与 CLI 路径两列为空。
  {
    nlohmann::json doc_body{
        {"doc_type", "OTH_IN"},
        {"warehouse_id", 1},
        {"doc_date", xiaotu::today_text()},
        {"partner", "审计来源测试"},
        {"request_id", "smoke-audit-req"},
        {"lines", nlohmann::json::array({{{"item_id", item_body.value("id", 0LL)},
                                          {"qty", 1},
                                          {"price_cents", 100}}})},
    };
    const auto created = client.Post("/api/documents", doc_body.dump(), "application/json");
    check(created && created->status == 200, "带请求号的单据创建应成功");

    const auto by_request = client.Get("/api/audit-logs?request_id=smoke-audit-req");
    check(by_request && by_request->status == 200, "按请求号筛选审计应返回 200");
    if (by_request) {
      const auto body = nlohmann::json::parse(by_request->body);
      bool matched = body.value("total", 0) >= 1;
      for (const auto& row : body["items"]) {
        matched = matched && row.value("request_id", "") == "smoke-audit-req" &&
                  row.value("client_ip", "") == "127.0.0.1";
      }
      check(matched, "同一请求号的多条审计应能一起筛出，且带出来源 IP");
    }

    const auto by_ip = client.Get("/api/audit-logs?client_ip=127.0.0.1&page_size=200");
    check(by_ip && by_ip->status == 200, "按来源 IP 筛选审计应返回 200");
    if (by_ip) {
      const auto body = nlohmann::json::parse(by_ip->body);
      bool all_local = body.value("total", 0) >= 1;
      for (const auto& row : body["items"]) {
        all_local = all_local && row.value("client_ip", "") == "127.0.0.1";
      }
      check(all_local, "按来源 IP 筛选应只返回该 IP 的审计");
    }

    // CLI 路径：没有请求上下文，两列应为空串而不是报错。
    services.db.write([&](sqlite3* conn) {
      xiaotu::audit_write(conn, "restore", "backup", 0, "", "CLI 恢复测试");
    });
    const auto cli_audit = client.Get("/api/audit-logs?entity=backup");
    check(cli_audit && cli_audit->status == 200, "CLI 审计查询应返回 200");
    if (cli_audit) {
      const auto body = nlohmann::json::parse(cli_audit->body);
      bool found = false;
      for (const auto& row : body["items"]) {
        if (row.value("action", "") != "restore") continue;
        found = row.value("client_ip", "x").empty() && row.value("request_id", "x").empty();
      }
      check(found, "CLI 产生的审计来源 IP 与请求号应为空");
    }

    // 迁移前的旧行（两列走默认值）读取应正常，值为空串。
    services.db.write([&](sqlite3* conn) {
      sqlite3_exec(conn,
                   "INSERT INTO audit_log (at, actor, action, entity, entity_id, entity_no,"
                   " detail) VALUES ('2026-01-01 00:00:00', 'system', 'legacy', 'item', 0, '',"
                   " '历史行');",
                   nullptr, nullptr, nullptr);
    });
    const auto legacy = client.Get("/api/audit-logs?entity=item&page_size=200");
    if (legacy) {
      const auto body = nlohmann::json::parse(legacy->body);
      bool legacy_ok = false;
      for (const auto& row : body["items"]) {
        if (row.value("action", "") != "legacy") continue;
        legacy_ok = row.value("client_ip", "x").empty() && row.value("request_id", "x").empty();
      }
      check(legacy && legacy->status == 200 && legacy_ok, "旧审计行两列为空且读取不报错");
    }

    const auto audit_csv =
        client.Get("/api/audit-logs/export?format=csv&request_id=smoke-audit-req");
    check(audit_csv && audit_csv->status == 200 &&
              audit_csv->body.find("来源 IP") != std::string::npos &&
              audit_csv->body.find("smoke-audit-req") != std::string::npos,
          "审计 CSV 导出应带来源 IP 与请求号");
    const auto audit_xlsx =
        client.Get("/api/audit-logs/export?format=xlsx&request_id=smoke-audit-req");
    check(audit_xlsx && audit_xlsx->status == 200, "审计 XLSX 导出应返回 200");
  }

  // 规模额度（社区版）：额度内不受影响，到达上限后新建被拒并说明上限，读不受影响。
  if (community) {
    const auto limit_of = [&](const char* key) {
      return health_body["capabilities"]["quotas"].value(key, std::int64_t{-1});
    };
    const auto count_of = [&](const std::string& sql) {
      std::int64_t value = 0;
      db.read([&](sqlite3* conn) {
        xiaotu::Stmt stmt(conn, sql);
        if (stmt.step()) value = stmt.i64(0);
      });
      return value;
    };
    const auto fill_to = [&](const std::string& sql, const char* what) {
      db.write([&](sqlite3* conn) {
        check(sqlite3_exec(conn, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK,
              std::string("补足") + what + "额度数据应成功");
      });
    };
    const auto expect_quota = [&](const auto& result, const char* label) {
      check(result && result->status == 409 &&
                result->body.find("QUOTA_EXCEEDED") != std::string::npos &&
                result->body.find(label) != std::string::npos,
            std::string("超过") + label + "上限应被拒绝并提示上限");
    };

    // 商品
    {
      const auto limit = limit_of("max_items");
      const auto current = count_of("SELECT COUNT(*) FROM item WHERE is_deleted = 0 AND status = 1;");
      if (current < limit) {
        const auto ok = client.Post("/api/items",
                                    R"({"code":"QOK-ITEM","name":"额度内商品","unit_id":1})",
                                    "application/json");
        check(ok && ok->status == 200, "商品未到上限时应能新建");
      }
      const auto now = count_of("SELECT COUNT(*) FROM item WHERE is_deleted = 0 AND status = 1;");
      if (now < limit) {
        fill_to("WITH RECURSIVE seq(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM seq WHERE n < " +
                    std::to_string(limit - now) + ") INSERT INTO item (id, code, name, created_at,"
                    " updated_at) SELECT 810000000 + n, 'QITEM' || n, '额度商品' || n,"
                    " '2026-01-01 00:00:00', '2026-01-01 00:00:00' FROM seq;",
                "商品");
      }
      const auto rejected = client.Post("/api/items",
                                        R"({"code":"QOVER-ITEM","name":"超限商品","unit_id":1})",
                                        "application/json");
      expect_quota(rejected, "商品数");
      check(client.Get("/api/items") && client.Get("/api/items")->status == 200,
            "达到上限后商品列表仍应可读");
    }

    // 仓库
    {
      const auto limit = limit_of("max_warehouses");
      const auto current =
          count_of("SELECT COUNT(*) FROM warehouse WHERE is_deleted = 0 AND status = 1;");
      if (current < limit) {
        const auto ok = client.Post("/api/warehouses",
                                    R"({"code":"QOK-WH","name":"额度内仓库"})",
                                    "application/json");
        check(ok && ok->status == 200, "仓库未到上限时应能新建");
      }
      const auto now =
          count_of("SELECT COUNT(*) FROM warehouse WHERE is_deleted = 0 AND status = 1;");
      if (now < limit) {
        fill_to("WITH RECURSIVE seq(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM seq WHERE n < " +
                    std::to_string(limit - now) + ") INSERT INTO warehouse (id, code, name,"
                    " created_at, updated_at) SELECT 811000000 + n, 'QWH' || n, '额度仓库' || n,"
                    " '2026-01-01 00:00:00', '2026-01-01 00:00:00' FROM seq;",
                "仓库");
      }
      const auto rejected = client.Post("/api/warehouses",
                                        R"({"code":"QOVER-WH","name":"超限仓库"})",
                                        "application/json");
      expect_quota(rejected, "仓库数");
    }

    // 往来单位（客户 + 供应商合计）
    {
      const char* partner_count =
          "SELECT (SELECT COUNT(*) FROM customer WHERE is_deleted = 0 AND status = 1)"
          " + (SELECT COUNT(*) FROM supplier WHERE is_deleted = 0 AND status = 1);";
      const auto limit = limit_of("max_partners");
      const auto current = count_of(partner_count);
      if (current < limit) {
        const auto ok = client.Post("/api/customers",
                                    R"({"code":"QOK-CUS","name":"额度内客户"})",
                                    "application/json");
        check(ok && ok->status == 200, "往来单位未到上限时应能新建");
      }
      const auto now = count_of(partner_count);
      if (now < limit) {
        fill_to("WITH RECURSIVE seq(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM seq WHERE n < " +
                    std::to_string(limit - now) + ") INSERT INTO customer (id, code, name,"
                    " created_at, updated_at) SELECT 812000000 + n, 'QCUS' || n, '额度客户' || n,"
                    " '2026-01-01 00:00:00', '2026-01-01 00:00:00' FROM seq;",
                "往来单位");
      }
      const auto rejected = client.Post("/api/suppliers",
                                        R"({"code":"QOVER-SUP","name":"超限供应商"})",
                                        "application/json");
      expect_quota(rejected, "往来单位数");
    }

    // 打印模板
    {
      const auto limit = limit_of("max_print_templates");
      const auto current = count_of("SELECT COUNT(*) FROM print_template WHERE is_deleted = 0;");
      if (current < limit) {
        const auto ok = client.Post(
            "/api/print-templates",
            R"({"code":"QOK-TPL","name":"额度内模板","target_type":"DOC","target_key":"PUR_IN",)"
            R"("columns":[{"column_key":"line_no"}]})",
            "application/json");
        check(ok && ok->status == 200, "打印模板未到上限时应能新建");
      }
      const auto rejected = client.Post(
          "/api/print-templates",
          R"({"code":"QOVER-TPL","name":"超限模板","target_type":"DOC","target_key":"PUR_IN",)"
          R"("columns":[{"column_key":"line_no"}]})",
          "application/json");
      expect_quota(rejected, "打印模板数");
    }

    // 单据
    {
      const auto limit = limit_of("max_documents");
      const auto current = count_of("SELECT COUNT(*) FROM doc_header;");
      if (current < limit) {
        fill_to("WITH RECURSIVE seq(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM seq WHERE n < " +
                    std::to_string(limit - current) + ") INSERT INTO doc_header (id, doc_no,"
                    " doc_type, doc_date, warehouse_id, status, created_at) SELECT 813000000 + n,"
                    " 'QDOC' || n, 'OTH_IN', '2026-01-01', 1, 'DRAFT', '2026-01-01 00:00:00'"
                    " FROM seq;",
                "单据");
      }
      nlohmann::json over_doc{
          {"doc_type", "OTH_IN"},
          {"warehouse_id", 1},
          {"doc_date", xiaotu::today_text()},
          {"partner", "额度超限"},
          {"request_id", "quota-doc"},
          {"lines", nlohmann::json::array({{{"item_id", item_body.value("id", 0LL)},
                                            {"qty", 1},
                                            {"price_cents", 100}}})},
      };
      expect_quota(client.Post("/api/documents", over_doc.dump(), "application/json"), "单据数");
      check(client.Get("/api/documents") && client.Get("/api/documents")->status == 200,
            "达到上限后单据列表仍应可读");
    }
  }

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

  // 参数生效：改完 session_idle_minutes 后，新签发的会话有效期应按 45 分钟。
  const auto relogin_after_timeout = client.Post(
      "/api/login", R"({"username":"admin","password":"Passw0rd!"})", "application/json");
  check(relogin_after_timeout && relogin_after_timeout->status == 200,
        "修改会话超时后应能重新登录");
  if (relogin_after_timeout) {
    std::int64_t idle_minutes = -1;
    db.read([&](sqlite3* conn) {
      xiaotu::Stmt stmt(
          conn,
          "SELECT CAST((julianday(expires_at) - julianday('now', 'localtime')) * 1440 AS INTEGER)"
          " FROM app_session WHERE expires_at = (SELECT MAX(expires_at) FROM app_session);");
      if (stmt.step()) idle_minutes = stmt.i64(0);
    });
    check(idle_minutes >= 43 && idle_minutes <= 46,
          "新会话空闲有效期应按设置签发（约 45 分钟）");
    client.set_default_headers({{"Cookie", session_cookie(relogin_after_timeout)},
                                {"X-CSRF-Token", body_field(relogin_after_timeout, "csrf")}});
  }

  // 公司抬头已有保存/读回用例；这里补超长校验，失败请求不应改动现有值。
  const std::string too_long(101, 'x');
  const auto company_rejected = client.Put(
      "/api/settings",
      nlohmann::json{{"company", nlohmann::json{{"name", too_long}}}}.dump(),
      "application/json");
  check(company_rejected && company_rejected->status == 400, "公司名称超长应被拒绝");
  const auto company_after_reject = client.Get("/api/settings");
  check(company_after_reject && company_after_reject->status == 200 &&
            nlohmann::json::parse(company_after_reject->body)["company"].value("name", "") ==
                "冒烟公司",
        "超长请求失败后公司抬头应保持不变");

  // 成本口径：默认含税；切换后记录生效时间，库存流水报表能读出切换日。
  const auto basis_default = client.Get("/api/settings");
  check(basis_default && basis_default->status == 200 &&
            nlohmann::json::parse(basis_default->body).value("cost_basis", "") == "INCLUSIVE",
        "成本口径默认应为含税价");
  check(basis_default &&
            nlohmann::json::parse(basis_default->body).value("cost_basis_changed_at", "x").empty(),
        "未切换过口径时切换日应为空");
  const auto basis_switched =
      client.Put("/api/settings", R"({"cost_basis":"NET"})", "application/json");
  check(basis_switched && basis_switched->status == 200, "切换成本口径应返回 200");
  const std::string basis_changed_at =
      basis_switched ? nlohmann::json::parse(basis_switched->body)
                           .value("cost_basis_changed_at", std::string{})
                     : std::string{};
  check(!basis_changed_at.empty(), "切换成本口径应写入生效时间并返回");
  const auto ledger_report = client.Get("/api/reports/stock-ledger");
  check(ledger_report && ledger_report->status == 200, "库存流水报表应返回 200");
  if (ledger_report) {
    const auto data = nlohmann::json::parse(ledger_report->body);
    check(data.value("cost_basis", "") == "NET", "库存流水报表应标注当前成本口径");
    check(data.value("cost_basis_changed_at", "") == basis_changed_at,
          "库存流水报表应能读出切换日");
  }
  const auto profit_report = client.Get("/api/reports/sales-profit");
  if (community) {
    check(profit_report && profit_report->status == 403 &&
              profit_report->body.find("FEATURE_UNAVAILABLE") != std::string::npos,
          "社区版销售毛利接口应明确拒绝");
  } else {
    check(profit_report && profit_report->status == 200, "销售毛利报表应返回 200");
  }
  const auto dashboard_report = client.Get("/api/reports/dashboard");
  if (community) {
    check(dashboard_report && dashboard_report->status == 403 &&
              dashboard_report->body.find("FEATURE_UNAVAILABLE") != std::string::npos,
          "社区版经营看板接口应明确拒绝");
  } else {
    check(dashboard_report && dashboard_report->status == 200, "经营看板应返回 200");
  }
  const auto basis_restored =
      client.Put("/api/settings", R"({"cost_basis":"INCLUSIVE"})", "application/json");
  check(basis_restored && basis_restored->status == 200 &&
            nlohmann::json::parse(basis_restored->body).value("cost_basis", "") == "INCLUSIVE",
        "成本口径应能切回含税价");

  // 模板抬头为空时，打印用公司名称兜底。
  const auto printed = client.Get("/api/print/render?target_type=REPORT&target_key=sales-profit");
  if (community) {
    check(printed && printed->status == 403 &&
              printed->body.find("FEATURE_UNAVAILABLE") != std::string::npos,
          "社区版销售毛利打印应明确拒绝");
  } else {
    check(printed && printed->status == 200, "打印渲染应返回 200");
  }
  if (!community && printed) {
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
    check(data.value("schema_version", 0) == 40, "诊断信息里的 schema 应为 40");
    check(data.value("migrations", 0) == 40, "诊断信息里的迁移条数应为 40");
    check(data.value("mode", "") == "server", "网络版诊断信息 mode 应为 server");
    check(!data.value("data_dir", "").empty(), "诊断信息应包含数据目录");
    check(data.value("backup_dir", "") == (dir / "backups").string(),
          "诊断信息应包含解析后的备份目录（默认 <数据目录>/backups）");
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

  // 本机口令端点只接受本机页面：跨站 Origin 必须拒绝，否则可被关掉口令门禁。
  {
    httplib::Client cross_site("127.0.0.1", port);
    cross_site.set_default_headers({{"Origin", "https://evil.example"}});
    const auto blocked = cross_site.Post("/api/local/password/skip", "", "text/plain");
    check(blocked && blocked->status == 403, "跨站 Origin 不应能跳过本机口令");
  }
  const auto skip = client.Post("/api/local/password/skip", "", "text/plain");
  check(skip && skip->status == 200, "本机客户端应能跳过口令设置");

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
