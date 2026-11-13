#pragma once

#include <cstdint>
#include <fstream>
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
  void add_text_row(const std::vector<std::string>& cells);
  void add_header_row(const std::vector<std::string>& cells);
  void add_row(const std::vector<XlsxCell>& cells);

  std::string build() const;

 private:
  std::string name_;
  std::vector<std::vector<XlsxCell>> rows_;
  std::vector<bool> header_flags_;
};

// 大报表导出用的流式 xlsx：工作表 XML 按批写入临时文件，行数据不驻留内存；
// 组装完成后返回临时文件路径，由调用方流式发送并在响应结束时删除。
class XlsxStreamWriter {
 public:
  explicit XlsxStreamWriter(std::string name);
  ~XlsxStreamWriter();

  XlsxStreamWriter(const XlsxStreamWriter&) = delete;
  XlsxStreamWriter& operator=(const XlsxStreamWriter&) = delete;

  bool ok() const { return ok_; }

  void add_text_row(std::initializer_list<std::string> cells);
  void add_header_row(std::initializer_list<std::string> cells);
  void add_row(std::initializer_list<XlsxCell> cells);
  void add_text_row(const std::vector<std::string>& cells);
  void add_header_row(const std::vector<std::string>& cells);
  void add_row(const std::vector<XlsxCell>& cells);

  // 结束工作表并组装 xlsx；成功返回临时文件路径，失败返回空串。
  std::string finish();

 private:
  void append_row(const std::vector<XlsxCell>& cells, bool header);
  void append_raw(const std::string& text);
  void flush_pending();

  std::string name_;
  std::string sheet_path_;
  std::string pending_;
  std::uint32_t crc_ = 0xFFFFFFFFu;
  std::uint64_t sheet_size_ = 0;
  std::int64_t row_count_ = 0;
  bool ok_ = false;
  std::ofstream sheet_;
};

}  // namespace xiaotu
