#pragma once

#include <cstdint>
#include <string_view>

namespace xiaotu {

// 版本能力判定：所有版本差异只在服务层调用这里，路由与界面不出现版本字符串。
// 能力表由 tools/codegen/schema/editions.json 生成，构建参数 -DXIAOTU_EDITION 选择生效版本。
class EditionPolicy {
 public:
  static const EditionPolicy& instance();

  std::string_view code() const;
  std::string_view label() const;

  // full / limited / hidden；未登记的能力按 hidden 处理，避免漏配后默认放行。
  std::string_view state(std::string_view feature) const;
  bool available(std::string_view feature) const;

  // hidden 能力直接拒绝，统一抛 FEATURE_UNAVAILABLE。
  void require(std::string_view feature) const;

  // -1 表示不限。
  std::int64_t limit(std::string_view limit_key) const;

  // 新增与启用前校验额度；超限抛 QUOTA_EXCEEDED，存量数据仍可查询与导出。
  void check_quota(std::string_view limit_key, std::int64_t current, std::int64_t delta) const;
};

}  // namespace xiaotu
