#pragma once

#include <ctime>
#include <filesystem>
#include <string>

namespace xiaotu {

// core 里唯一允许出现编译器/操作系统差异的边界：只补齐标准库在不同实现上的能力差异，
// 不引入任何平台框架、句柄或专有 API。其余代码一律调用这里。

// 线程安全的本地时间转换：POSIX 用 localtime_r，MSVC 用 localtime_s（参数顺序相反）。
inline std::tm local_time(std::time_t seconds) {
  std::tm local{};
#if defined(_WIN32)
  localtime_s(&local, &seconds);
#else
  localtime_r(&seconds, &local);
#endif
  return local;
}

// std::filesystem::path → UTF-8 窄串。SQLite 的 sqlite3_open* 只接受 UTF-8；
// Windows 上 path 内部是 wchar_t，直接 .string() 会按 ANSI 代码页转换，中文路径会打不开。
inline std::string path_to_utf8(const std::filesystem::path& path) {
  const std::u8string utf8 = path.u8string();
  return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

}  // namespace xiaotu
