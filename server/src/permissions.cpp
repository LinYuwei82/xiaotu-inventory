#include "xiaotu/permissions.h"

#include <string>
#include <unordered_map>
#include <unordered_set>

namespace xiaotu {
namespace {

const std::unordered_map<std::string, std::unordered_set<std::string>>& role_table() {
  static const std::unordered_map<std::string, std::unordered_set<std::string>> table = {
      {"admin", {"*"}},
      {"operator",
       {"item.read", "item.write", "warehouse.read", "warehouse.write", "document.read",
        "document.write", "stock.read"}},
      {"viewer", {"item.read", "warehouse.read", "document.read", "stock.read"}},
  };
  return table;
}

}  // namespace

bool role_has_permission(std::string_view role, std::string_view permission) {
  const auto it = role_table().find(std::string(role));
  if (it == role_table().end()) {
    return false;
  }
  return it->second.count("*") > 0 || it->second.count(std::string(permission)) > 0;
}

}  // namespace xiaotu
