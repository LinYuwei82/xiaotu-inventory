#include "xiaotu/apply_update.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <vector>

#include "xiaotu/platform.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace xiaotu {
namespace {

using nlohmann::json;

std::string now_iso_local() {
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &now);
#else
  localtime_r(&now, &tm);
#endif
  char buffer[32] = {};
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &tm);
  return buffer;
}

// 跨平台原子写：先写 .tmp 再替换。Windows 的 rename 不覆盖已存在文件，
// 所以失败时删掉目标再试一次——结果文件允许「后写覆盖先写」。
bool write_json_file(const std::filesystem::path& target, const json& doc,
                     std::string* error) {
  std::error_code ec;
  std::filesystem::create_directories(target.parent_path(), ec);
  std::filesystem::path temp = target;
  temp += ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) {
      if (error) *error = "无法写入 " + path_to_utf8(temp);
      return false;
    }
    out << doc.dump(2);
    out.flush();
    if (!out) {
      if (error) *error = "写入失败 " + path_to_utf8(temp);
      std::filesystem::remove(temp, ec);
      return false;
    }
  }
  std::filesystem::rename(temp, target, ec);
  if (ec) {
    std::error_code ignored;
    std::filesystem::remove(target, ignored);
    std::filesystem::rename(temp, target, ec);
  }
  if (ec) {
    if (error) *error = "无法替换 " + path_to_utf8(target) + "：" + ec.message();
    std::filesystem::remove(temp, ec);
    return false;
  }
  return true;
}

std::filesystem::path apply_state_path(const std::filesystem::path& data_dir) {
  return data_dir / "updates" / "apply-state.json";
}

#ifdef _WIN32

std::wstring widen(const std::string& value) {
  if (value.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                       static_cast<int>(value.size()), nullptr, 0);
  if (size <= 0) return {};
  std::wstring out(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                      out.data(), size);
  return out;
}

std::string narrow(const std::wstring& value) {
  if (value.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                       static_cast<int>(value.size()), nullptr, 0,
                                       nullptr, nullptr);
  if (size <= 0) return {};
  std::string out(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                      out.data(), size, nullptr, nullptr);
  return out;
}

// Inno/命令行参数引号规则：反斜杠只在引号前成对翻倍。
std::wstring quote_arg(const std::wstring& value) {
  if (value.find_first_of(L" \t\n\v\"") == std::wstring::npos) return value;
  std::wstring out = L"\"";
  std::size_t backslashes = 0;
  for (wchar_t c : value) {
    if (c == L'\\') {
      ++backslashes;
    } else if (c == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
      out.push_back(L'"');
      backslashes = 0;
    } else {
      out.append(backslashes, L'\\');
      backslashes = 0;
      out.push_back(c);
    }
  }
  out.append(backslashes * 2, L'\\');
  out.push_back(L'"');
  return out;
}

std::string last_error_text() {
  const DWORD code = GetLastError();
  return "Win32 错误 " + std::to_string(static_cast<unsigned long>(code));
}

std::string now_iso_utc() {
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
  gmtime_s(&tm, &now);
  char buffer[32] = {};
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", tm);
  return buffer;
}

struct RunResult {
  bool started = false;
  int exit_code = -1;
  std::string error;
};

// 跑一个进程并等它结束。SW_HIDE：协助器本身已被壳以隐藏窗口启动，
// 它拉起的安装器/解压工具同样不该闪窗。
RunResult run_and_wait(const std::filesystem::path& program,
                       const std::wstring& command_line,
                       const std::filesystem::path& working_dir) {
  RunResult result;
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION process{};
  std::vector<wchar_t> buffer(command_line.begin(), command_line.end());
  buffer.push_back(L'\0');
  const std::wstring cwd = working_dir.wstring();
  // 不用 DETACHED_PROCESS / CREATE_NO_WINDOW：devcard 实测这两个标志会让
  // Inno Setup 子进程无限等待。默认标志继承协助器自己的控制台即可。
  if (!CreateProcessW(program.c_str(), buffer.data(), nullptr, nullptr, FALSE, 0,
                      nullptr, cwd.c_str(), &startup, &process)) {
    result.error = "无法启动 " + path_to_utf8(program) + "：" + last_error_text();
    return result;
  }
  result.started = true;
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  result.exit_code = static_cast<int>(code);
  return result;
}

