#include "xiaotu/update_install.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

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

std::string lower_extension(const std::filesystem::path& path) {
  std::string extension = path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return extension;
}

bool same_file_name(const std::string& left, const std::string& right) {
#if defined(_WIN32)
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (std::tolower(static_cast<unsigned char>(left[index])) !=
        std::tolower(static_cast<unsigned char>(right[index]))) {
      return false;
    }
  }
  return true;
#else
  return left == right;
#endif
}

// 在解压目录里找与当前可执行文件同名的文件：优先层级最浅的一份。
std::filesystem::path find_payload(const std::filesystem::path& staging,
                                   const std::string& name) {
  std::filesystem::path chosen;
  int chosen_depth = -1;
  std::error_code error;
  for (std::filesystem::recursive_directory_iterator entry(staging, error), end;
       !error && entry != end; entry.increment(error)) {
    std::error_code type_error;
    if (!entry->is_regular_file(type_error) || type_error) {
      continue;
    }
    if (!same_file_name(entry->path().filename().string(), name)) {
      continue;
    }
    const int depth = entry.depth();
    if (chosen.empty() || depth < chosen_depth ||
        (depth == chosen_depth && entry->path() < chosen)) {
      chosen = entry->path();
      chosen_depth = depth;
    }
  }
  return chosen;
}

// 同一时间只保留本次待用的替换暂存目录，避免反复尝试后累积旧版本解压内容。
void remove_old_staging(const std::filesystem::path& staging) {
  const std::filesystem::path parent = staging.parent_path();
  if (parent.empty()) {
    return;
  }
  std::error_code error;
  std::filesystem::directory_iterator entry(parent, error);
  if (error) {
    return;
  }
  for (const std::filesystem::directory_iterator end; entry != end; entry.increment(error)) {
    if (error) {
      return;
    }
    std::error_code type_error;
    if (!entry->is_directory(type_error) || type_error) {
      continue;
    }
    const std::string name = entry->path().filename().string();
    if (name.rfind("staging-", 0) != 0 || entry->path() == staging) {
      continue;
    }
    std::error_code remove_error;
    std::filesystem::remove_all(entry->path(), remove_error);
  }
}

bool extract_archive(const std::filesystem::path& package,
                     const std::filesystem::path& staging, std::string* detail) {  std::filesystem::create_directories(staging);
  // 先试 tar：Windows 10+ 的 bsdtar 与 macOS/Linux 的 tar 都能解 zip。
  const ProcessResult tar =
      run_process_capture("tar", {"-xf", package.string(), "-C", staging.string()});
  if (tar.started && tar.collected && tar.exit_code == 0) {
    return true;
  }
  std::string errors = "tar：" + package_command_error(tar);
#if defined(_WIN32)
  // bsdtar 不可用时回落到 PowerShell 的 Expand-Archive。
  if (package.string().find('\'') == std::string::npos) {
    const std::string command =
        "Expand-Archive -LiteralPath '" + package.string() + "' -DestinationPath '" +
        staging.string() + "' -Force";
    const ProcessResult powershell = run_process_capture(
        "powershell.exe",
        {"-NoProfile", "-NonInteractive", "-Command", command});
    if (powershell.started && powershell.collected && powershell.exit_code == 0) {
      return true;
    }
    errors += "；PowerShell：" + package_command_error(powershell);
  }
#else
  const ProcessResult unzip =
      run_process_capture("unzip", {"-o", "-q", package.string(), "-d", staging.string()});
  if (unzip.started && unzip.collected && unzip.exit_code == 0) {
    return true;
  }
  errors += "；unzip：" + package_command_error(unzip);
#endif
  *detail = errors;
  return false;
}

// 脚本文件按 UTF-8 写入，路径也统一转成 UTF-8；Windows 侧脚本开头会切到 65001。
std::string path_utf8(const std::filesystem::path& path) {
#if defined(_WIN32)
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
#else
  return path.string();
#endif
}

std::string shell_quote(const std::string& value) {  // POSIX 单引号内除单引号本身都是字面量；单引号用 '\'' 拼接。
  std::string quoted = "'";
  for (const char ch : value) {
    if (ch == '\'') {
      quoted += "'\\''";
    } else {
      quoted.push_back(ch);
    }
  }
  quoted.push_back('\'');
  return quoted;
}

