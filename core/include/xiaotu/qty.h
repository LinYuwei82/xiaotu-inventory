#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace xiaotu {

// 数量统一用 4 位小数的定点整数存储：1 个单位存为 10000。
// 代码里只做整数加减乘与整数除法，不出现浮点。
using Qty = std::int64_t;
inline constexpr Qty kQtyScale = 10000;
inline constexpr int kQtyMaxDecimals = 4;
// 上限 100 万个单位，避免数量与单价相乘时溢出。
inline constexpr Qty kQtyLimit = 10000000000LL;

inline Qty qty_from_int(std::int64_t value) { return value * kQtyScale; }

// 十进制文本 → 定点整数。最多 max_decimals 位小数，不接受正号、指数、空串与负数。
// 返回 nullopt 表示格式不合法或超出允许的小数位。
inline std::optional<Qty> parse_qty(std::string_view text,
                                    int max_decimals = kQtyMaxDecimals) {
  if (text.empty() || max_decimals < 0 || max_decimals > kQtyMaxDecimals) {
    return std::nullopt;
  }
  std::size_t pos = 0;
  bool negative = false;
  if (text[pos] == '-') {
    negative = true;
    ++pos;
  }
  if (pos == text.size()) {
    return std::nullopt;
  }

  std::int64_t whole = 0;
  int whole_digits = 0;
  while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
    whole = whole * 10 + (text[pos] - '0');
    ++whole_digits;
    ++pos;
    if (whole_digits > 9) {
      return std::nullopt;
    }
  }
  if (whole_digits == 0) {
    return std::nullopt;
  }

  std::int64_t fraction = 0;
  int fraction_digits = 0;
  if (pos < text.size() && text[pos] == '.') {
    ++pos;
    const std::size_t start = pos;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
      if (fraction_digits >= max_decimals) {
        return std::nullopt;
      }
      fraction = fraction * 10 + (text[pos] - '0');
      ++fraction_digits;
      ++pos;
    }
    if (pos == start) {
      return std::nullopt;
    }
  }
  if (pos != text.size()) {
    return std::nullopt;
  }

  for (int i = fraction_digits; i < kQtyMaxDecimals; ++i) {
    fraction *= 10;
  }
  Qty scaled = whole * kQtyScale + fraction;
  if (scaled > kQtyLimit) {
    return std::nullopt;
  }
  return negative ? -scaled : scaled;
}

// 定点整数 → 十进制文本，最多 4 位小数并去掉末尾 0；整数不显示小数点。
inline std::string qty_text(Qty qty) {
  const bool negative = qty < 0;
  const Qty absolute = negative ? -qty : qty;
  std::string fraction = std::to_string(absolute % kQtyScale);
  if (fraction.size() < static_cast<std::size_t>(kQtyMaxDecimals)) {
    fraction.insert(0, static_cast<std::size_t>(kQtyMaxDecimals) - fraction.size(), '0');
  }
  while (!fraction.empty() && fraction.back() == '0') {
    fraction.pop_back();
  }
  return (negative ? "-" : "") + std::to_string(absolute / kQtyScale) +
         (fraction.empty() ? "" : "." + fraction);
}

// 数量（定点整数）× 单价（分）→ 金额（分），四舍五入到分。
inline std::int64_t mul_qty_price(Qty qty, std::int64_t price_cents) {
  const std::int64_t numerator = qty * price_cents;
  if (numerator >= 0) {
    return (numerator + kQtyScale / 2) / kQtyScale;
  }
  return -((-numerator + kQtyScale / 2) / kQtyScale);
}

// 数量是否符合单位精度：1 件（0 位）必须是 10000 的整数倍，千克（3 位）是 10 的整数倍。
inline bool qty_fits_decimals(Qty qty, int decimals) {
  if (decimals < 0) {
    decimals = 0;
  }
  if (decimals > kQtyMaxDecimals) {
    decimals = kQtyMaxDecimals;
  }
  Qty step = 1;
  for (int i = decimals; i < kQtyMaxDecimals; ++i) {
    step *= 10;
  }
  return qty % step == 0;
}

}  // namespace xiaotu
