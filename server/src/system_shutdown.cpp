#include "xiaotu/system_shutdown.h"

#include <fstream>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

#include "xiaotu/ids.h"
#include "xiaotu/log.h"

namespace xiaotu {
namespace {

bool v4_is_loopback(const unsigned char* bytes) {
  return bytes[0] == 127 && bytes[1] == 0 && bytes[2] == 0 && bytes[3] == 1;
}

const unsigned char* ipv6_bytes(const in6_addr& address) {
#if defined(_WIN32)
  return address.u.Byte;
#else
  return address.s6_addr;
#endif
}

}  // namespace

bool is_loopback_address(std::string_view address) {
  if (address.empty()) {
    return false;
  }
  const std::string text(address);

  in_addr v4{};
  if (inet_pton(AF_INET, text.c_str(), &v4) == 1) {
    return v4_is_loopback(reinterpret_cast<const unsigned char*>(&v4));
  }

  in6_addr v6{};
  if (inet_pton(AF_INET6, text.c_str(), &v6) != 1) {
    return false;
  }
  const unsigned char* bytes = ipv6_bytes(v6);
  bool loopback = true;
  for (int index = 0; index < 15; ++index) {
    loopback = loopback && bytes[index] == 0;
  }
  if (loopback && bytes[15] == 1) {
    return true;
  }
  for (int index = 0; index < 10; ++index) {
    if (bytes[index] != 0) {
      return false;
    }
  }
  return bytes[10] == 0xFF && bytes[11] == 0xFF && v4_is_loopback(bytes + 12);
}

bool ShutdownToken::issue(const std::filesystem::path& file) {
  std::lock_guard<std::mutex> lock(mutex_);
  token_.clear();
  file_.clear();
  used_ = false;

  const std::string token = random_hex(32);
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  out << token;
  out.close();
  if (!out) {
    return false;
  }

  std::error_code error;
  std::filesystem::permissions(
      file, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
      std::filesystem::perm_options::replace, error);
  if (error) {
    // 权限收紧失败不阻断启动，但要在日志里留痕（文件内容仍是随机令牌）。
    log_warn("关闭令牌文件权限收紧失败: " + file.string() + "（" + error.message() + "）");
  }

  token_ = token;
  file_ = file;
  return true;
}

bool ShutdownToken::consume(std::string_view candidate) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (used_ || token_.empty() || candidate.empty()) {
    return false;
  }
  if (!constant_time_equals(std::string(candidate), token_)) {
    return false;
  }
  used_ = true;
  token_.clear();
  std::error_code error;
  std::filesystem::remove(file_, error);
  if (error) {
    log_warn("关闭令牌文件删除失败: " + file_.string() + "（" + error.message() + "）");
  }
  return true;
}

bool ShutdownToken::issued() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return !token_.empty() && !used_;
}

std::string ShutdownToken::file_path() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return file_.string();
}

}  // namespace xiaotu
