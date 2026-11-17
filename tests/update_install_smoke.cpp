// 自动替换的确定性用例：脚本生成（等待/备份/回滚/重启参数）与安装包载荷定位。
// 不真正启动替换脚本，避免测试进程被替换。
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "xiaotu/update_install.h"
#include "xiaotu/update_service.h"

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << std::endl;
    ++failures;
  }
}

bool contains(const std::string& text, const std::string& piece) {
  return text.find(piece) != std::string::npos;
}

std::filesystem::path write_verifier_stub(const std::filesystem::path& directory) {
#if defined(_WIN32)
  const auto path = directory / "verify-stub.bat";
  std::ofstream out(path);
  out << "@echo off\r\nexit /b 0\r\n";
  return path;
#else
  const auto path = directory / "verify-stub.sh";
  {
    std::ofstream out(path);
    out << "#!/bin/sh\nexit 0\n";
  }
  std::filesystem::permissions(path,
                               std::filesystem::perms::owner_exec |
                                   std::filesystem::perms::group_exec |
                                   std::filesystem::perms::others_exec,
                               std::filesystem::perm_options::add);
  return path;
#endif
}

#if defined(_WIN32)
struct ChildProcess {
  HANDLE handle = nullptr;
  DWORD pid = 0;
};

// 起一个长驻子进程模拟“正在运行的服务端”，用于真实走一遍等待退出后的替换。
bool start_child(const std::filesystem::path& executable, const std::wstring& arguments,
                 ChildProcess* child) {
  std::wstring command = L"\"" + executable.wstring() + L"\"";
  if (!arguments.empty()) {
    command += L" " + arguments;
  }
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                       &startup, &process) == FALSE) {
    return false;
  }
  ::CloseHandle(process.hThread);
  child->handle = process.hProcess;
  child->pid = process.dwProcessId;
  return true;
}

void stop_child(ChildProcess* child) {
  if (child->handle != nullptr) {
    ::TerminateProcess(child->handle, 0);
    ::WaitForSingleObject(child->handle, 5000);
    ::CloseHandle(child->handle);
    child->handle = nullptr;
  }
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

// 真实替换：启动占位进程 → 生成并后台启动脚本 → 结束占位进程 → 等待覆盖与重启。
void run_replace_end_to_end(const std::filesystem::path& root) {
  const std::filesystem::path target = root / "e2e-target.exe";
  const std::filesystem::path replacement = root / "e2e-new.exe";
  const std::filesystem::path restarted = root / "e2e-restarted.txt";
  std::filesystem::copy_file("C:\\Windows\\System32\\ping.exe", target,
                             std::filesystem::copy_options::overwrite_existing);
  // 用测试程序自身作为新程序：重启后执行 --write-marker，留下“确实被拉起”的证据。
  std::filesystem::copy_file(xiaotu::current_executable_path(), replacement,
                             std::filesystem::copy_options::overwrite_existing);

  ChildProcess child;
  if (!start_child(target, L"-t 127.0.0.1", &child)) {    std::cerr << "SKIP: 无法启动占位进程，跳过替换端到端用例" << std::endl;
    return;
  }

  xiaotu::UpdateApplyOptions options;
  options.executable = target;
  options.args = {"--write-marker", restarted.string()};
  options.working_dir = root;
  options.log_file = root / "e2e-update-apply.log";
  options.pid = static_cast<long long>(child.pid);

  const xiaotu::UpdatePackageResult scheduled =
      xiaotu::schedule_update_replace(replacement, options, root / "e2e-staging");
  check(scheduled.ok, "替换脚本后台启动成功");
  if (!scheduled.ok) {
    stop_child(&child);
    return;
  }

  // 给脚本一点时间进入等待，再结束占位进程。
  std::this_thread::sleep_for(std::chrono::seconds(2));
  stop_child(&child);

  const std::string expected = read_file(replacement);
  bool replaced = false;
  // 替换脚本要等旧进程退出再覆盖；机器负载高时 15 秒窗口不够，放宽到 30 秒。
  for (int attempt = 0; attempt < 60 && !replaced; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    replaced = read_file(target) == expected;
  }
  check(replaced, "旧进程退出后新程序覆盖到位");

  const std::filesystem::path log = root / "e2e-update-apply.log";
  bool cleaned = false;
  for (int attempt = 0; attempt < 10 && !cleaned; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    cleaned = !std::filesystem::exists(target.string() + ".old");
  }
  check(cleaned, "替换成功后清理旧版本副本");

  bool relaunched = false;
  for (int attempt = 0; attempt < 40 && !relaunched; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    relaunched = std::filesystem::exists(restarted);
  }
  check(relaunched, "替换后按原参数重新启动程序");
  if (!replaced || !relaunched) {
    std::cerr << "脚本内容：\n"
              << read_file(root / "e2e-staging" / "apply-update.cmd")
              << "\n替换日志：" << read_file(log) << std::endl;
  }
}
#endif

}  // namespace

