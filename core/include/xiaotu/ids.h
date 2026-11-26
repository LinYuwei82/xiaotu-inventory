#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace xiaotu {

// 64 位主键：高位为 2020-01-01 起的毫秒数，低 12 位为进程内序列。
// 整体保持在 2^53 以内，前端 JS 可直接用 Number 表示而不丢精度。
// 不依赖数据库自增，便于日后平滑迁移 PostgreSQL。
std::int64_t next_id();

// 加密安全随机字节：Windows 用 BCryptGenRandom，POSIX 读 /dev/urandom，
// 两者都失败才回退 std::random_device。口令盐与全部安全 token 共用这一实现。
void fill_random(unsigned char* buffer, std::size_t size);

// 指定字节数的十六进制随机串（用于一次性 token、会话 id）。
std::string random_hex(std::size_t bytes);

// 常量时间比较，避免 token 比较被计时攻击。
bool constant_time_equals(const std::string& a, const std::string& b);

}  // namespace xiaotu
