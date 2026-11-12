#pragma once

#include <filesystem>
#include <string>

namespace xiaotu::platform {

// 平台标识，用于日志与诊断。
const char* name();

// 应用数据目录。路径使用固定的应用标识，不随显示名变化：
//   macOS   ~/Library/Application Support/com.simem.xiaotu/
//   Windows %LOCALAPPDATA%/com.simem.xiaotu/；该变量缺失时依次退
//           %USERPROFILE%/AppData/Local/ 与 %PROGRAMDATA%/，三者皆无则报错
std::filesystem::path app_data_dir();

// 数据目录若位于云同步目录（iCloud Drive / OneDrive），返回同步服务名；
// 否则返回空串。SQLite 的锁在同步目录上不可靠，启动时需要向用户告警。
std::string sync_service_for(const std::filesystem::path& dir);

}  // namespace xiaotu::platform
