#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <string>
#include <thread>

#include "xiaotu/audit.h"
#include "xiaotu/context.h"
#include "xiaotu/http_json.h"
#include "xiaotu/http_server.h"
#include "xiaotu/http_util.h"
#include "xiaotu/log.h"
#include "xiaotu/permissions.h"
#include "xiaotu/services.h"

namespace xiaotu {

void HttpServer::register_system_routes() {
  // 优雅关闭：只允许回环地址；管理员会话或一次性令牌二选一。
  // 先写审计与响应，再在独立线程里 stop：在 handler 线程内 stop 会等待自身
  // 所在的连接/任务队列，造成死锁；在途请求由 httplib 的任务队列排空后自然完成。
  server_->Post("/api/system/shutdown", [this](const httplib::Request& req,
                                               httplib::Response& res) {
    if (!shutdown_from_local_origin(req)) {
      json_error(res, 403, "FORBIDDEN", "仅允许本机访问");
      return;
    }
    const std::string& role = current_context().role;
    bool authorized = false;
    const char* source = "";
    if (!role.empty() && role == "admin" && role_has_permission(role, "setting.write")) {
      authorized = true;
      source = "session";
    } else if (shutdown_token_.consume(req.get_header_value("X-Xiaotu-Shutdown-Token"))) {
      authorized = true;
      source = "token";
    }
    if (!authorized) {
      if (role.empty()) {
        json_error(res, 401, "UNAUTHORIZED", "未登录或会话已过期");
      } else {
        json_error(res, 403, "FORBIDDEN", "仅管理员可关闭服务");
      }
      return;
    }
    // 审计写失败不应阻止关闭。
    try {
      std::string detail = std::string("来源=") + source + " 客户端=" + req.remote_addr;
      services_.db.write([&](Conn& conn) {
        audit_write(conn, "shutdown", "system", 0, "", detail);
      });
    } catch (const std::exception& error) {
      log_warn(std::string("关闭审计写入失败: ") + error.what());
    }
    // 重复调用保持幂等：只触发一次 stop。
    if (!shutdown_started_.exchange(true)) {
      shutdown_thread_ = std::thread([this] {
        // 给响应写出留出时间，避免关闭监听套接字时打断正在回写的响应。
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        server_->stop();
      });
    }
    json_ok(res, nlohmann::json{{"status", "stopping"}});
  });
}

}  // namespace xiaotu
