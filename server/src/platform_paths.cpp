#include "xiaotu/platform_paths.h"

#include <cstdlib>
#include <string>

#include "xiaotu/error.h"

namespace xiaotu::platform {
namespace {

constexpr const char* kAppId = "com.simem.xiaotu";

#if !defined(_WIN32)
std::filesystem::path home_dir() {
  if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
    return std::filesystem::path(home);
  }
  return std::filesystem::current_path();
}
#else
// 读取环境变量给出的目录；未设置或为空时返回空路径。
std::filesystem::path env_dir(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    return std::filesystem::path{};
  }
  return std::filesystem::path(value);
}
#endif

}  // namespace

const char* name() {
#if defined(__APPLE__)
  return "macOS";
#elif defined(_WIN32)
  return "Windows";
#else
  return "unknown";
#endif
}

std::filesystem::path app_data_dir() {
#if defined(_WIN32)
  // 顺序依据：
  //   1) LOCALAPPDATA 是既有默认，已安装用户的数据目录必须保持不变；
  //   2) 环境被精简掉 LOCALAPPDATA 时，用户的本地应用数据仍在 USERPROFILE 下；
  //   3) 服务 / LocalSystem 没有用户目录，机器级数据落 PROGRAMDATA；
  //   4) 三者皆无说明运行环境不完整，直接报错，不再静默退回当前目录
  //      （安装目录可能是只读的 Program Files，散落的数据也无从预期）。
  if (const std::filesystem::path local = env_dir("LOCALAPPDATA"); !local.empty()) {
    return local / kAppId;
  }
  if (const std::filesystem::path profile = env_dir("USERPROFILE"); !profile.empty()) {
    return profile / "AppData" / "Local" / kAppId;
  }
  if (const std::filesystem::path program_data = env_dir("PROGRAMDATA"); !program_data.empty()) {
    return program_data / kAppId;
  }
  throw AppError("DATA_DIR_UNAVAILABLE",
                 "无法确定应用数据目录：LOCALAPPDATA、USERPROFILE、PROGRAMDATA 均未设置，"
                 "请通过 --data 指定数据目录");
#else
  return home_dir() / "Library" / "Application Support" / kAppId;
#endif
}

}  // namespace xiaotu::platform
