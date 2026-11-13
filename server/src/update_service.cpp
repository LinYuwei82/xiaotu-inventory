#include "xiaotu/update_service.h"

#include <algorithm>
#include <fstream>

#include <nlohmann/json.hpp>

namespace xiaotu {

UpdateCheckResult check_for_update(const UpdateSettings& settings, const UpdateQuery& query,
                                   std::int64_t now_epoch) {
  UpdateCheckResult result;
  if (settings.manifest_file.empty()) {
    result.error_code = "UPDATE_NOT_CONFIGURED";
    result.error_message = "未配置更新清单";
    return result;
  }
  result.configured = true;

  std::ifstream input(settings.manifest_file);
  if (!input) {
    result.error_code = "UPDATE_MANIFEST_UNREADABLE";
    result.error_message = "更新清单不可读";
    return result;
  }
  nlohmann::json body = nlohmann::json::parse(input, nullptr, false);
  if (body.is_discarded()) {
    result.error_code = "UPDATE_MANIFEST_INVALID_JSON";
    result.error_message = "更新清单不是合法 JSON";
    return result;
  }

  ManifestError error;
  if (!parse_manifest(body, result.manifest, error)) {
    result.error_code = error.code;
    result.error_message = error.message;
    return result;
  }
  if (!settings.channel.empty() && result.manifest.channel != settings.channel) {
    result.error_code = "UPDATE_CHANNEL_UNAVAILABLE";
    result.error_message = "当前通道没有可用清单";
    return result;
  }
  result.manifest.rollout_percent =
      std::min(result.manifest.rollout_percent, std::clamp(settings.rollout_percent, 1, 100));

  result.manifest_ok = true;
  result.decision = evaluate_manifest(result.manifest, query, now_epoch);

  // 服务端最低支持版本优先于清单判定：即使没有更高构建，也要求升级。
  if (!settings.min_supported_version.empty()) {
    ProductVersion current;
    ProductVersion minimum;
    if (parse_product_version(query.version, current) &&
        parse_product_version(settings.min_supported_version, minimum) &&
        compare_product_version(current, minimum) < 0) {
      result.decision.update_available = true;
      result.decision.mandatory = true;
      result.decision.reason = "MIN_SUPPORTED";
      result.decision.code = "UPDATE_REQUIRED";
    }
  }
  return result;
}

}  // namespace xiaotu