// 绿色包替换：staging 解压 → 定位载荷 → 整目录备份 → 覆盖 → 验证 → 回滚。
// 返回退出码语义与安装器分支一致：0 成功；非 0 时 error 说明卡在哪一步。
int apply_zip_update(const ApplyUpdateOptions& options, const std::filesystem::path& app_dir,
                     std::string* error) {
  std::error_code ec;
  const std::filesystem::path staging =
      options.package.parent_path() / ("apply-staging-" + std::to_string(::GetCurrentProcessId()));
  std::filesystem::remove_all(staging, ec);
  std::filesystem::create_directories(staging, ec);
  if (ec) {
    *error = "无法创建解压目录：" + ec.message();
    return 1;
  }
  // Win10 1803+ 自带 bsdtar，能解 zip；失败即报错，不做多级回落
  // （绿色包由同一发布链产出，zip 格式可控）。
  const RunResult tar = run_and_wait(
      L"C:\\Windows\\System32\\tar.exe",
      L"-xf " + quote_arg(options.package.wstring()) + L" -C " + quote_arg(staging.wstring()),
      staging.parent_path());
  if (!tar.started) {
    *error = tar.error;
    return 1;
  }
  if (tar.exit_code != 0) {
    *error = "tar 解压失败，退出码 " + std::to_string(tar.exit_code);
    return 1;
  }
  // 定位载荷：zip 内与壳同名的 exe（可能在根或一层子目录里）。
  std::filesystem::path payload;
  const std::wstring shell_name = L"xiaotu-shell.exe";
  for (std::filesystem::recursive_directory_iterator it(staging, ec), end;
       !ec && it != end; it.increment(ec)) {
    if (it->is_regular_file(ec) && _wcsicmp(it->path().filename().c_str(), shell_name.c_str()) == 0) {
      payload = it->path();
      break;
    }
  }
  if (payload.empty()) {
    *error = "解压包里找不到 xiaotu-shell.exe";
    return 1;
  }
  const std::filesystem::path new_root = payload.parent_path();
  // 整目录备份 → 清空 → 拷入。协助器自身在数据目录副本里跑，不受影响。
  const std::filesystem::path backup =
      app_dir.parent_path() / (app_dir.filename().string() + ".bak-" + std::to_string(::GetCurrentProcessId()));
  std::filesystem::remove_all(backup, ec);
  if (!std::filesystem::rename(app_dir, backup, ec)) {
    *error = "备份旧目录失败（可能被占用）：" + ec.message();
    return 1;
  }
  bool copied = false;
  std::filesystem::create_directories(app_dir, ec);
  if (!ec) {
    copied = true;
    for (std::filesystem::recursive_directory_iterator it(new_root, ec), end;
         !ec && it != end; it.increment(ec)) {
      const std::filesystem::path rel = std::filesystem::relative(it->path(), new_root, ec);
      if (ec) {
        copied = false;
        break;
      }
      const std::filesystem::path dest = app_dir / rel;
      if (it->is_directory(ec)) {
        std::filesystem::create_directories(dest, ec);
        if (ec) {
          copied = false;
          break;
        }
      } else if (it->is_regular_file(ec)) {
        std::filesystem::create_directories(dest.parent_path(), ec);
        std::filesystem::copy_file(it->path(), dest,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
          copied = false;
          break;
        }
      }
    }
  }
  if (!copied) {
    // 回滚：删掉半成品，把备份搬回去。
    std::error_code ignored;
    std::filesystem::remove_all(app_dir, ignored);
    if (!std::filesystem::rename(backup, app_dir, ignored)) {
      *error = "覆盖失败且回滚失败，旧版本目录在 " + path_to_utf8(backup);
      return 2;
    }
    *error = "拷贝新文件失败：" + ec.message();
    return 1;
  }
  std::filesystem::remove_all(backup, ec);  // 成功则清掉备份，失败不影响结果
  return 0;
}

#endif  // _WIN32

}  // namespace

