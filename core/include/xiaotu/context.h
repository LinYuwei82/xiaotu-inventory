#pragma once

#include <cstdint>
#include <string>

namespace xiaotu {

// 一次请求的上下文。由 HTTP 中间件在每个请求进入时填充，
// 供审计字段、权限校验等横切逻辑读取，业务代码无需层层传参。
struct RequestContext {
  std::string user = "system";
  std::string role = "admin";
  std::string request_id;
  std::string csrf;
  std::int64_t user_id = 0;
  // 管理员重置口令后为 true：除改密与退出外的接口一律拒绝。
  bool must_change_password = false;
  // 审计来源：HTTP 请求的客户端地址；CLI / 无请求上下文时为空。
  std::string client_ip;
  // 审计请求号：客户端显式提供时才写入（如单据的幂等键），没有就留空。
  std::string audit_request_id;
};

RequestContext& current_context();

std::string current_user();

}  // namespace xiaotu
