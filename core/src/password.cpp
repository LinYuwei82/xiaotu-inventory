#include "xiaotu/password.h"

#include <argon2.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "xiaotu/error.h"
#include "xiaotu/ids.h"

namespace xiaotu {
namespace {

constexpr std::uint32_t kTimeCost = 3;
constexpr std::uint32_t kMemoryKib = 64 * 1024;  // 64 MiB
constexpr std::uint32_t kParallelism = 1;
constexpr std::size_t kSaltBytes = 16;
constexpr std::size_t kHashBytes = 32;

}  // namespace

bool password_acceptable(std::string_view password) {
  return password.size() >= 8 && password.size() <= 128;
}

std::string hash_password(std::string_view password) {
  std::array<unsigned char, kSaltBytes> salt{};
  fill_random(salt.data(), salt.size());
  std::array<char, 512> encoded{};
  const int result = argon2id_hash_encoded(
      kTimeCost, kMemoryKib, kParallelism, password.data(), password.size(),
      salt.data(), salt.size(), kHashBytes, encoded.data(), encoded.size());
  if (result != ARGON2_OK) {
    throw AppError("PASSWORD_HASH_FAILED", argon2_error_message(result));
  }
  return std::string(encoded.data());
}

bool verify_password(std::string_view encoded, std::string_view password) {
  if (encoded.empty()) {
    return false;
  }
  const std::string hash(encoded);
  return argon2id_verify(hash.c_str(), password.data(), password.size()) == ARGON2_OK;
}

}  // namespace xiaotu