std::string read_product_version(const std::filesystem::path& exe) {
#ifdef _WIN32
  if (exe.empty()) return {};
  const std::wstring file = exe.wstring();
  DWORD unused = 0;
  const DWORD size = GetFileVersionInfoSizeW(file.c_str(), &unused);
  if (size == 0) return {};
  std::vector<unsigned char> data(size);
  if (!GetFileVersionInfoW(file.c_str(), 0, size, data.data())) return {};
  void* pointer = nullptr;
  UINT length = 0;
  // Translation 固定 0x0409/1200，与 version.rc 一致。
  if (!VerQueryValueW(data.data(), L"\\StringFileInfo\\040904b0\\ProductVersion",
                      &pointer, &length) ||
      !pointer || length == 0) {
    return {};
  }
  const wchar_t* text = static_cast<const wchar_t*>(pointer);
  return narrow(std::wstring(text, wcsnlen(text, length)));
#else
  (void)exe;
  return {};
#endif
}

bool spawn_apply_update(const std::filesystem::path& helper,
                        const ApplyUpdateOptions& options,
                        std::string* err) {
#ifdef _WIN32
  std::wstring command = quote_arg(helper.wstring());
  command += L" --apply-update " + quote_arg(options.package.wstring());
  command += L" --wait-pid " + std::to_wstring(options.wait_pid);
  command += L" --result " + quote_arg(options.result.wstring());
  if (!options.relaunch.empty()) {
    command += L" --relaunch " + quote_arg(options.relaunch.wstring());
    for (const std::string& arg : options.relaunch_args) {
      command += L" --relaunch-arg " + quote_arg(widen(arg));
    }
  }
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION process{};
  std::vector<wchar_t> buffer(command.begin(), command.end());
  buffer.push_back(L'\0');
  const std::wstring cwd = helper.parent_path().wstring();
  if (!CreateProcessW(helper.c_str(), buffer.data(), nullptr, nullptr, FALSE, 0, nullptr,
                      cwd.c_str(), &startup, &process)) {
    if (err) *err = "无法启动更新协助程序：" + last_error_text();
    return false;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
#else
  (void)helper;
  (void)options;
  if (err) *err = "协助更新仅支持 Windows";
  return false;
#endif
}

int run_apply_update(const ApplyUpdateOptions& options) {
#ifndef _WIN32
  (void)options;
  std::cerr << "协助更新仅支持 Windows" << std::endl;
  return 1;
#else
  // 1) 等壳退出：壳经 Job Object 带走 server 进程树，等到壳即等到全部。
  if (options.wait_pid != 0) {
    HANDLE target = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(options.wait_pid));
    if (target) {
      WaitForSingleObject(target, 120000);
      CloseHandle(target);
    }
  }

  const std::filesystem::path app_dir = options.relaunch.parent_path();
  const std::filesystem::path installed_server = app_dir / L"xiaotu-server.exe";
  const std::string version_before = read_product_version(installed_server);

  json doc = {{"package", path_to_utf8(options.package)},
              {"installed_server", path_to_utf8(installed_server)},
              {"version_before", version_before},
              {"version_after", ""},
              {"exit_code", -1},
              {"error", ""},
              {"finished_at", ""}};

  std::error_code ec;
  // data_dir = result.parent_path().parent_path()（result 在 <data>/updates/ 下）。
  const std::filesystem::path data_dir = options.result.parent_path().parent_path();
  // 提前设好环境变量：安装版分支的 setup.exe 会继承它，Inno [Run] 的
  // RestartAfterSilent 检测到即返回 False，避免安装器与协助器双重启壳。
  SetEnvironmentVariableW(L"XIAOTU_APPLY_HELPER", L"1");
  std::string state_error;
  write_apply_state(data_dir, ApplyState::Verifying, "协助器已接管，正在执行安装",
                    &state_error);

  std::string package_ext = options.package.extension().string();
  for (auto& c : package_ext) c = static_cast<char>(std::tolower(c));
  if (options.package.empty() || !std::filesystem::exists(options.package, ec)) {
    doc["error"] = "安装包不存在：" + path_to_utf8(options.package);
  } else if (package_ext == ".zip") {
    // 绿色版：解压替换自身目录。
    std::string zip_error;
    const int rc = apply_zip_update(options, app_dir, &zip_error);
    doc["exit_code"] = rc;
    if (rc != 0) doc["error"] = zip_error;
    doc["version_after"] = read_product_version(installed_server);
  } else {
    // 安装版：Inno 静默安装。/CLOSEAPPLICATIONS 保留作兜底（壳已优雅退出），
    // /LOG 留过程日志，失败时能在 install.log 里看出卡在哪一步。
    const std::filesystem::path log =
        options.package.parent_path() / L"inno-install.log";
    std::wstring command = quote_arg(options.package.wstring());
    command += L" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS";
    command += L" /LOG=" + quote_arg(log.wstring());
    const RunResult run =
        run_and_wait(options.package, command, options.package.parent_path());
    if (!run.started) {
      doc["error"] = run.error;
    } else {
      doc["exit_code"] = run.exit_code;
      doc["version_after"] = read_product_version(installed_server);
    }
  }
  doc["finished_at"] = now_iso_utc();

  const int exit_code = doc["exit_code"].get<int>();
  const bool succeeded = exit_code == 0 && doc["error"].get<std::string>().empty();
  write_apply_state(data_dir, succeeded ? ApplyState::Done : ApplyState::Failed,
                    succeeded ? "安装器退出码 0"
                              : "退出码 " + std::to_string(exit_code),
                    &state_error);

  std::string write_error;
  if (!write_json_file(options.result, doc, &write_error)) {
    std::cerr << "警告：无法写入安装结果 " << path_to_utf8(options.result) << std::endl;
  }

  // 无论成败都拉起目标程序：失败时拉起的是旧版本（文件没被替换）。
  if (!options.relaunch.empty() && std::filesystem::exists(options.relaunch, ec)) {
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring command_line = quote_arg(options.relaunch.wstring());
    for (const std::string& arg : options.relaunch_args) {
      command_line += L" " + quote_arg(widen(arg));
    }
    std::vector<wchar_t> buffer(command_line.begin(), command_line.end());
    buffer.push_back(L'\0');
    const std::wstring cwd = options.relaunch.parent_path().wstring();
    if (CreateProcessW(options.relaunch.c_str(), buffer.data(), nullptr, nullptr, FALSE, 0,
                       nullptr, cwd.c_str(), &startup, &process)) {
      CloseHandle(process.hThread);
      CloseHandle(process.hProcess);
    }
  }
  return exit_code == 0 ? 0 : 1;
#endif
}

