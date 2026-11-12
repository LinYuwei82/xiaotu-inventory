#include "xiaotu/permissions.h"

#include <sqlite3.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "xiaotu/db.h"

namespace xiaotu {
namespace {

// 启动时写入一次，之后进程内只读；不做运行期热更新，因此无需加锁。
std::unordered_map<std::string, std::unordered_set<std::string>>& mutable_table() {
  static std::unordered_map<std::string, std::unordered_set<std::string>> table;
  return table;
}

}  // namespace

void load_role_permissions(Db& db) {
  std::unordered_map<std::string, std::unordered_set<std::string>> loaded;
  db.read([&](sqlite3* conn) {
    Stmt stmt(conn,
              "SELECT r.code, p.permission FROM app_role r"
              " JOIN app_role_permission p ON p.role_id = r.id;");
    while (stmt.step()) {
      loaded[stmt.text(0)].insert(stmt.text(1));
    }
  });
  mutable_table() = std::move(loaded);
}

bool role_has_permission(std::string_view role, std::string_view permission) {
  const auto& table = mutable_table();
  const auto it = table.find(std::string(role));
  if (it == table.end()) {
    return false;
  }
  return it->second.count("*") > 0 || it->second.count(std::string(permission)) > 0;
}

std::vector<std::string> role_permissions(std::string_view role) {
  std::vector<std::string> result;
  const auto& table = mutable_table();
  const auto it = table.find(std::string(role));
  if (it == table.end()) {
    return result;
  }
  result.assign(it->second.begin(), it->second.end());
  std::sort(result.begin(), result.end());
  return result;
}

bool role_exists(std::string_view role) {
  return mutable_table().count(std::string(role)) > 0;
}

}  // namespace xiaotu
