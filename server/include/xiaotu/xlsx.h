#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace xiaotu {

// 报表导出用的最小 xlsx 生成器：单工作表、无三方依赖。
// ZIP 采用 store（不压缩）方式，工作簿只包含 Excel 打开所必需的核心部件。
struct XlsxCell {
  enum class Kind { Text, Integer, Money };

  static XlsxCell text(std::string value);
  static XlsxCell integer(std::int64_t value);
  // 金额从整数分换算为「元、两位小数」，写入数值单元格，保留两位小数格式。
  static XlsxCell money(std::int64_t cents);

  Kind kind = Kind::Text;
  std::string value;
};

class XlsxSheet {
 public:
  explicit XlsxSheet(std::string name);

  void add_text_row(std::initializer_list<std::string> cells);
  void add_header_row(std::initializer_list<std::string> cells);
  void add_row(std::initializer_list<XlsxCell> cells);

  std::string build() const;

 private:
  std::string name_;
  std::vector<std::vector<XlsxCell>> rows_;
  std::vector<bool> header_flags_;
};

}  // namespace xiaotu
