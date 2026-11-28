#pragma once

#include <httplib.h>

#include <atomic>
#include <string>

namespace xiaotu {

class Services;
class SessionStore;

// 认证中间件依赖的最小上下文：只引用 HttpServer 的成员，不持有生命周期。
struct AuthMiddlewareContext {
  Services& services;
  SessionStore& sessions;
  const std::string& mode;  // "local" / "server"
  const int& port;          // bind() 之后才确定，必须按引用读取
  std::atomic<bool>& maintenance;
};

// 安装 pre-routing 认证与安全校验：会话续期、本机来源、API 版本协商、
// 维护态拦截、请求体上限、登录白名单、CSRF 与强制改密。
void install_auth_middleware(httplib::Server& server, AuthMiddlewareContext context);

}  // namespace xiaotu
