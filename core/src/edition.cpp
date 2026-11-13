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

namespace {

// 面向用户的上限名称；未登记的键退回原始键名。
std::string_view quota_label(std::string_view key) {
  if (key == "max_items") return "商品数";
  if (key == "max_documents") return "单据数";
  if (key == "max_users") return "用户数";
  if (key == "max_warehouses") return "仓库数";
  if (key == "max_partners") return "往来单位数";
  if (key == "max_employees") return "职员数";
  if (key == "max_print_templates") return "打印模板数";
  return key;
}

}  // namespace

void EditionPolicy::check_quota(std::string_view limit_key, std::int64_t current,
                                std::int64_t delta) const {
  const std::int64_t max = limit(limit_key);
  if (max < 0 || current + delta <= max) {
    return;
  }
  throw AppError("QUOTA_EXCEEDED", "已达当前版本上限：" + std::string(quota_label(limit_key)) +
                                       "最多 " + std::to_string(max));
}

}  // namespace xiaotu
