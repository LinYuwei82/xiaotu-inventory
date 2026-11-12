#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace xiaotu {

// config.toml 中出现的配置项。未出现的键保持为空，交由调用方按
// 「命令行 > 环境变量 > 配置文件 > 内置默认」的顺序合并。
struct ConfigValues {
  std::optional<std::string> listen;    // [server] listen
  std::optional<int> port;              // [server] port
  std::optional<std::string> data_dir;  // [storage] data_dir
  std::optional<std::string> log_level; // [log] level
  std::optional<std::string> log_dir;   // [log] dir
  // [sales] credit_limit_mode：warn（默认，提示并二次确认后放行）/ reject（硬拦）。
  std::optional<std::string> credit_limit_mode;
};

// 读取 config.toml。只支持本程序需要的 TOML 子集：
//   - 段：[server]、[storage]、[log]
//   - 键值：`key = "字符串" | 整数 | true/false`
//   - 注释：整行或以 # 起始的行尾注释（字符串内的 # 不截断）
// 未知段、未知键、重复键、类型不符一律报错，避免拼错后静默失效。
// 文件不存在或读不了抛 AppError。
ConfigValues load_config_file(const std::filesystem::path& path);

}  // namespace xiaotu
