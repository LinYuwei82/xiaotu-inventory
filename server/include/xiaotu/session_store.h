#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace xiaotu {

struct Session {
  std::int64_t user_id = 0;
  std::string user;
  std::string role;
};

// 内存会话表。M0 骨架不落库，进程重启即失效；
// 网络版接入用户体系后替换为持久化实现即可（接口保持不变）。
class SessionStore {
 public:
  std::string create(std::int64_t user_id, std::string user, std::string role);
  std::optional<Session> get(const std::string& id) const;
  void erase(const std::string& id);

 private:
  mutable std::mutex mutex_;
  std::unordered_map<std::string, Session> sessions_;
};

}  // namespace xiaotu
