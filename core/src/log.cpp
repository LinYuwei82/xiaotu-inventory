#include "xiaotu/log.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include "xiaotu/platform.h"

namespace xiaotu {
namespace {

struct LogState {
  std::filesystem::path directory;
  std::string file_name = "app.log";
  LogLevel level = LogLevel::Info;
  std::int64_t max_bytes = 5 * 1024 * 1024;
  int keep_files = 3;
  bool active = false;
};

std::mutex g_mutex;
LogState g_state;

std::string timestamp_now() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
  const auto millis =
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() %
      1000;
  const std::tm local = local_time(seconds);
  char buffer[32] = {};
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local);
  char suffix[8] = {};
  std::snprintf(suffix, sizeof(suffix), ".%03d", static_cast<int>(millis));
  return std::string(buffer) + suffix;
}

std::string thread_tag() {
  const auto id = std::hash<std::thread::id>{}(std::this_thread::get_id());
  return std::to_string(static_cast<unsigned long long>(id) & 0xFFFFU);
}

std::string format_line(LogLevel level, std::string_view message) {
  return timestamp_now() + " [" + log_level_name(level) + "] [tid:" + thread_tag() + "] " +
         std::string(message) + "\n";
}

void rotate_locked() {
  const std::filesystem::path base = g_state.directory / g_state.file_name;
  std::error_code error;
  if (!std::filesystem::exists(base, error)) {
    return;
  }
  for (int index = g_state.keep_files; index >= 1; --index) {
    const std::filesystem::path source =
        index == 1 ? base
                   : g_state.directory / (g_state.file_name + "." + std::to_string(index - 1));
    const std::filesystem::path target =
        g_state.directory / (g_state.file_name + "." + std::to_string(index));
    if (!std::filesystem::exists(source, error)) {
      continue;
    }
    if (std::filesystem::exists(target, error)) {
      std::filesystem::remove(target, error);
    }
    std::filesystem::rename(source, target, error);
  }
}

void write_locked(const std::string& line) {
  if (!g_state.active || g_state.directory.empty()) {
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);
    return;
  }
  const std::filesystem::path path = g_state.directory / g_state.file_name;
  std::error_code error;
  const std::uintmax_t size =
      std::filesystem::exists(path, error) ? std::filesystem::file_size(path, error) : 0;
  if (size > 0 &&
      static_cast<std::int64_t>(size + line.size()) > g_state.max_bytes) {
    rotate_locked();
  }
  std::ofstream out(path, std::ios::app | std::ios::binary);
  if (!out) {
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);
    return;
  }
  out.write(line.data(), static_cast<std::streamsize>(line.size()));
}

}  // namespace

const char* log_level_name(LogLevel level) {
  switch (level) {
    case LogLevel::Debug:
      return "DEBUG";
    case LogLevel::Info:
      return "INFO";
    case LogLevel::Warn:
      return "WARN";
    case LogLevel::Error:
      return "ERROR";
  }
  return "INFO";
}

LogLevel parse_log_level(std::string_view text, LogLevel fallback) {
  std::string value(text);
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  if (value == "debug") return LogLevel::Debug;
  if (value == "info") return LogLevel::Info;
  if (value == "warn" || value == "warning") return LogLevel::Warn;
  if (value == "error") return LogLevel::Error;
  return fallback;
}

void init_logging(const LogOptions& options) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_state = LogState{};
  g_state.level = options.level;
  if (!options.file_name.empty()) {
    g_state.file_name = options.file_name;
  }
  g_state.max_bytes = std::max<std::int64_t>(options.max_bytes, 1024);
  g_state.keep_files = std::max(options.keep_files, 1);
  if (!options.directory.empty()) {
    std::error_code error;
    std::filesystem::create_directories(options.directory, error);
    if (!error) {
      g_state.directory = options.directory;
      g_state.active = true;
    }
  }
  write_locked(format_line(LogLevel::Info,
                           "日志初始化 level=" + std::string(log_level_name(g_state.level)) +
                               " dir=" +
                               (g_state.directory.empty() ? std::string("stderr")
                                                          : g_state.directory.string())));
}

void shutdown_logging() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_state.active) {
    write_locked(format_line(LogLevel::Info, "日志关闭"));
  }
  g_state = LogState{};
}

void log_message(LogLevel level, std::string_view message) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (static_cast<int>(level) < static_cast<int>(g_state.level)) {
    return;
  }
  write_locked(format_line(level, message));
}

}  // namespace xiaotu
