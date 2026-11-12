#pragma once

#include <string>
#include <string_view>

namespace xiaotu {

// 会话令牌的落库形式。令牌本身是 32 字节随机数，不需要加盐或慢哈希；
// 这里只做一次快速摘要，避免数据库文件泄露后有人直接拿去当 Cookie 用。
std::string hash_token(std::string_view token);

}  // namespace xiaotu
