#include "xiaotu/platform_paths.h"

#include <cstdlib>
#include <string>

namespace xiaotu::platform {
namespace {

constexpr const char* kAppId = "com.simem.xiaotu";

std::filesystem::path home_dir() {
  if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
    return std::filesystem::path(home);
  }
  return std::filesystem::current_path();
}

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
  if (const char* local = std::getenv("LOCALAPPDATA"); local != nullptr && *local != '\0') {
    return std::filesystem::path(local) / kAppId;
  }
  return home_dir() / "AppData" / "Local" / kAppId;
#else
  return home_dir() / "Library" / "Application Support" / kAppId;
#endif
}

}  // namespace xiaotu::platform