bool script_value_safe(const std::string& value) {
  return value.find('\n') == std::string::npos && value.find('\r') == std::string::npos;
}

bool windows_value_safe(const std::string& value) {
  if (!script_value_safe(value)) {
    return false;
  }
  // 批处理会展开 %VAR% 与 !VAR!；^ 在引号外是转义符。这些字符不做兼容，
  // 直接拒绝，避免生成一个看似成功、实际替换错文件的脚本。
  return value.find_first_of("\"%!^") == std::string::npos;
}

std::string join_windows_args(const std::vector<std::string>& args, bool* ok) {
  std::string line;
  for (const std::string& arg : args) {
    if (!windows_value_safe(arg)) {
      *ok = false;
      return {};
    }
    if (!line.empty()) {
      line.push_back(' ');
    }
    line += "\"" + arg + "\"";
  }
  return line;
}

std::string join_shell_args(const std::vector<std::string>& args, bool* ok) {
  std::string line;
  for (const std::string& arg : args) {
    if (!script_value_safe(arg)) {
      *ok = false;
      return {};
    }
    if (!line.empty()) {
      line.push_back(' ');
    }
    line += shell_quote(arg);
  }
  return line;
}

long long current_process_id() {
#if defined(_WIN32)
  return static_cast<long long>(::GetCurrentProcessId());
#else
  return static_cast<long long>(::getpid());
#endif
}

#if defined(_WIN32)
// 不继承句柄地拉起脚本：如果沿用 run_process_capture，脚本会继承输出管道写端，
// 父进程读管道会一直等到脚本结束，而脚本又在等父进程退出，形成死锁。
bool launch_detached(const std::filesystem::path& script, std::string* error) {
  std::wstring command = L"cmd.exe /c \"" + script.wstring() + L"\"";
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  const BOOL created = ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                                        DETACHED_PROCESS | CREATE_NO_WINDOW, nullptr, nullptr,
                                        &startup, &process);
  if (created == FALSE) {
    *error = "无法启动替换脚本（系统错误 " + std::to_string(::GetLastError()) + "）";
    return false;
  }
  ::CloseHandle(process.hThread);
  ::CloseHandle(process.hProcess);
  return true;
}
#endif

}  // namespace

std::filesystem::path update_package_path(const std::filesystem::path& directory,
                                          const UpdateManifest& manifest) {
  return directory / (manifest.version + "-" + std::to_string(manifest.build) + "-" +
                      url_file_name(manifest.url));
}

void remove_update_packages(const std::filesystem::path& directory,
                            const std::filesystem::path& keep) {
  std::error_code error;
  std::filesystem::directory_iterator entry(directory, error);
  if (error) {
    return;
  }
  const std::string keep_name = keep.filename().string();
  for (const std::filesystem::directory_iterator end; entry != end; entry.increment(error)) {
    if (error) {
      return;
    }
    std::error_code type_error;
    if (!entry->is_regular_file(type_error) || type_error) {
      continue;
    }
    if (!keep_name.empty() && entry->path().filename().string() == keep_name) {
      continue;
    }
    std::error_code remove_error;
    std::filesystem::remove(entry->path(), remove_error);
  }
}

