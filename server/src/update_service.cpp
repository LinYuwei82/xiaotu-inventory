#include "xiaotu/update_service.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <nlohmann/json.hpp>

namespace xiaotu {
namespace {

#if defined(_WIN32)

std::wstring utf8_to_wide(const std::string& value) {
  if (value.empty()) {
    return std::wstring();
  }
  const int length = ::MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0);
  if (length <= 0) {
    return std::wstring();
  }
  std::wstring wide(static_cast<std::size_t>(length), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), wide.data(),
                        length);
  return wide;
}

// 按 CommandLineToArgvW 的规则给单个参数加引号：反斜杠数量按官方算法翻倍，
// 保证含空格或引号的路径原样传到子进程。
std::wstring quote_windows_arg(const std::wstring& value) {
  if (!value.empty() && value.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
    return value;
  }
  std::wstring quoted = L"\"";
  std::size_t index = 0;
  while (true) {
    std::size_t backslashes = 0;
    while (index < value.size() && value[index] == L'\\') {
      ++index;
      ++backslashes;
    }
    if (index == value.size()) {
      quoted.append(backslashes * 2, L'\\');
      break;
    }
    if (value[index] == L'"') {
      quoted.append(backslashes * 2 + 1, L'\\');
    } else {
      quoted.append(backslashes, L'\\');
    }
    quoted.push_back(value[index]);
    ++index;
  }
  quoted.push_back(L'"');
  return quoted;
}

bool is_batch_script(const std::string& path) {
  std::string extension = std::filesystem::path(path).extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return extension == ".bat" || extension == ".cmd";
}

// 不经 shell：可执行程序与参数分开交给 CreateProcess，StdOut/StdErr 直接挂到 NUL。
// 只有 .bat/.cmd 这类脚本必须由 cmd.exe 解释，此时按 cmd /c ""<脚本>" <参数>" 的规则
// 在整条命令外再加一层引号，否则 cmd 会剥掉首尾引号、把含空格的路径截断。
int run_verifier_process(const std::string& program, const std::vector<std::string>& args) {
  std::wstring command_line = quote_windows_arg(utf8_to_wide(program));
  for (const std::string& arg : args) {
    command_line += L' ';
    command_line += quote_windows_arg(utf8_to_wide(arg));
  }

  std::wstring application = utf8_to_wide(program);
  if (is_batch_script(program)) {
    wchar_t comspec[MAX_PATH] = {};
    const DWORD comspec_length = ::GetEnvironmentVariableW(L"ComSpec", comspec, MAX_PATH);
    application = (comspec_length > 0 && comspec_length < MAX_PATH)
                      ? std::wstring(comspec)
                      : std::wstring(L"C:\\Windows\\System32\\cmd.exe");
    command_line = quote_windows_arg(application) + L" /c \"" + command_line + L"\"";
  }

  SECURITY_ATTRIBUTES inherit{};
  inherit.nLength = sizeof(inherit);
  inherit.bInheritHandle = TRUE;
  // 三个标准句柄全部显式绑到 NUL，一个都不继承宿主：
  // STDIN 若沿用 GetStdHandle(STD_INPUT_HANDLE)，在无控制台的宿主里（壳用
  // CREATE_NO_WINDOW 拉起、或被计划任务/SMB 会话启动）拿到的是无效句柄，
  // 验签工具读 stdin 会异常退出，被误判成「验签失败」。
  // 读、写都要用到同一个 NUL，所以句柄按 GENERIC_READ|GENERIC_WRITE 打开。
  HANDLE null_device =
      ::CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    &inherit, OPEN_EXISTING, 0, nullptr);
  if (null_device == INVALID_HANDLE_VALUE) {
    return -1;
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  // STARTF_USESTDHANDLES 要求三个句柄都为可继承的有效句柄，配合 bInheritHandles=TRUE。
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = null_device;
  startup.hStdOutput = null_device;
  startup.hStdError = null_device;

  PROCESS_INFORMATION process{};
  if (::CreateProcessW(application.c_str(), command_line.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) == FALSE) {
    ::CloseHandle(null_device);
    return -1;
  }
  ::WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exit_code = 1;
  ::GetExitCodeProcess(process.hProcess, &exit_code);
  ::CloseHandle(process.hThread);
  ::CloseHandle(process.hProcess);
  ::CloseHandle(null_device);
  return static_cast<int>(exit_code);
}

#else

std::string shell_quote(const std::string& value) {
  std::string quoted = "'";
  for (const char ch : value) {
    if (ch == '\'') {
      quoted += "'\\''";
    } else {
      quoted += ch;
    }
  }
  quoted += "'";
  return quoted;
}

#endif

// 用发布侧同一套 Python 工具验签，确保规范化规则与生成侧一致。
bool verify_manifest_signature(const UpdateSettings& settings, const std::string& manifest_file,
                               UpdateCheckResult& result) {
  if (settings.public_key_file.empty()) {
    result.error_code = "UPDATE_UNVERIFIED";
    result.error_message = "未配置更新公钥，拒绝使用未验签的清单";
    return false;
  }
  if (settings.verifier_script.empty() ||
      !std::filesystem::exists(settings.verifier_script) ||
      !std::filesystem::exists(settings.public_key_file)) {
    result.error_code = "UPDATE_VERIFY_UNAVAILABLE";
    result.error_message = "验签工具或公钥不可用";
    return false;
  }
#if defined(_WIN32)
  const std::vector<std::string> verifier_args = {
      settings.verifier_script, "verify", "--manifest", manifest_file, "--pub",
      settings.public_key_file};
  const bool verified = run_verifier_process(settings.verifier_python, verifier_args) == 0;
#else
  const std::string command = shell_quote(settings.verifier_python) + " " +
                              shell_quote(settings.verifier_script) + " verify --manifest " +
                              shell_quote(manifest_file) + " --pub " +
                              shell_quote(settings.public_key_file) + " >/dev/null 2>&1";
  const bool verified = std::system(command.c_str()) == 0;
#endif
  if (!verified) {
    result.error_code = "UPDATE_SIGNATURE_INVALID";
    result.error_message = "更新清单签名校验失败";
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
  if (!verify_manifest_signature(settings, settings.manifest_file, result)) {
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

}  // namespace xiaotu
