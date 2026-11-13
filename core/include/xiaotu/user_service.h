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
  bool must_change_password = false;
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
  // 连续失败达到阈值后账号锁定 15 分钟，抛 ACCOUNT_LOCKED；锁定期间的尝试
  // 即使口令正确也拒绝。失败、锁定与成功都在同一写事务里更新计数并写审计。
  UserRow authenticate(std::string_view username, std::string_view password,
                       std::string_view source = {});
  bool has_password(std::int64_t id) const;
  // 设置口令：清空失败计数与锁定。默认只有首次设置（原口令为空）才置
  // must_change_password=1；force_change_on_first_set=false 用于单机版本机
  // 口令，门禁页本身就是口令入口，不再叠加改密流程。
  void set_password(std::int64_t id, std::string_view password,
                    bool force_change_on_first_set = true);
  // 用户自助改密：校验原口令，成功后清空失败计数、锁定与强制改密标记。
  void change_password(std::int64_t id, std::string_view old_password,
                       std::string_view new_password);

  std::int64_t count_active() const;

 private:
  Db& db_;
};

}  // namespace xiaotu