UpdatePackageResult download_update_package(const UpdateSettings& settings,
                                            const std::string& manifest_file,
                                            const UpdateManifest& manifest,
                                            const std::filesystem::path& directory,
                                            const std::atomic<bool>* cancel) {
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
  // 同一时间只保留一个待装包：开始下载前清掉目录里的旧包，避免版本累积占磁盘。
  remove_update_packages(directory, result.path);
  std::filesystem::remove(result.path, error);
  error.clear();

  if (cancel != nullptr && cancel->load()) {
    result.error_code = "UPDATE_DOWNLOAD_CANCELED";
    result.error_message = "已取消下载";
    log_info("更新下载已取消：" + result.path.filename().string());
    return result;
  }

  // curl 在 Windows 10 1803 以上与 macOS 均随系统提供；不引入新的网络依赖。
  const std::vector<std::string> args = {
      "-f",          "-L",           "--silent", "--show-error",
      "--connect-timeout", "10",     "--max-time", "600",
      "--output",    result.path.string(),
      manifest.url};
  const ProcessResult process = run_process_capture("curl", args, cancel);
  if (process.canceled) {
    result.error_code = "UPDATE_DOWNLOAD_CANCELED";
    result.error_message = "已取消下载";
    std::filesystem::remove(result.path, error);
    log_info("更新下载已取消：" + result.path.filename().string());
    return result;
  }
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

std::filesystem::path current_executable_path() {
#if defined(_WIN32)
  std::vector<wchar_t> buffer(32768, L'\0');
  const DWORD length =
      ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) {
    return {};
  }
  return std::filesystem::path(std::wstring(buffer.data(), length));
#elif defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string buffer(size, '\0');
  if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
    return {};
  }
  std::error_code error;
  const std::filesystem::path canonical =
      std::filesystem::weakly_canonical(std::filesystem::path(buffer.c_str()), error);
  return error ? std::filesystem::path(buffer.c_str()) : canonical;
#else
  std::error_code error;
  const std::filesystem::path target = std::filesystem::read_symlink("/proc/self/exe", error);
  return error ? std::filesystem::path{} : target;
#endif
}

UpdatePackageResult stage_update_package(const UpdateSettings& settings,
                                         const std::string& manifest_file,
                                         const std::filesystem::path& package,
                                         const std::filesystem::path& staging_dir,
                                         const std::filesystem::path& executable) {
  UpdatePackageResult result;
  result.path = package;
  result.mode = "replace";

  const PackageVerifyResult verified = verify_package(settings, manifest_file, package.string());
  if (!verified.ok) {
    result.error_code = verified.error_code;
    result.error_message = verified.error_message;
    log_warn("安装前复验失败，未准备替换：" + verified.error_message);
    return result;
  }
  if (executable.empty()) {
    result.error_code = "UPDATE_APPLY_UNSUPPORTED";
    result.error_message = "无法确定当前程序路径，不能自动替换";
    return result;
  }

  const std::string extension = lower_extension(package);
  remove_old_staging(staging_dir);
  if (extension == ".zip") {
    std::error_code error;
    std::filesystem::remove_all(staging_dir, error);
    std::filesystem::create_directories(staging_dir, error);
    if (error) {
      result.error_code = "UPDATE_APPLY_FAILED";
      result.error_message = "无法创建解压目录：" + error.message();
      return result;
    }
    std::string detail;
    if (!extract_archive(package, staging_dir, &detail)) {
      result.error_code = "UPDATE_EXTRACT_FAILED";
      result.error_message = "解压安装包失败：" + detail;
      log_warn("更新解压失败：" + detail);
      return result;
    }
    const std::filesystem::path payload = find_payload(staging_dir, executable.filename().string());
    if (payload.empty()) {
      result.error_code = "UPDATE_PACKAGE_PAYLOAD_MISSING";
      result.error_message =
          "安装包内未找到与当前程序同名的可执行文件：" + executable.filename().string();
      log_warn("更新安装包缺少载荷：" + result.error_message);
      return result;
    }
    result.path = payload;
  } else if (extension.empty() || extension == ".exe" || extension == ".bin") {
    // 单文件包：下载文件名带版本前缀，脚本会复制成当前可执行文件名。
    result.path = package;
  } else {
    result.error_code = "UPDATE_PACKAGE_UNSUPPORTED";
    result.error_message =
        "自动替换只接受单文件可执行程序或 zip 包，当前安装包类型为 " + extension;
    log_warn("更新安装包类型不支持自动替换：" + package.string());
    return result;
  }

  result.ok = true;
  std::error_code size_error;
  result.bytes = std::filesystem::file_size(result.path, size_error);
  if (size_error) {
    result.bytes = 0;
  }
  return result;
}

