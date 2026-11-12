#include "xiaotu/ids.h"

#include <atomic>
#include <chrono>
#include <random>

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

std::string random_hex(std::size_t bytes) {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes * 2);
  for (std::size_t i = 0; i < bytes; ++i) {
    const auto value = static_cast<unsigned>(rng() & 0xFFU);
    out.push_back(kHex[value >> 4]);
    out.push_back(kHex[value & 0x0FU]);
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