const char* apply_state_name(ApplyState state) {
  switch (state) {
    case ApplyState::Installing:
      return "installing";
    case ApplyState::Verifying:
      return "verifying";
    case ApplyState::Done:
      return "done";
    case ApplyState::Failed:
      return "failed";
  }
  return "failed";
}

bool write_apply_state(const std::filesystem::path& data_dir, ApplyState state,
                       const std::string& detail, std::string* error) {
  const json doc = {{"state", apply_state_name(state)},
                    {"detail", detail},
                    {"updated_at", now_iso_local()}};
  return write_json_file(apply_state_path(data_dir), doc, error);
}

ApplyStateSnapshot read_apply_state(const std::filesystem::path& data_dir) {
  ApplyStateSnapshot snapshot;
  const std::filesystem::path file = apply_state_path(data_dir);
  std::error_code ec;
  if (!std::filesystem::is_regular_file(file, ec)) return snapshot;
  snapshot.present = true;
  const auto write_time = std::filesystem::last_write_time(file, ec);
  if (!ec) {
    snapshot.age_seconds =
        std::chrono::duration_cast<std::chrono::seconds>(
            std::filesystem::file_time_type::clock::now() - write_time)
            .count();
  }
  std::ifstream in(file, std::ios::binary);
  if (!in) return snapshot;
  const std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  const json doc = json::parse(text, nullptr, false);
  if (doc.is_discarded() || !doc.is_object()) {
    snapshot.state = "unknown";
    return snapshot;
  }
  snapshot.state = doc.value("state", std::string{});
  snapshot.detail = doc.value("detail", std::string{});
  snapshot.updated_at = doc.value("updated_at", std::string{});
  return snapshot;
}

bool apply_state_interrupted(const ApplyStateSnapshot& snapshot, int threshold_seconds) {
  return snapshot.present && snapshot.state == "installing" &&
         snapshot.age_seconds > threshold_seconds;
}

}  // namespace xiaotu
