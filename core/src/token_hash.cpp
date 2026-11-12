#include "xiaotu/token_hash.h"

#include <array>
#include <cstddef>

#include "blake2/blake2.h"
#include "xiaotu/error.h"

namespace xiaotu {
namespace {

constexpr std::size_t kDigestBytes = 32;
constexpr char kHex[] = "0123456789abcdef";

}  // namespace

std::string hash_token(std::string_view token) {
  std::array<unsigned char, kDigestBytes> digest{};
  // 复用随仓库内嵌的 Argon2 附带的 BLAKE2b，不额外引入密码学实现或外部依赖。
  const int result =
      blake2b(digest.data(), digest.size(), token.data(), token.size(), nullptr, 0);
  if (result != 0) {
    throw AppError("TOKEN_HASH_FAILED", "会话令牌散列失败");
  }
  std::string out;
  out.reserve(kDigestBytes * 2);
  for (const unsigned char byte : digest) {
    out.push_back(kHex[byte >> 4]);
    out.push_back(kHex[byte & 0x0FU]);
  }
  return out;
}

}  // namespace xiaotu
