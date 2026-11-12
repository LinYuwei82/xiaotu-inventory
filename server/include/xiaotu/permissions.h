#pragma once

#include <string>
#include <string_view>

namespace xiaotu {

// 权限判定集中在这里：路由只声明权限码，校验由中间件统一执行。
// M0 只有内置角色；接入用户/角色表后把实现替换为查库即可，调用方不变。
bool role_has_permission(std::string_view role, std::string_view permission);

}  // namespace xiaotu
