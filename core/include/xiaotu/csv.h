#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace xiaotu {

// 表格软件会把 = + - @ 开头的单元格当公式执行。负号后接数字或小数点是正常数值，
// 保持原样；其余危险前缀加单引号，让导入方按文本处理。
inline bool csv_looks_like_formula(std::string_view value) {
  if (value.empty()) return false;
  const char first = value.front();
  if (first == '=' || first == '+' || first == '@' || first == '\t' || first == '\r') {
    return true;
  }
  if (first != '-') return false;
  return value.size() == 1 ||
         !(std::isdigit(static_cast<unsigned char>(value[1])) || value[1] == '.');
}

inline std::string csv_escape(std::string_view value) {
  const bool formula = csv_looks_like_formula(value);
  if (!formula && value.find_first_of(",\"\r\n") == std::string_view::npos) {
    return std::string(value);
  }
  std::string out;
  out.reserve(value.size() + 3);
  out += '"';
  if (formula) out += '\'';
  for (const char ch : value) {
    if (ch == '"') out += '"';
    out += ch;
  }
  out += '"';
  return out;
}

}  // namespace xiaotu
