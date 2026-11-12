#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "xiaotu/db.h"

namespace xiaotu {

struct UserRow {
  std::int64_t id = 0;
  std::string username;
  std::string display_name;
  std::string role;  // 角色码：admin / operator / viewer
  bool active = true;
  std::string created_at;
};

struct UserInput {
  std::string username;
  std::string display_name;
  std::string role = "viewer";
  bool active = true;
};

// 用户表的最小 CRUD。口令列 password_hash 暂留空，登录在 M5 接入。
class UserService {
 public:
  explicit UserService(Db& db) : db_(db) {}

  std::vector<UserRow> list() const;
  UserRow get(std::int64_t id) const;
  UserRow find_by_username(std::string_view username) const;

  UserRow create(const UserInput& input);
  UserRow update(std::int64_t id, const UserInput& input);
  void remove(std::int64_t id);

  std::int64_t count_active() const;

 private:
  Db& db_;
};

}  // namespace xiaotu