std::string update_replace_script(const UpdateApplyOptions& options,
                                  const std::filesystem::path& new_executable) {
  if (options.executable.empty() || new_executable.empty()) {
    return {};
  }
  const long long pid = options.pid > 0 ? options.pid : current_process_id();
  const std::filesystem::path working_dir =
      options.working_dir.empty() ? options.executable.parent_path() : options.working_dir;
  const std::filesystem::path log_file = options.log_file.empty()
                                             ? working_dir / "update-apply.log"
                                             : options.log_file;

#if defined(_WIN32)
  const std::string executable = path_utf8(options.executable);
  const std::string replacement = path_utf8(new_executable);
  const std::string backup = executable + ".old";
  const std::string log = path_utf8(log_file);
  const std::string directory = path_utf8(working_dir);
  if (!windows_value_safe(executable) || !windows_value_safe(replacement) ||
      !windows_value_safe(log) || !windows_value_safe(directory)) {
    return {};
  }
  bool args_ok = true;
  const std::string args = join_windows_args(options.args, &args_ok);
  if (!args_ok) {
    return {};
  }
  std::string script;
  script += "@echo off\r\n";
  // 日志是 UTF-8，切到 65001 避免中文乱码。
  script += "chcp 65001 >nul\r\n";
  script += "set \"EXE=" + executable + "\"\r\n";
  script += "set \"OLD=" + backup + "\"\r\n";
  script += "set \"NEW=" + replacement + "\"\r\n";
  script += "set \"LOG=" + log + "\"\r\n";
  // 必须等旧进程真正退出后再替换：Windows 允许重命名正在运行的程序，
  // 只靠 move 是否成功判断会过早重启，造成两个服务同时监听同一端口。
  script += "set \"PID=" + std::to_string(pid) + "\"\r\n";
  script += "\"%SystemRoot%\\System32\\WindowsPowerShell\\v1.0\\powershell.exe\" -NoProfile"
            " -NonInteractive -WindowStyle Hidden -Command \"while (Get-Process -Id %PID%"
            " -ErrorAction SilentlyContinue) { Start-Sleep -Milliseconds 500 }\" >nul 2>&1\r\n";
  script += "if errorlevel 1 goto waitfailed\r\n";
  script += "if exist \"%OLD%\" del /f /q \"%OLD%\" >nul 2>&1\r\n";
  script += "move /y \"%EXE%\" \"%OLD%\" >nul 2>&1\r\n";
  script += "if errorlevel 1 goto failed\r\n";
  script += "copy /y \"%NEW%\" \"%EXE%\" >nul 2>&1\r\n";
  script += "if errorlevel 1 (\r\n";
  script += "  move /y \"%OLD%\" \"%EXE%\" >nul 2>&1\r\n";
  script += "  goto failed\r\n";
  script += ")\r\n";
  script += "del /f /q \"%OLD%\" >nul 2>&1\r\n";
  script += "cd /d \"" + directory + "\"\r\n";
  // 直接在当前脚本里拉起新程序：脚本会作为父进程一直等到服务退出。
  // 不用 start：脚本运行在无控制台的分离进程里，start /b 在部分环境下拉不起子进程。
  script += "\"%EXE%\"";
  if (!args.empty()) {
    script += " " + args;
  }
  script += "\r\n";
  script += "exit /b 0\r\n";
  script += ":waitfailed\r\n";
  script += ">>\"%LOG%\" echo [更新] 等待旧进程退出失败，未替换程序\r\n";
  script += "exit /b 1\r\n";
  script += ":failed\r\n";
  script += ">>\"%LOG%\" echo [更新] 替换失败：无法覆盖 %EXE%，已保留或恢复旧版本\r\n";
  script += "exit /b 1\r\n";
  return script;
#else
  bool args_ok = true;
  const std::string command_line = join_shell_args(options.args, &args_ok);
  if (!args_ok || !script_value_safe(path_utf8(options.executable)) ||
      !script_value_safe(path_utf8(working_dir)) || !script_value_safe(path_utf8(log_file))) {
    return {};
  }
  std::string script;
  script += "#!/bin/sh\n";
  script += "PID=" + std::to_string(pid) + "\n";
  script += "EXE=" + shell_quote(path_utf8(options.executable)) + "\n";
  script += "OLD=\"$EXE.old\"\n";
  script += "NEW=" + shell_quote(path_utf8(new_executable)) + "\n";
  script += "LOG=" + shell_quote(path_utf8(log_file)) + "\n";
  script += "TRIES=0\n";
  script += "while kill -0 \"$PID\" 2>/dev/null; do\n";
  script += "  TRIES=$((TRIES + 1))\n";
  script += "  if [ \"$TRIES\" -ge 120 ]; then\n";
  script += "    echo \"[更新] 替换超时：旧进程未退出\" >> \"$LOG\"\n";
  script += "    exit 1\n";
  script += "  fi\n";
  script += "  sleep 1\n";
  script += "done\n";
  script += "mv -f \"$EXE\" \"$OLD\" || { echo \"[更新] 无法备份旧版本\" >> \"$LOG\"; exit 1; }\n";
  script += "if ! cp -f \"$NEW\" \"$EXE\"; then\n";
  script += "  mv -f \"$OLD\" \"$EXE\"\n";
  script += "  echo \"[更新] 替换失败，已回滚旧版本\" >> \"$LOG\"\n";
  script += "  exit 1\n";
  script += "fi\n";
  script += "chmod 755 \"$EXE\" || true\n";
  script += "rm -f \"$OLD\"\n";
  script += "cd " + shell_quote(path_utf8(working_dir)) + " || true\n";
  script += "nohup \"$EXE\"";
  if (!command_line.empty()) {
    script += " " + command_line;
  }
  script += " >> \"$LOG\" 2>&1 &\n";
  script += "exit 0\n";
  return script;
#endif
}

