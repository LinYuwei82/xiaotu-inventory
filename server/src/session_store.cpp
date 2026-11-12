#include "xiaotu/session_store.h"

#include <utility>

#include "xiaotu/ids.h"

namespace xiaotu {

CreatedSession SessionStore::create(std::int64_t user_id, std::string user,
                                    std::string role) {
  CreatedSession created{.id = random_hex(32), .csrf = random_hex(32)};
  std::lock_guard<std::mutex> lock(mutex_);
  sessions_[created.id] = Session{user_id, std::move(user), std::move(role), created.csrf};
  return created;
}

std::optional<Session> SessionStore::get(const std::string& id) const {
  if (id.empty()) {
    return std::nullopt;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = sessions_.find(id);
  if (it == sessions_.end()) {
    return std::nullopt;
  }
  return it->second;
}

void SessionStore::erase(const std::string& id) {
  std::lock_guard<std::mutex> lock(mutex_);
  sessions_.erase(id);
}

}  // namespace xiaotu
