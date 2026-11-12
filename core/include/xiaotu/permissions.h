#pragma once

#include <string>
#include <string_view>

namespace xiaotu {

class Db;

// 权限判定集中在这里：路由只声明权限码，校验由中间件统一执行。
// 角色与权限点在启动时从 app_role_permission 整体载入内存，运行期只读。
// 未载入或未登记的角色一律拒绝，避免默认放行。
void load_role_permissions(Db& db);

bool role_has_permission(std::string_view role, std::string_view permission);

bool role_exists(std::string_view role);

}  // namespace xiaotu
