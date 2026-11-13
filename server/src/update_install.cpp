#include "xiaotu/update_install.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "xiaotu/log.h"
#include "xiaotu/process_capture.h"

namespace xiaotu {
namespace {

std::string url_file_name(const std::string& url) {
  std::string path = url;
  const std::size_t query = path.find_first_of("?#");
  if (query != std::string::npos) {
    path.resize(query);
  }
  const std::size_t slash = path.find_last_of('/');
  std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  // 只保留安全字符，避免清单里的 URL 末段带路径分隔符或控制字符。
  std::string cleaned;
  for (const char ch : name) {
    const unsigned char value = static_cast<unsigned char>(ch);
    if (std::isalnum(value) || ch == '.' || ch == '-' || ch == '_') {
      cleaned.push_back(ch);
    } else {
      cleaned.push_back('_');
    }
  }
  if (cleaned.empty() || cleaned == "." || cleaned == "..") {
    cleaned = "update-package";
  }
  return cleaned;
}

std::string package_command_error(const ProcessResult& process) {
  if (!process.started) {
    return process.error.empty() ? "程序未能启动" : process.error;
  }
  const std::string detail = compact_output(process.output);
  if (!detail.empty()) {
    return detail;
  }
  return "退出码 " + std::to_string(process.exit_code);
}

}  // namespace

std::filesystem::path update_package_path(const std::filesystem::path& directory,
                                          const UpdateManifest& manifest) {
  return directory / (manifest.version + "-" + std::to_string(manifest.build) + "-" +
                      url_file_name(manifest.url));
}

UpdatePackageResult download_update_package(const UpdateSettings& settings,
                                            const std::string& manifest_file,
                                            const UpdateManifest& manifest,
                                            const std::filesystem::path& directory) {
  UpdatePackageResult result;
  result.path = update_package_path(directory, manifest);
  result.sha256 = manifest.sha256;

  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    result.error_code = "UPDATE_DOWNLOAD_FAILED";
    result.error_message = "无法创建下载目录：" + error.message();
    log_warn("更新下载失败：" + result.error_message);
    return result;
  }
  std::filesystem::remove(result.path, error);
  error.clear();

  // curl 在 Windows 10 1803 以上与 macOS 均随系统提供；不引入新的网络依赖。
  const std::vector<std::string> args = {
      "-f",          "-L",           "--silent", "--show-error",
      "--connect-timeout", "10",     "--max-time", "600",
      "--output",    result.path.string(),
      manifest.url};
  const ProcessResult process = run_process_capture("curl", args);
  if (!process.started) {
    result.error_code = "UPDATE_DOWNLOAD_UNAVAILABLE";
    result.error_message = "无法启动 curl 下载安装包：" + process_failure_text(process);
    log_warn("更新下载失败：" + result.error_message);
    return result;
  }
  if (!process.collected) {
    // curl 已经跑起来，但收不到退出状态：失败原因不是「启动不了」。
    result.error_code = "UPDATE_DOWNLOAD_FAILED";
    result.error_message = "下载安装包失败：" + process_failure_text(process);
    log_warn("更新下载失败：" + process_failure_text(process));
    return result;
  }
  if (process.exit_code != 0) {
    result.error_code = "UPDATE_DOWNLOAD_FAILED";
    result.error_message = "下载安装包失败：" + package_command_error(process);
    std::filesystem::remove(result.path, error);
    log_warn("更新下载失败（exit=" + std::to_string(process.exit_code) + "）：" +
             compact_output(process.output));
    return result;
  }
  if (!std::filesystem::exists(result.path)) {
    // curl 正常退出却没留下文件：失败原因与退出码无关，不要回落到「退出码 0」。
    result.error_code = "UPDATE_DOWNLOAD_FAILED";
    result.error_message = "下载安装包失败：curl 已正常退出，但未生成安装包文件";
    log_warn("更新下载失败：未生成安装包文件 " + result.path.string());
    return result;
  }

  result.bytes = std::filesystem::file_size(result.path, error);
  if (error) {
    result.bytes = 0;
  }
  if (manifest.size > 0 && result.bytes != static_cast<std::uintmax_t>(manifest.size)) {
    result.error_code = "UPDATE_SIZE_MISMATCH";
    result.error_message = "安装包大小与清单不一致：清单 " + std::to_string(manifest.size) +
                           " 字节，实际 " + std::to_string(result.bytes) + " 字节";
    std::filesystem::remove(result.path, error);
    log_warn("更新安装包大小不符，已删除下载文件：" + result.error_message);
    return result;
  }

  const PackageVerifyResult verified = verify_package(settings, manifest_file, result.path.string());
  if (!verified.ok) {
    result.error_code = verified.error_code;
    result.error_message = verified.error_message;
    std::filesystem::remove(result.path, error);
    log_warn("更新安装包校验失败，已删除下载文件：" + verified.error_code + " " +
             verified.error_message);
    return result;
  }

  result.ok = true;
  log_info("更新安装包下载并校验通过：" + result.path.filename().string() + " 字节=" +
           std::to_string(result.bytes) + " sha256=" + manifest.sha256);
  return result;
}

UpdatePackageResult launch_update_package(const UpdateSettings& settings,
                                          const std::string& manifest_file,
                                          const std::filesystem::path& package) {
  UpdatePackageResult result;
  result.path = package;
  const PackageVerifyResult verified = verify_package(settings, manifest_file, package.string());
  if (!verified.ok) {
    result.error_code = verified.error_code;
    result.error_message = verified.error_message;
    log_warn("安装前复验失败，未启动安装包：" + verified.error_message);
    return result;
  }

#if defined(_WIN32)
  const ProcessResult process =
      run_process_capture("cmd.exe", {"/c", "start", "", package.string()});
#elif defined(__APPLE__)
  const ProcessResult process = run_process_capture("/usr/bin/open", {package.string()});
#else
  result.error_code = "UPDATE_INSTALL_UNSUPPORTED";
  result.error_message = "当前平台不支持自动打开安装包，请手动安装：" + package.filename().string();
  return result;
#endif

  if (!process.started) {
    result.error_code = "UPDATE_INSTALL_FAILED";
    result.error_message = "无法打开安装包：" + process_failure_text(process);
    log_warn("打开更新安装包失败：" + result.error_message);
    return result;
  }
  if (!process.collected) {
    result.error_code = "UPDATE_INSTALL_FAILED";
    result.error_message = "打开安装包失败：" + process_failure_text(process);
    log_warn("打开更新安装包失败：" + process_failure_text(process));
    return result;
  }
  if (process.exit_code != 0) {
    result.error_code = "UPDATE_INSTALL_FAILED";
    result.error_message = "无法打开安装包：" + package_command_error(process);
    log_warn("打开更新安装包失败：" + result.error_message);
    return result;
  }
  result.ok = true;
  result.mode = "manual";
  result.error_message = "已打开安装包，请按安装向导完成更新；安装前请退出本程序。";
  log_info("已打开更新安装包：" + package.filename().string());
  return result;
}

}  // namespace xiaotu