UpdatePackageResult schedule_update_replace(const std::filesystem::path& new_executable,
                                            const UpdateApplyOptions& options,
                                            const std::filesystem::path& staging_dir) {
  UpdatePackageResult result;
  result.mode = "replace";
  result.path = new_executable;

  UpdateApplyOptions effective = options;
  if (effective.log_file.empty()) {
    effective.log_file = staging_dir / "update-apply.log";
  }
  const std::string script_text = update_replace_script(effective, new_executable);
  if (script_text.empty()) {
    result.error_code = "UPDATE_APPLY_UNSUPPORTED";
    result.error_message = "无法生成替换脚本：程序路径或启动参数包含不支持的字符";
    log_warn("更新替换脚本生成失败：" + result.error_message);
    return result;
  }

  std::error_code error;
  std::filesystem::create_directories(staging_dir, error);
  if (error) {
    result.error_code = "UPDATE_APPLY_FAILED";
    result.error_message = "无法创建替换目录：" + error.message();
    return result;
  }

#if defined(_WIN32)
  const std::filesystem::path script = staging_dir / "apply-update.cmd";
#else
  const std::filesystem::path script = staging_dir / "apply-update.sh";
#endif
  {
    std::ofstream out(script, std::ios::binary);
    out << script_text;
    if (!out) {
      result.error_code = "UPDATE_APPLY_FAILED";
      result.error_message = "无法写入替换脚本：" + script.string();
      return result;
    }
  }

  std::string launch_error;
#if defined(_WIN32)
  const bool launched = launch_detached(script, &launch_error);
#else
  std::error_code permission_error;
  std::filesystem::permissions(script,
                               std::filesystem::perms::owner_exec |
                                   std::filesystem::perms::group_exec |
                                   std::filesystem::perms::others_exec,
                               std::filesystem::perm_options::add, permission_error);
  const std::string command =
      "nohup /bin/sh " + shell_quote(script.string()) + " >/dev/null 2>&1 &";
  const ProcessResult process = run_process_capture("/bin/sh", {"-c", command});
  const bool launched = process.started && process.collected && process.exit_code == 0;
  if (!launched) {
    launch_error = package_command_error(process);
  }
#endif

  if (!launched) {
    result.error_code = "UPDATE_APPLY_FAILED";
    result.error_message = "无法启动替换脚本：" + launch_error;
    log_warn("更新替换脚本启动失败：" + result.error_message);
    return result;
  }

  result.ok = true;
  result.error_message = "已准备替换：服务将退出，随后自动替换程序并重启";
  log_info("更新：替换脚本已启动 " + script.string());
  return result;
}

}  // namespace xiaotu
