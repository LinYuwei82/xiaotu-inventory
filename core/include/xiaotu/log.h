#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace xiaotu {

// 运行日志（与数据库里的审计日志是两回事）：
//   - 单行纯文本，`时间 级别 [线程] 消息`，便于 grep 与日志采集；
//   - 写入 <目录>/app.log，超过上限按 app.log.1、app.log.2 滚动；
//   - 线程安全，默认输出 Info 及以上；
//   - 禁止记录访问凭据、口令、Cookie、请求体等敏感内容；
//   - 未初始化时退回标准错误，保证命令行启动失败仍可见。
enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

struct LogOptions {
  std::filesystem::path directory;                 // 为空则只写标准错误
  LogLevel level = LogLevel::Info;
  std::int64_t max_bytes = 5 * 1024 * 1024;        // 单个文件上限
  int keep_files = 3;                              // 保留 app.log + 3 个滚动文件
};

void init_logging(const LogOptions& options);
void shutdown_logging();

void log_message(LogLevel level, std::string_view message);

inline void log_debug(std::string_view message) { log_message(LogLevel::Debug, message); }
inline void log_info(std::string_view message) { log_message(LogLevel::Info, message); }
inline void log_warn(std::string_view message) { log_message(LogLevel::Warn, message); }
inline void log_error(std::string_view message) { log_message(LogLevel::Error, message); }

// 无法识别时返回 fallback。
LogLevel parse_log_level(std::string_view text, LogLevel fallback = LogLevel::Info);
const char* log_level_name(LogLevel level);

}  // namespace xiaotu
