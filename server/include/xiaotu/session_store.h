#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "xiaotu/db.h"

namespace xiaotu {

struct Session {
  std::int64_t user_id = 0;
  std::string user;
  std::string role;
  std::string csrf;
  bool must_change_password = false;
};

struct CreatedSession {
  std::string id;
  std::string csrf;
};

// 会话落库：进程重启后登录态仍然有效，令牌只以散列形式存储。
// 有效期由系统参数决定：空闲窗口滑动续期（默认 30 分钟），
// 绝对上限从签发时刻起算（默认 12 小时），两者任一到期即拒绝。
// 同一会话 60 秒内最多写一次库，避免每个请求都去抢写连接。
// 登出删除单条记录，改密、停用、删除用户由 UserService 在同一事务里清空该用户的全部会话。
class SessionStore {
 public:
  explicit SessionStore(Db& db);

  CreatedSession create(std::int64_t user_id, int idle_minutes = 30);
  // renewed 非空时回写这次访问是否触发了续期，调用方据此刷新 Cookie。
  std::optional<Session> get(const std::string& id, int idle_minutes = 30,
                             int absolute_hours = 12, bool* renewed = nullptr);

  void erase(const std::string& id);
  // 清理已过期的会话；启动时与每次登录成功后各调一次，不另设定时任务。
  void purge_expired();

 private:
  Db& db_;
};

}  // namespace xiaotu
