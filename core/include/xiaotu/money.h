#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace xiaotu {

// 金额一律以「分」为单位的整数存储与运算，禁止浮点。
using Cents = std::int64_t;

// 12345 -> "123.45"
std::string format_cents(Cents value);

// "12.34" / "12" / "0.05" -> 分。非法输入（负号、超过两位小数、非数字）返回 nullopt。
std::optional<Cents> parse_cents(std::string_view text);

}  // namespace xiaotu
