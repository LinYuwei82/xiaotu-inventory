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
  std::string updated_at;
};

struct UserInput {
  std::string username;
  std::string display_name;
  std::string role = "viewer";
  bool active = true;
  std::string password;  // 新建必填；更新时留空表示不改口令。
};

// 用户表与口令的最小实现。口令以 Argon2id 散列存储，登录在 M5 接入。
class UserService {
 public:
  explicit UserService(Db& db) : db_(db) {}

  std::vector<UserRow> list() const;
  UserRow get(std::int64_t id) const;
  UserRow find_by_username(std::string_view username) const;

  UserRow create(const UserInput& input);
  UserRow update(std::int64_t id, const UserInput& input);
  void remove(std::int64_t id);

  // 用户名或口令错误时统一抛 INVALID_CREDENTIALS，不区分账号是否存在。
  UserRow authenticate(std::string_view username, std::string_view password) const;
  bool has_password(std::int64_t id) const;
  void set_password(std::int64_t id, std::string_view password);

  std::int64_t count_active() const;

 private:
  Db& db_;
};

}  // namespace xiaotu
