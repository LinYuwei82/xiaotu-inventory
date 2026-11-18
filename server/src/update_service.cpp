#include "xiaotu/update_service.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "xiaotu/log.h"
#include "xiaotu/process_capture.h"

namespace xiaotu {
namespace {

// 远端清单同步是低频动作，但可能被多个请求线程与后台调度同时触发。
// 进程内串行化，避免同一时刻重复下载、并发改写同一个缓存文件。
std::mutex g_manifest_mutex;

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
    result.error_message = "无法启动验签工具：" + process_failure_text(process);
    log_warn("更新验签工具启动失败：" + process_failure_text(process));
    return false;
  }
  if (!process.collected) {
    // 验签工具已经跑起来，但收不到退出状态：既不是签名无效，也不是没启动。
    // 复用 UNAVAILABLE：验签没有完成，调用方按「不可用」处理。
    result.error_code = "UPDATE_VERIFY_UNAVAILABLE";
    result.error_message = "更新清单验签失败：" + process_failure_text(process);
    log_warn("更新验签失败：" + process_failure_text(process));
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

// 清单同步失败后的重试间隔：离线时不要把每次登录检查都拖成一次 5–20 秒的 curl 超时。
constexpr std::int64_t kManifestRetrySeconds = 60;

struct FailureMarker {
  std::int64_t at = 0;
  std::string message;
};

FailureMarker read_failure_marker(const std::string& path) {
  FailureMarker marker;
  std::ifstream input(path);
  if (!input) {
    return marker;
  }
  std::string first;
  if (!std::getline(input, first)) {
    return marker;
  }
  try {
    marker.at = std::stoll(first);
  } catch (const std::exception&) {
    marker.at = 0;
    return marker;
  }
  std::string line;
  while (std::getline(input, line)) {
    if (!marker.message.empty()) {
      marker.message.push_back('\n');
    }
    marker.message += line;
  }
  return marker;
}

void write_failure_marker(const std::string& path, std::int64_t at, const std::string& message) {
  std::ofstream out(path, std::ios::trunc);
  if (out) {
    out << at << "\n" << message << "\n";
  }
}

std::int64_t file_mtime_epoch(const std::string& path) {
  std::error_code error;
  const auto modified = std::filesystem::last_write_time(path, error);
  if (error) {
    return 0;
  }
  // file_time_type 的纪元与 system_clock 不同，先换算到 system_clock 再取秒。
  const auto system_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
      modified - std::filesystem::file_time_type::clock::now() +
      std::chrono::system_clock::now());
  return std::chrono::duration_cast<std::chrono::seconds>(system_time.time_since_epoch()).count();
}

bool starts_with_https(const std::string& url) {
  return url.rfind("https://", 0) == 0;
}

// 先写临时文件再替换：缓存目录与目标同目录，避免跨盘 rename 失败。
// Windows 的 rename 不能覆盖已存在文件，改用 MoveFileEx(MOVEFILE_REPLACE_EXISTING)。
bool replace_file(const std::string& from, const std::string& to) {
#if defined(_WIN32)
  return ::MoveFileExA(from.c_str(), to.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  std::error_code error;
  std::filesystem::rename(from, to, error);
  return !error;
#endif
}

bool download_manifest(const UpdateSettings& settings, const std::string& target,
                       ProcessResult& process) {
  std::vector<std::string> args = {"-f",       "-L",           "--silent",
                                   "--show-error", "--connect-timeout", "5",
                                   "--max-time", "20",         "--output",
                                   target,     settings.manifest_url};
  if (starts_with_https(settings.manifest_url)) {
    // 只允许 HTTPS，防止清单地址被降级成明文传输。
    args.insert(args.begin(), {"--proto", "=https"});
  }
  process = run_process_capture("curl", args);
  return process.started && process.collected && process.exit_code == 0 &&
         std::filesystem::exists(target);
}

// 缓存清单必须结构与签名都通过才允许替换，避免把损坏或未签名内容写进缓存。
bool validate_manifest_file(const UpdateSettings& settings, const std::string& path,
                            UpdateCheckResult& result) {
  std::ifstream input(path);
  if (!input) {
    result.error_code = "UPDATE_MANIFEST_UNREADABLE";
    result.error_message = "更新清单不可读：" + path;
    return false;
  }
  const nlohmann::json body = nlohmann::json::parse(input, nullptr, false);
  if (body.is_discarded()) {
    result.error_code = "UPDATE_MANIFEST_INVALID_JSON";
    result.error_message = "更新清单不是合法 JSON";
    return false;
  }
  ManifestError error;
  UpdateManifest manifest;
  if (!parse_manifest(body, manifest, error)) {
    result.error_code = error.code;
    result.error_message = error.message;
    return false;
  }
  return verify_manifest_signature(settings, path, {}, result);
}

}  // namespace

ManifestSyncResult sync_update_manifest(const UpdateSettings& settings,
                                        std::int64_t now_epoch) {
  ManifestSyncResult result;
  if (settings.manifest_url.empty()) {
    result.ok = !settings.manifest_file.empty();
    result.path = settings.manifest_file;
    if (!result.ok) {
      result.error_code = "UPDATE_NOT_CONFIGURED";
      result.error_message = "未配置更新清单（[update] manifest_file / manifest_url）";
    }
    return result;
  }
  result.path = settings.manifest_cache_file;
  if (settings.manifest_cache_file.empty()) {
    result.error_code = "UPDATE_MANIFEST_CACHE_MISSING";
    result.error_message = "配置了 manifest_url 但缺少本地缓存路径";
    return result;
  }

  std::lock_guard<std::mutex> lock(g_manifest_mutex);
  const bool cached = std::filesystem::exists(settings.manifest_cache_file);
  const std::int64_t modified = cached ? file_mtime_epoch(settings.manifest_cache_file) : 0;

  // 缓存在有效期内直接使用：客户端每次登录都会来查一次，不能每次都打外网。
  // ttl <= 0 表示不缓存（每次都刷新），便于测试与强制检查。
  if (cached && modified > 0 && now_epoch >= modified && settings.manifest_ttl_seconds > 0 &&
      now_epoch - modified < settings.manifest_ttl_seconds) {
    result.ok = true;
    result.path = settings.manifest_cache_file;
    result.using_cache = true;
    return result;
  }

  // 拉取失败时回退到上一次验签通过的缓存；没有缓存才报错。
  const std::string failure_file = settings.manifest_cache_file + ".error";
  auto fallback = [&](const std::string& code, const std::string& message) {
    if (cached) {
      write_failure_marker(failure_file, now_epoch, message);
      log_warn("更新：远端清单不可用，回退到本地缓存（" + code + "）：" + message);
      result.ok = true;
      result.using_cache = true;
      result.error_code = code;
      result.error_message = message;
      return result;
    }
    result.ok = false;
    result.error_code = code;
    result.error_message = message;
    return result;
  };

  // 最近失败过：在重试间隔内直接用缓存，避免离线时每次请求都等网络超时。
  if (cached) {
    const FailureMarker marker = read_failure_marker(failure_file);
    if (marker.at > 0 && now_epoch >= marker.at &&
        now_epoch - marker.at < kManifestRetrySeconds) {
      result.ok = true;
      result.using_cache = true;
      result.error_code = "UPDATE_MANIFEST_UNREACHABLE";
      result.error_message = marker.message.empty() ? "远端清单暂不可用，稍后重试"
                                                    : marker.message;
      return result;
    }
  }

  const std::filesystem::path cache_path(settings.manifest_cache_file);
  std::error_code error;
  std::filesystem::create_directories(cache_path.parent_path(), error);
  if (error) {
    return fallback("UPDATE_MANIFEST_CACHE_UNWRITABLE",
                    "无法创建更新清单缓存目录：" + error.message());
  }

  const std::string temporary = settings.manifest_cache_file + ".download";
  std::filesystem::remove(temporary, error);
  ProcessResult process;
  if (!download_manifest(settings, temporary, process)) {
    std::filesystem::remove(temporary, error);
    if (!process.started) {
      return fallback("UPDATE_MANIFEST_UNREACHABLE",
                      "无法启动 curl 拉取更新清单：" + process_failure_text(process));
    }
    if (!process.collected) {
      return fallback("UPDATE_MANIFEST_UNREACHABLE",
                      "拉取更新清单失败：" + process_failure_text(process));
    }
    std::string detail = compact_output(process.output);
    if (detail.empty()) {
      detail = process.exit_code == 0 ? "curl 已正常退出，但未生成清单文件" : "无输出";
    }
    return fallback("UPDATE_MANIFEST_UNREACHABLE",
                    "拉取更新清单失败（exit=" + std::to_string(process.exit_code) + "）：" +
                        detail);
  }

  UpdateCheckResult check;
  if (!validate_manifest_file(settings, temporary, check)) {
    std::filesystem::remove(temporary, error);
    log_warn("更新：远端清单校验失败，未写入缓存（" + check.error_code + "）：" +
             check.error_message);
    return fallback(check.error_code, check.error_message);
  }

  if (!replace_file(temporary, settings.manifest_cache_file)) {
    std::filesystem::remove(temporary, error);
    return fallback("UPDATE_MANIFEST_CACHE_UNWRITABLE", "更新清单写入缓存失败");
  }
  std::filesystem::remove(failure_file, error);

  result.ok = true;
  result.from_network = true;
  result.path = settings.manifest_cache_file;
  log_info("更新：已从厂商更新源刷新清单 " + settings.manifest_cache_file);
  return result;
}

UpdateCheckResult check_for_update(const UpdateSettings& settings, const UpdateQuery& query,
                                   std::int64_t now_epoch) {
  UpdateCheckResult result;
  if (settings.manifest_url.empty() && settings.manifest_file.empty()) {
    result.error_code = "UPDATE_NOT_CONFIGURED";
    result.error_message = "未配置更新清单（[update] manifest_file / manifest_url）";
    return result;
  }
  result.configured = true;

  const ManifestSyncResult sync = sync_update_manifest(settings, now_epoch);
  if (!sync.ok) {
    result.error_code = sync.error_code;
    result.error_message = sync.error_message;
    return result;
  }
  result.manifest_source = sync.from_network ? "network" : "file";
  if (!settings.manifest_url.empty() && !sync.from_network) {
    result.manifest_source = "cache";
  }
  result.manifest_stale = !sync.error_message.empty();
  result.manifest_warning = sync.error_message;
  result.manifest_path = sync.path;

  std::ifstream input(sync.path);
  if (!input) {
    result.error_code = "UPDATE_MANIFEST_UNREADABLE";
    result.error_message = "更新清单不可读：" + sync.path;
    return result;
  }
  nlohmann::json body = nlohmann::json::parse(input, nullptr, false);
  if (body.is_discarded()) {
    result.error_code = "UPDATE_MANIFEST_INVALID_JSON";
    result.error_message = "更新清单不是合法 JSON";
    return result;
  }

  ManifestError error;
  // v2 清单按平台拍平：查询方声明的平台决定读 platforms.macos 还是 platforms.windows。
  // Linux 等非桌面平台不参与更新分发，按空平台做结构校验，后续 evaluate 报 PLATFORM_MISMATCH。
  const std::string requested_platform =
      (query.platform == "macos" || query.platform == "windows") ? query.platform : std::string();
  if (!parse_manifest(body, requested_platform, result.manifest, error)) {
    result.error_code = error.code;
    result.error_message = error.message;
    return result;
  }
  if (!verify_manifest_signature(settings, sync.path, {}, result)) {
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
