#pragma once

#include <httplib.h>

#include <string>

namespace xiaotu {

// 路由声明式权限：guard("item.read", handler)。
// 未声明的权限码一律拒绝，避免「漏写校验」变成默认放行。
httplib::Server::Handler guard(std::string permission, httplib::Server::Handler handler);

}  // namespace xiaotu
