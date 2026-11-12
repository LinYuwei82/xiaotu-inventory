#include "xiaotu/http_guard.h"

#include <utility>

#include "xiaotu/context.h"
#include "xiaotu/http_json.h"
#include "xiaotu/permissions.h"

namespace xiaotu {

httplib::Server::Handler guard(std::string permission, httplib::Server::Handler handler) {
  return [permission = std::move(permission), handler = std::move(handler)](
             const httplib::Request& req, httplib::Response& res) {
    if (!role_has_permission(current_context().role, permission)) {
      json_error(res, 403, "FORBIDDEN", "没有权限执行该操作: " + permission);
      return;
    }
    handler(req, res);
  };
}

}  // namespace xiaotu
