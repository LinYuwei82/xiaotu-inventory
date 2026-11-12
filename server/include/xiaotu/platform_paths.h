#pragma once

#include <filesystem>

namespace xiaotu::platform {

// 平台标识，用于日志与诊断。
const char* name();

// 应用数据目录。路径使用固定的应用标识，不随显示名变化：
//   macOS   ~/Library/Application Support/com.simem.xiaotu/
//   Windows %LOCALAPPDATA%/com.simem.xiaotu/；该变量缺失时依次退
//           %USERPROFILE%/AppData/Local/ 与 %PROGRAMDATA%/，三者皆无则报错
std::filesystem::path app_data_dir();

}  // namespace xiaotu::platform
