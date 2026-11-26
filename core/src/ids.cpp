#include "xiaotu/ids.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <random>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#endif

namespace xiaotu {
namespace {
// 低 12 位为进程内序列，高位为 2020-01-01 起的毫秒数。
// 约束：整体取值必须小于 2^53（约 9.0e15），否则前端 JS 的 Number 会丢精度；
// 当前设计到 2089 年仍安全，单毫秒可分配 4096 个 id。
constexpr std::int64_t kEpochMs = 1577836800000LL;
constexpr std::int64_t kSeqMask = 0xFFF;
constexpr int kSeqBits = 12;
}  // namespace

std::int64_t next_id() {
  static std::atomic<std::int64_t> counter{0};
  const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  const auto offset = now_ms > kEpochMs ? now_ms - kEpochMs : 0;
  const auto seq = counter.fetch_add(1, std::memory_order_relaxed) & kSeqMask;
  return (offset << kSeqBits) | seq;
}

void fill_random(unsigned char* buffer, std::size_t size) {
#if defined(_WIN32)
  if (BCryptGenRandom(nullptr, buffer, static_cast<ULONG>(size),
                      BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0) {
    return;
  }
#else
  std::ifstream source("/dev/urandom", std::ios::binary);
  if (source && source.read(reinterpret_cast<char*>(buffer),
                            static_cast<std::streamsize>(size))) {
    return;
  }
#endif
  // 兜底：系统随机源不可用时退回 random_device（每字节单独取数）。
  std::random_device device;
  for (std::size_t index = 0; index < size; ++index) {
    buffer[index] = static_cast<unsigned char>(device() & 0xFFU);
  }
}

std::string random_hex(std::size_t bytes) {
  // 必须用加密安全随机源：这里的输出是会话 id、CSRF、一次性换票与
  // 免输凭据等安全令牌；mt19937 这类伪随机数种子熵不足、输出可预测。
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.resize(bytes * 2);
  std::string raw(bytes, '\0');
  fill_random(reinterpret_cast<unsigned char*>(raw.data()), bytes);
  for (std::size_t i = 0; i < bytes; ++i) {
    const unsigned value = static_cast<unsigned char>(raw[i]);
    out[i * 2] = kHex[value >> 4];
    out[i * 2 + 1] = kHex[value & 0x0FU];
  }
  return out;
}

bool constant_time_equals(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) {
    return false;
  }
  unsigned char diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    diff |= static_cast<unsigned char>(a[i] ^ b[i]);
  }
  return diff == 0;
}

}  // namespace xiaotu
