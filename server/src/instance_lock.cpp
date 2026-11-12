#include "xiaotu/instance_lock.h"

#include <cerrno>
#include <fstream>
#include <functional>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#include "xiaotu/error.h"

namespace xiaotu {
namespace {

std::string read_pid(const std::filesystem::path& path) {
  std::ifstream in(path);
  std::string pid;
  std::getline(in, pid);
  while (!pid.empty() && (pid.back() == '\r' || pid.back() == ' ')) {
    pid.pop_back();
  }
  return pid;
}

void write_pid(const std::filesystem::path& path, const std::string& pid) {
  std::ofstream out(path, std::ios::trunc);
  out << pid << "\n";
}

long long current_pid() {
#if defined(_WIN32)
  return static_cast<long long>(GetCurrentProcessId());
#else
  return static_cast<long long>(::getpid());
#endif
}

}  // namespace

InstanceLock::~InstanceLock() { release(); }

bool InstanceLock::acquire(const std::filesystem::path& data_dir) {
  release();
  lock_path_ = data_dir / "app.lock";

#if defined(_WIN32)
  // 锁名跟随数据目录，不同目录可并行；Local\ 前缀保证只在当前登录会话内互斥。
  const std::wstring name =
      L"Local\\com.simem.xiaotu.instance." +
      std::to_wstring(std::hash<std::wstring>{}(lock_path_.lexically_normal().wstring()));
  HANDLE mutex = CreateMutexW(nullptr, TRUE, name.c_str());
  if (mutex == nullptr) {
    throw AppError("LOCK_FAILED", "无法创建单实例锁");
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    holder_pid_ = read_pid(lock_path_);
    CloseHandle(mutex);
    return false;
  }
  mutex_ = mutex;
  write_pid(lock_path_, std::to_string(current_pid()));
  return true;
#else
  const int fd = ::open(lock_path_.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd < 0) {
    throw AppError("LOCK_FAILED", "无法打开单实例锁文件：" + lock_path_.string());
  }
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    holder_pid_ = read_pid(lock_path_);
    ::close(fd);
    return false;
  }
  fd_ = fd;
  write_pid(lock_path_, std::to_string(current_pid()));
  return true;
#endif
}

void InstanceLock::release() {
#if defined(_WIN32)
  if (mutex_ != nullptr) {
    ReleaseMutex(static_cast<HANDLE>(mutex_));
    CloseHandle(static_cast<HANDLE>(mutex_));
    mutex_ = nullptr;
  }
#else
  if (fd_ >= 0) {
    ::flock(fd_, LOCK_UN);
    ::close(fd_);
    fd_ = -1;
  }
#endif
  holder_pid_.clear();
}

}  // namespace xiaotu
