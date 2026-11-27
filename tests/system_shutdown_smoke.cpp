// 优雅关闭：回环判断单测、一次性令牌（写成 0600、使用后删除且不可重复使用）、
// HTTP 鉴权语义（无凭据 401、错误令牌 401、非管理员会话 403、正确令牌/管理员 200）
// 以及服务在收到关闭请求后真的停止 listen。
#include <httplib.h>
#include "xiaotu/db.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#include <nlohmann/json.hpp>

#include "xiaotu/http_server.h"
#include "xiaotu/migrations.h"
#include "xiaotu/permissions.h"
#include "xiaotu/services.h"
#include "xiaotu/system_shutdown.h"

namespace {

int g_failures = 0;

void check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << std::endl;
    ++g_failures;
  }
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

unsigned int file_mode(const std::filesystem::path& path) {
#if defined(_WIN32)
  return 0;
#else
  struct stat info {};
  if (::stat(path.string().c_str(), &info) != 0) {
    return 0;
  }
  return static_cast<unsigned int>(info.st_mode) & 0777U;
#endif
}

void clear_must_change(xiaotu::Db& db, std::int64_t id) {
  db.write([&](xiaotu::Conn& conn) {
    xiaotu::exec_sql(conn, "UPDATE app_user SET must_change_password = 0 WHERE id = " +
                               std::to_string(id) + ";");
  });
}

std::string session_cookie(const httplib::Result& result) {
  const std::string header = result->get_header_value("Set-Cookie");
  const std::size_t end = header.find(';');
  return header.substr(0, end == std::string::npos ? header.size() : end);
}

std::string body_field(const httplib::Result& result, const char* key) {
  return nlohmann::json::parse(result->body).value(key, std::string{});
}

std::string last_shutdown_detail(xiaotu::Db& db) {
  std::string detail;
  db.read([&](xiaotu::Conn& conn) {
    xiaotu::Stmt stmt(conn,
                      "SELECT detail FROM audit_log WHERE action = 'shutdown'"
                      " ORDER BY id DESC LIMIT 1;");
    if (stmt.step()) {
      detail = stmt.text(0);
    }
  });
  return detail;
}

