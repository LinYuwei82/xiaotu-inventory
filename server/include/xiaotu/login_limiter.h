#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

namespace xiaotu {

// 登录失败限流：同一来源 + 用户名在窗口内失败达到上限后暂时拒绝。
class LoginLimiter {
 public:
  bool blocked(const std::string& key) const;
  void record_failure(const std::string& key);
  void reset(const std::string& key);

 private:
  struct Entry {
    int failures = 0;
    std::chrono::steady_clock::time_point window_start;
  };

  mutable std::mutex mutex_;
  mutable std::unordered_map<std::string, Entry> entries_;
};

}  // namespace xiaotu
