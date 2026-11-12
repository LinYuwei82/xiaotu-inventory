#include "xiaotu/money.h"

#include <cctype>
#include <cstdio>

namespace xiaotu {

std::string format_cents(Cents value) {
  const bool negative = value < 0;
  const auto abs_value = static_cast<unsigned long long>(negative ? -value : value);
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%s%llu.%02llu", negative ? "-" : "",
                abs_value / 100ULL, abs_value % 100ULL);
  return std::string(buffer);
}

std::optional<Cents> parse_cents(std::string_view text) {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  if (begin == end) {
    return std::nullopt;
  }

  Cents integer_part = 0;
  Cents fraction_part = 0;
  int fraction_digits = 0;
  bool seen_dot = false;
  bool seen_digit = false;

  for (std::size_t i = begin; i < end; ++i) {
    const char ch = text[i];
    if (ch == '.') {
      if (seen_dot) {
        return std::nullopt;
      }
      seen_dot = true;
      continue;
    }
    if (!std::isdigit(static_cast<unsigned char>(ch))) {
      return std::nullopt;
    }
    seen_digit = true;
    const auto digit = static_cast<Cents>(ch - '0');
    if (!seen_dot) {
      integer_part = integer_part * 10 + digit;
      if (integer_part > 1'000'000'000'000LL) {
        return std::nullopt;
      }
    } else {
      if (fraction_digits >= 2) {
        return std::nullopt;
      }
      fraction_part = fraction_part * 10 + digit;
      ++fraction_digits;
    }
  }

  if (!seen_digit) {
    return std::nullopt;
  }
  if (fraction_digits == 1) {
    fraction_part *= 10;
  }
  return integer_part * 100 + fraction_part;
}

}  // namespace xiaotu
