#include "xiaotu/login_limiter.h"

namespace xiaotu {
namespace {

constexpr int kMaxFailures = 5;
constexpr auto kWindow = std::chrono::minutes(5);

bool expired(const std::chrono::steady_clock::time_point& start) {
  return std::chrono::steady_clock::now() - start >= kWindow;
}

}  // namespace

bool LoginLimiter::blocked(const std::string& key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = entries_.find(key);
  if (it == entries_.end()) {
    return false;
  }
  if (expired(it->second.window_start)) {
    entries_.erase(it);
    return false;
  }
  return it->second.failures >= kMaxFailures;
}

void LoginLimiter::record_failure(const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto now = std::chrono::steady_clock::now();
  auto& entry = entries_[key];
  if (entry.failures == 0 || expired(entry.window_start)) {
    entry.failures = 0;
    entry.window_start = now;
  }
  ++entry.failures;
}

void LoginLimiter::reset(const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.erase(key);
}

}  // namespace xiaotu
