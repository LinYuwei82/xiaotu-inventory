#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace xiaotu {

// 最小 xlsx 读取器：只解压 ZIP（store / deflate）并解析第一个工作表，
// 不支持公式、日期序列号、合并单元格与多工作表选择。
struct SheetRow {
  int number = 0;                     // Excel 实际行号，表头为 1
  std::vector<std::string> cells;     // 空单元格为空串
};

struct SheetTable {
  std::vector<std::string> header;    // 第 1 行的表头文字（已去掉尾部空列）
  std::vector<SheetRow> rows;         // 跳过整行为空的行
};

// 读取失败（不是 xlsx、ZIP 损坏、缺工作表、存在合并单元格）抛
// AppError("IMPORT_INVALID", ...)。
SheetTable read_first_sheet(std::string_view bytes);

// 表头文字 → 列下标。未知列、重复列、空表头、缺必填列都抛
// AppError("IMPORT_INVALID", ...)，不做模糊匹配。
class ColumnMap {
 public:
  std::size_t index(std::string_view column) const;
  bool has(std::string_view column) const;
  // 取单元格文本；列不存在或行太短都返回空串。
  std::string cell(const SheetRow& row, std::string_view column) const;

 private:
  friend ColumnMap map_columns(const std::vector<std::string>&,
                               const std::vector<std::string>&,
                               const std::vector<std::string>&);
  std::map<std::string, std::size_t> index_;
};

ColumnMap map_columns(const std::vector<std::string>& header,
                      const std::vector<std::string>& known,
                      const std::vector<std::string>& required);

// 去掉首尾空白（空格、制表符、回车、换行）。
std::string trim_text(std::string_view value);

}  // namespace xiaotu
