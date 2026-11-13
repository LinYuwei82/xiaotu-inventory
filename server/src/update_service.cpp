#include "xiaotu/update_service.h"

#include <algorithm>
#include <climits>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "xiaotu/log.h"
#include "xiaotu/process_capture.h"

namespace xiaotu {
namespace {

// 验签或下载失败时，把子进程输出压成一行带进错误信息：
// 只回传退出码会让「公钥不对 / 验签工具报错 / 安装包哈希不符」看起来一模一样。
std::string process_failure_detail(const ProcessResult& process) {
  if (!process.started) {
    return process.error.empty() ? "程序未能启动" : process.error;
  }
  const std::string line = last_output_line(process.output);
  if (!line.empty()) {
    return line;
  }
  return "退出码 " + std::to_string(process.exit_code) + "（无输出）";
}

// 用发布侧同一套 Python 工具验签，确保规范化规则与生成侧一致。
// 传 file 时同时校验安装包 sha256，避免「清单签名对、包被换」的中间状态。
bool verify_manifest_signature(const UpdateSettings& settings, const std::string& manifest_file,
                               const std::string& package_file, UpdateCheckResult& result) {
  if (settings.public_key_file.empty()) {
    result.error_code = "UPDATE_UNVERIFIED";
    result.error_message = "未配置更新公钥，拒绝使用未验签的清单";
    return false;
  }
  if (settings.verifier_script.empty() || !std::filesystem::exists(settings.verifier_script) ||
      !std::filesystem::exists(settings.public_key_file)) {
    result.error_code = "UPDATE_VERIFY_UNAVAILABLE";
    result.error_message = "验签工具或公钥不可用";
    return false;
  }
  std::vector<std::string> args = {settings.verifier_script, "verify", "--manifest", manifest_file,
                                   "--pub", settings.public_key_file};
  if (!package_file.empty()) {
    args.push_back("--file");
    args.push_back(package_file);
  }

  const ProcessResult process = run_process_capture(settings.verifier_python, args);
  if (!process.started) {
    result.error_code = "UPDATE_VERIFY_UNAVAILABLE";
    result.error_message = "无法启动验签工具：" + process_failure_detail(process);
    log_warn("更新验签工具启动失败：" + process_failure_detail(process));
    return false;
  }
  if (process.exit_code != 0) {
    result.error_code = "UPDATE_SIGNATURE_INVALID";
    result.error_message =
        "更新清单签名校验失败：" + process_failure_detail(process);
    log_warn("更新验签失败（exit=" + std::to_string(process.exit_code) + "）：" +
             compact_output(process.output));
    return false;
  }
  return true;
}

}  // namespace

UpdateCheckResult check_for_update(const UpdateSettings& settings, const UpdateQuery& query,
                                   std::int64_t now_epoch) {
  UpdateCheckResult result;
  if (settings.manifest_file.empty()) {
    result.error_code = "UPDATE_NOT_CONFIGURED";
    result.error_message = "未配置更新清单（[update] manifest_file）";
    return result;
  }
  result.configured = true;

  std::ifstream input(settings.manifest_file);
  if (!input) {
    result.error_code = "UPDATE_MANIFEST_UNREADABLE";
    result.error_message = "更新清单不可读：" + settings.manifest_file;
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
  if (!verify_manifest_signature(settings, settings.manifest_file, {}, result)) {
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
  // 但旧 schema 必须逐级迁移时，保持 INTERMEDIATE_REQUIRED，避免直接跳最新版。
  if (!result.decision.intermediate_required && !settings.min_supported_version.empty()) {
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

PackageVerifyResult verify_package(const UpdateSettings& settings,
                                   const std::string& manifest_file,
                                   const std::string& package_file) {
  PackageVerifyResult result;
  if (package_file.empty() || !std::filesystem::exists(package_file)) {
    result.error_code = "UPDATE_PACKAGE_MISSING";
    result.error_message = "安装包不存在，请先下载";
    return result;
  }
  UpdateCheckResult check;
  if (!verify_manifest_signature(settings, manifest_file, package_file, check)) {
    result.error_code = check.error_code;
    result.error_message = check.error_message;
    return result;
  }
  result.ok = true;
  return result;
}

}  // namespace xiaotu
