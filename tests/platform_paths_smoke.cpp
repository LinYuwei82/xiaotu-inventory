// 应用数据目录的兜底顺序测试：Windows 下环境变量缺失时按 LOCALAPPDATA →
// USERPROFILE → PROGRAMDATA 降级，三者皆无应报错而不是落到当前目录。
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "xiaotu/error.h"
#include "xiaotu/instance_lock.h"
#include "xiaotu/platform_paths.h"

namespace {

int g_failures = 0;

void check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << std::endl;
    ++g_failures;
  }
}

#if defined(_WIN32)
// 保存并在析构时恢复原值，避免用例之间互相污染。
class EnvGuard {
 public:
  explicit EnvGuard(const char* name) : name_(name) {
    const char* value = std::getenv(name);
    if (value != nullptr) {
      saved_ = value;
      had_value_ = true;
    }
  }

  ~EnvGuard() { _putenv_s(name_, had_value_ ? saved_.c_str() : ""); }

  static void set(const char* name, const std::filesystem::path& value) {
    _putenv_s(name, value.string().c_str());
  }

  static void unset(const char* name) { _putenv_s(name, ""); }

 private:
  const char* name_;
  std::string saved_;
  bool had_value_ = false;
};
#endif

}  // namespace

int main() {
  namespace fs = std::filesystem;
  namespace platform = xiaotu::platform;

#if defined(_WIN32)
  EnvGuard local_app_data("LOCALAPPDATA");
  EnvGuard user_profile("USERPROFILE");
  EnvGuard program_data("PROGRAMDATA");

  const fs::path root = fs::temp_directory_path() / "xiaotu-paths-smoke";
  const fs::path app_id = "com.simem.xiaotu";

  // 首选：LOCALAPPDATA，与既有默认一致。
  EnvGuard::set("LOCALAPPDATA", root / "local");
  EnvGuard::set("USERPROFILE", root / "profile");
  EnvGuard::set("PROGRAMDATA", root / "programdata");
  check(platform::app_data_dir() == root / "local" / app_id,
        "LOCALAPPDATA 存在时应作为首选");

  // 其次：USERPROFILE 下的本地应用数据。
  EnvGuard::unset("LOCALAPPDATA");
  check(platform::app_data_dir() == root / "profile" / "AppData" / "Local" / app_id,
        "缺少 LOCALAPPDATA 时应退到 USERPROFILE/AppData/Local");

  // 服务场景：没有用户目录时落机器级 PROGRAMDATA。
  EnvGuard::unset("USERPROFILE");
  check(platform::app_data_dir() == root / "programdata" / app_id,
        "没有用户目录时应退到 PROGRAMDATA");

  // 全部缺失：必须显式报错，不能静默退回当前目录。
  EnvGuard::unset("PROGRAMDATA");
  bool rejected = false;
  try {
    (void)platform::app_data_dir();
  } catch (const xiaotu::AppError& error) {
    rejected = error.code() == "DATA_DIR_UNAVAILABLE";
  }
  check(rejected, "三个环境变量都缺失时应抛 DATA_DIR_UNAVAILABLE");
#else
  // 非 Windows 只校验目录形状，避免依赖运行测试的机器主目录。
  const fs::path dir = platform::app_data_dir();
  check(dir.filename() == "com.simem.xiaotu", "应用数据目录应使用固定应用标识");
  check(dir.parent_path().filename() == "Application Support",
        "macOS 应用数据目录应在 Application Support 下");
#endif

  // 同步目录识别：iCloud 与 OneDrive 路径应命中，普通目录不应误报。
  const fs::path sync_root = fs::temp_directory_path() / "xiaotu-paths-smoke";
  check(platform::sync_service_for(sync_root / "Library" / "Mobile Documents" /
                                   "com~apple~CloudDocs" / "data") == "iCloud Drive",
        "iCloud Drive 路径应被识别");
  check(platform::sync_service_for(sync_root / "OneDrive" / "xiaotu") == "OneDrive",
        "OneDrive 路径应被识别");
  check(platform::sync_service_for(sync_root / "localdata").empty(),
        "普通目录不应误报同步目录");

  // 单实例锁：同一目录第二次获取应失败，持有者释放后可重新获取。
  const fs::path lock_dir = fs::temp_directory_path() / "xiaotu-lock-smoke";
  fs::create_directories(lock_dir);
  {
    xiaotu::InstanceLock first;
    check(first.acquire(lock_dir), "首次获取单实例锁应成功");
    xiaotu::InstanceLock second;
    check(!second.acquire(lock_dir), "同一目录重复获取单实例锁应失败");
    check(!second.holder_pid().empty(), "获取失败时应能读到持有者 PID");
  }
  {
    xiaotu::InstanceLock again;
    check(again.acquire(lock_dir), "释放后应能重新获取单实例锁");
  }
  {
    std::error_code ignored;
    fs::remove_all(lock_dir, ignored);
  }

  if (g_failures != 0) {
    std::cerr << g_failures << " 项断言失败" << std::endl;
    return 1;
  }
  std::cout << "应用数据目录兜底顺序：全部通过" << std::endl;
  return 0;
}
