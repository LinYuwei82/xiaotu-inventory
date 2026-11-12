#pragma once

#include <string>

namespace xiaotu {

// 一次请求的上下文。由 HTTP 中间件在每个请求进入时填充，
// 供审计字段、权限校验等横切逻辑读取，业务代码无需层层传参。
struct RequestContext {
  std::string user = "system";
  std::string role = "admin";
  std::string request_id;
  std::string csrf;
};

RequestContext& current_context();

std::string current_user();

}  // namespace xiaotu
