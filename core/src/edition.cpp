#include "xiaotu/edition.h"

#include <string>

#include "xiaotu/error.h"
#include "xiaotu/gen/edition_caps.h"

namespace xiaotu {

const EditionPolicy& EditionPolicy::instance() {
  static const EditionPolicy policy;
  return policy;
}

std::string_view EditionPolicy::code() const { return gen::active_edition().code; }

std::string_view EditionPolicy::label() const { return gen::active_edition().label; }

std::string_view EditionPolicy::state(std::string_view feature) const {
  for (const auto& cap : gen::active_edition().features) {
    if (cap.key == feature) {
      return cap.state;
    }
  }
  return "hidden";
}

bool EditionPolicy::available(std::string_view feature) const { return state(feature) != "hidden"; }

void EditionPolicy::require(std::string_view feature) const {
  if (!available(feature)) {
    throw AppError("FEATURE_UNAVAILABLE", "当前版本不提供该功能: " + std::string(feature));
  }
}

std::int64_t EditionPolicy::limit(std::string_view limit_key) const {
  for (const auto& quota : gen::active_edition().quotas) {
    if (quota.key == limit_key) {
      return quota.limit;
    }
  }
  return -1;
}

void EditionPolicy::check_quota(std::string_view limit_key, std::int64_t current,
                                std::int64_t delta) const {
  const std::int64_t max = limit(limit_key);
  if (max < 0 || current + delta <= max) {
    return;
  }
  throw AppError("QUOTA_EXCEEDED",
                 "已达当前版本上限（" + std::string(limit_key) + " = " + std::to_string(max) + "）");
}

}  // namespace xiaotu