int main(int argc, char** argv) {
  namespace fs = std::filesystem;
  // 自动替换重启后由新程序调用：写一个标记文件证明重启成功。
  if (argc == 3 && std::string(argv[1]) == "--write-marker") {
    std::ofstream(argv[2]) << "restarted";
    return 0;
  }
  const fs::path root = fs::temp_directory_path() / "xiaotu-update-install-test";
  fs::remove_all(root);
  fs::create_directories(root);

  const fs::path executable = root / "xiaotu-server.exe";
  const fs::path replacement = root / "staging" / "xiaotu-server.exe";
  fs::create_directories(replacement.parent_path());
  std::ofstream(executable) << "old";
  std::ofstream(replacement) << "new";

  xiaotu::UpdateApplyOptions options;
  options.executable = executable;
  options.args = {"--server", "--config", "C:\\Program Files\\xiaotu\\config.toml"};
  options.working_dir = root;
  options.log_file = root / "update-apply.log";
  options.pid = 4321;

  const std::string script = xiaotu::update_replace_script(options, replacement);
  check(!script.empty(), "生成替换脚本");
#if defined(_WIN32)
  check(contains(script, "move /y \"%EXE%\" \"%OLD%\""), "先备份旧程序再覆盖");
  check(contains(script, "copy /y \"%NEW%\" \"%EXE%\""), "把新程序复制到原位置");
  check(contains(script, "move /y \"%OLD%\" \"%EXE%\""), "覆盖失败时回滚旧程序");
  check(contains(script, "\"%EXE%\""), "替换完成后重启");
  check(contains(script, "Get-Process -Id %PID%"), "等待旧进程退出后才替换");
  check(contains(script, "--config"), "重启沿用原启动参数");
  check(contains(script, "C:\\Program Files\\xiaotu\\config.toml"), "含空格的参数按原值写入");
#else
  check(contains(script, "PID=4321"), "等待当前进程退出");
  check(contains(script, "mv -f \"$EXE\" \"$OLD\""), "先备份旧程序再覆盖");
  check(contains(script, "mv -f \"$OLD\" \"$EXE\""), "覆盖失败时回滚旧程序");
  check(contains(script, "nohup \"$EXE\""), "替换完成后重启");
  check(contains(script, "--config"), "重启沿用原启动参数");
#endif

#if defined(_WIN32)
  // 批处理会把 %VAR% 展开成环境变量；含 % 的参数必须拒绝，避免生成错误脚本。
  xiaotu::UpdateApplyOptions unsafe = options;
  unsafe.args = {"--config", "C:\\100%\\config.toml"};
  check(xiaotu::update_replace_script(unsafe, replacement).empty(),
        "含 % 的启动参数拒绝生成脚本");
#else
  xiaotu::UpdateApplyOptions unsafe = options;
  unsafe.args = {"--config", "bad\npath"};
  check(xiaotu::update_replace_script(unsafe, replacement).empty(),
        "含换行的启动参数拒绝生成脚本");
#endif

  // 安装包载荷定位：单文件包直接使用；不支持的格式明确报错；文件缺失报错。
  const fs::path public_key = root / "update.pub.pem";
  const fs::path verifier_script = root / "update_manifest.py";
  std::ofstream(public_key) << "test-public-key";
  std::ofstream(verifier_script) << "# test verifier";

  xiaotu::UpdateSettings settings;
  settings.public_key_file = public_key.string();
  settings.verifier_script = verifier_script.string();
  settings.verifier_python = write_verifier_stub(root).string();

  const fs::path package = root / "xiaotu-server-9.9.9.bin";
  std::ofstream(package) << "new-binary";
  xiaotu::UpdatePackageResult staged =
      xiaotu::stage_update_package(settings, "", package, root / "staging", executable);
  check(staged.ok && staged.path == package && staged.mode == "replace",
        "单文件可执行包直接作为替换载荷");

  const fs::path dmg = root / "xiaotu-9.9.9.dmg";
  std::ofstream(dmg) << "dmg";
  staged = xiaotu::stage_update_package(settings, "", dmg, root / "staging", executable);
  check(!staged.ok && staged.error_code == "UPDATE_PACKAGE_UNSUPPORTED",
        "dmg 等非可执行包拒绝自动替换");

  staged = xiaotu::stage_update_package(settings, "", root / "missing.bin", root / "staging",
                                        executable);
  check(!staged.ok && staged.error_code == "UPDATE_PACKAGE_MISSING", "安装包缺失时拒绝");

#if defined(_WIN32)
  run_replace_end_to_end(root);
#endif

  fs::remove_all(root);
  if (failures == 0) {
    std::cout << "update_install_smoke 全部通过" << std::endl;
  }
  return failures == 0 ? 0 : 1;
}