// 关闭在独立线程触发，主线程轮询等待 listen 结束；这里给足 5 秒。
bool wait_until_stopped(xiaotu::HttpServer& http) {
  for (int attempt = 0; attempt < 100; ++attempt) {
    if (!http.serving()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

void prepare_database(const std::filesystem::path& dir) {
  std::filesystem::create_directories(dir);
}

void run_loopback_checks() {
  check(xiaotu::is_loopback_address("127.0.0.1"), "127.0.0.1 应判定为回环");
  check(xiaotu::is_loopback_address("::1"), "::1 应判定为回环");
  check(xiaotu::is_loopback_address("::ffff:127.0.0.1"), "::ffff:127.0.0.1 应判定为回环");
  check(!xiaotu::is_loopback_address("192.168.1.10"), "192.168.1.10 不应判定为回环");
  check(!xiaotu::is_loopback_address("10.0.0.1"), "10.0.0.1 不应判定为回环");
  check(!xiaotu::is_loopback_address(""), "空地址不应判定为回环");
  check(!xiaotu::is_loopback_address("localhost"), "域名不应判定为回环");
  check(!xiaotu::is_loopback_address("127.0.0.2"), "127.0.0.2 不在允许列表内");
}

void run_token_checks(const std::filesystem::path& dir) {
  prepare_database(dir);
  const std::filesystem::path token_file = dir / ".shutdown-token";

  xiaotu::ShutdownToken token;
  check(token.issue(token_file), "关闭令牌应能写入文件");
  check(std::filesystem::exists(token_file), "关闭令牌文件应存在");
  const std::string secret = read_file(token_file);
  check(secret.size() == 64, "关闭令牌应为 32 字节的 hex（64 字符）");
  check(secret.find_first_not_of("0123456789abcdef") == std::string::npos,
        "关闭令牌应只包含小写 hex");
#if !defined(_WIN32)
  check(file_mode(token_file) == 0600U, "关闭令牌文件权限应为 0600");
#endif
  check(!token.consume("wrong-token"), "错误令牌不应通过校验");
  check(std::filesystem::exists(token_file), "错误令牌不应删除令牌文件");
  check(token.consume(secret), "正确令牌应通过校验");
  check(!std::filesystem::exists(token_file), "令牌使用后文件应被删除");
  check(!token.consume(secret), "同一令牌不能重复使用");
  check(!token.issued(), "令牌使用后应失效");
}

// HTTP：无凭据 401 → 错误令牌 401 → 正确令牌 200 且服务停止、令牌文件删除、审计落库。
void run_http_token_mode(const std::filesystem::path& dir) {
  prepare_database(dir);
  xiaotu::Db db(dir / "app.db", 2);
  xiaotu::migrate(db);
  xiaotu::load_role_permissions(db);
  xiaotu::Services services(db);

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

  const std::filesystem::path token_file = dir / ".shutdown-token";
  check(std::filesystem::exists(token_file), "启动后应生成关闭令牌文件");
  const std::string secret = read_file(token_file);

  httplib::Client client("127.0.0.1", port);
  const auto anonymous = client.Post("/api/system/shutdown", "", "application/json");
  check(anonymous && anonymous->status == 401, "无令牌、无会话应返回 401");
  check(std::filesystem::exists(token_file), "未授权请求不应删除令牌文件");

  httplib::Client wrong("127.0.0.1", port);
  wrong.set_default_headers({{"X-Xiaotu-Shutdown-Token", "not-the-token"}});
  const auto wrong_result = wrong.Post("/api/system/shutdown", "", "application/json");
  check(wrong_result && wrong_result->status == 401, "错误令牌应返回 401");
  check(std::filesystem::exists(token_file), "错误令牌不应删除令牌文件");
  check(http.serving(), "未授权的请求不应停止服务");

  httplib::Client authorized("127.0.0.1", port);
  authorized.set_default_headers({{"X-Xiaotu-Shutdown-Token", secret}});
  const auto accepted = authorized.Post("/api/system/shutdown", "", "application/json");
  check(accepted && accepted->status == 200, "正确令牌应返回 200");
  check(accepted && accepted->body.find("stopping") != std::string::npos,
        "关闭响应应包含 stopping");
  check(!std::filesystem::exists(token_file), "令牌使用后文件应被删除");
  check(wait_until_stopped(http), "收到关闭请求后服务应在合理时间内停止 listen");

  const std::string detail = last_shutdown_detail(db);
  check(detail.find("来源=token") != std::string::npos, "审计 detail 应标明来源 token");
  check(detail.find("127.0.0.1") != std::string::npos, "审计 detail 应包含客户端 IP");

  http.stop();
}

// HTTP：会话路径。只读角色 403；管理员缺 CSRF 403；管理员带 CSRF 200 并停止。
void run_http_session_mode(const std::filesystem::path& dir) {
  prepare_database(dir);
  xiaotu::Db db(dir / "app.db", 2);
  xiaotu::migrate(db);
  xiaotu::load_role_permissions(db);
  xiaotu::Services services(db);
  services.users.set_password(1, "Passw0rd!");
  clear_must_change(db, 1);
  services.users.create(xiaotu::UserInput{.username = "shutdown_viewer",
                                          .display_name = "关闭只读",
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

  {
    httplib::Client viewer("127.0.0.1", port);
    const auto login = viewer.Post("/api/login",
                                   R"({"username":"shutdown_viewer","password":"Viewer0rd!"})",
                                   "application/json");
    check(login && login->status == 200, "只读账号应能登录");
    if (login) {
      viewer.set_default_headers({{"Cookie", session_cookie(login)},
                                  {"X-CSRF-Token", body_field(login, "csrf")}});
      const auto rejected = viewer.Post("/api/system/shutdown", "", "application/json");
      check(rejected && rejected->status == 403, "非管理员会话应返回 403");
      check(http.serving(), "非管理员请求不应停止服务");
    }
  }

  httplib::Client admin("127.0.0.1", port);
  const auto login =
      admin.Post("/api/login", R"({"username":"admin","password":"Passw0rd!"})",
                 "application/json");
  check(login && login->status == 200, "管理员应能登录");
  const std::string cookie = login ? session_cookie(login) : std::string{};
  const std::string csrf = login ? body_field(login, "csrf") : std::string{};
  if (login) {
    admin.set_default_headers({{"Cookie", cookie}});
    const auto no_csrf = admin.Post("/api/system/shutdown", "", "application/json");
    check(no_csrf && no_csrf->status == 403 &&
              no_csrf->body.find("CSRF_INVALID") != std::string::npos,
          "管理员会话缺少 CSRF 应返回 403 CSRF_INVALID");
    check(http.serving(), "CSRF 校验失败不应停止服务");

    admin.set_default_headers({{"Cookie", cookie}, {"X-CSRF-Token", csrf}});
    const auto accepted = admin.Post("/api/system/shutdown", "", "application/json");
    check(accepted && accepted->status == 200, "管理员会话应能关闭服务");
    check(accepted && accepted->body.find("stopping") != std::string::npos,
          "关闭响应应包含 stopping");
    check(wait_until_stopped(http), "管理员关闭后服务应在合理时间内停止 listen");

    const std::string detail = last_shutdown_detail(db);
    check(detail.find("来源=session") != std::string::npos,
          "审计 detail 应标明来源 session");
    check(detail.find("127.0.0.1") != std::string::npos, "审计 detail 应包含客户端 IP");
  }

  http.stop();
}

}  // namespace

int main() {
  namespace fs = std::filesystem;
  const fs::path root = fs::temp_directory_path() / "xiaotu-shutdown-smoke";
  try {
    fs::remove_all(root);
    run_loopback_checks();
    run_token_checks(root / "token");
    run_http_token_mode(root / "http-token");
    run_http_session_mode(root / "http-session");
    fs::remove_all(root);
  } catch (const std::exception& error) {
    std::cerr << "异常: " << error.what() << std::endl;
    ++g_failures;
  }

  if (g_failures != 0) {
    std::cerr << g_failures << " 项失败" << std::endl;
    return 1;
  }
  std::cout << "system_shutdown_smoke 通过" << std::endl;
  return 0;
}
