#pragma once

#include <filesystem>
#include <string>

namespace xiaotu {

// 同一数据目录的单实例锁：进程存活期间持有 <数据目录>/app.lock，
// 第二个进程获取失败后应提示用户并退出，避免两个实例同时迁移或写库。
// Windows 用命名互斥体保证互斥，POSIX 用 flock；PID 写入锁文件仅供提示。
class InstanceLock {
 public:
  InstanceLock() = default;
  ~InstanceLock();
  InstanceLock(const InstanceLock&) = delete;
  InstanceLock& operator=(const InstanceLock&) = delete;

  // 尝试锁定数据目录；目录必须已存在。已有实例持有时返回 false，
  // 并把对方 PID（能读到的话）放进 holder_pid()。
  bool acquire(const std::filesystem::path& data_dir);

  const std::filesystem::path& lock_path() const { return lock_path_; }
  const std::string& holder_pid() const { return holder_pid_; }

 private:
  void release();

  std::filesystem::path lock_path_;
  std::string holder_pid_;
#if defined(_WIN32)
  void* mutex_ = nullptr;
#else
  int fd_ = -1;
#endif
};

}  // namespace xiaotu
