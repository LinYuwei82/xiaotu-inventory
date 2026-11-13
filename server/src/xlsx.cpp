#include "xiaotu/xlsx.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace xiaotu {
namespace {

void put_u16(std::string& out, std::uint16_t value) {
  out.push_back(static_cast<char>(value & 0xFF));
  out.push_back(static_cast<char>((value >> 8) & 0xFF));
}

void put_u32(std::string& out, std::uint32_t value) {
  out.push_back(static_cast<char>(value & 0xFF));
  out.push_back(static_cast<char>((value >> 8) & 0xFF));
  out.push_back(static_cast<char>((value >> 16) & 0xFF));
  out.push_back(static_cast<char>((value >> 24) & 0xFF));
}

const std::array<std::uint32_t, 256>& crc_table() {
  static const std::array<std::uint32_t, 256> table = [] {
    std::array<std::uint32_t, 256> values{};
    for (std::uint32_t i = 0; i < values.size(); ++i) {
      std::uint32_t current = i;
      for (int bit = 0; bit < 8; ++bit) {
        current = (current & 1u) ? 0xEDB88320u ^ (current >> 1) : current >> 1;
      }
      values[i] = current;
    }
    return values;
  }();
  return table;
}

// 增量 CRC：流式写工作表时边写边算，避免为算校验和再读一遍数据。
std::uint32_t crc32_update(std::uint32_t state, const char* data, std::size_t size) {
  const auto& table = crc_table();
  for (std::size_t i = 0; i < size; ++i) {
    state = table[(state ^ static_cast<unsigned char>(data[i])) & 0xFFu] ^ (state >> 8);
  }
  return state;
}

std::uint32_t crc32(const std::string& data) {
  return crc32_update(0xFFFFFFFFu, data.data(), data.size()) ^ 0xFFFFFFFFu;
}

std::string xml_escape(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (const unsigned char ch : value) {
    switch (ch) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '"':
        out += "&quot;";
        break;
      case '\'':
        out += "&apos;";
        break;
      default:
        if (ch < 0x20 && ch != '\t' && ch != '\n' && ch != '\r') break;
        out.push_back(static_cast<char>(ch));
    }
  }
  return out;
}

// 整数分 → 元，保留两位小数；全程整数运算，避免浮点误差。
std::string cents_to_yuan(std::int64_t cents) {
  const bool negative = cents < 0;
  const std::uint64_t abs =
      negative ? static_cast<std::uint64_t>(-(cents + 1)) + 1 : static_cast<std::uint64_t>(cents);
  std::string fraction = std::to_string(abs % 100);
  if (fraction.size() < 2) fraction.insert(0, 2 - fraction.size(), '0');
  return (negative ? "-" : "") + std::to_string(abs / 100) + "." + fraction;
}

std::string column_name(std::size_t index) {
  std::string name;
  for (++index; index > 0; index = (index - 1) / 26) {
    name.insert(name.begin(), static_cast<char>('A' + (index - 1) % 26));
  }
  return name;
}

std::string sanitize_sheet_name(const std::string& name) {
  std::string out;
  for (const char ch : name) {
    if (ch == '[' || ch == ']' || ch == ':' || ch == '*' || ch == '?' || ch == '/' ||
        ch == '\\') {
      out.push_back(' ');
    } else {
      out.push_back(ch);
    }
  }
  if (out.empty()) out = "报表";
  if (out.size() > 31) {
    out.resize(31);
    while (!out.empty() && out.back() == ' ') out.pop_back();
  }
  return out;
}

std::string row_xml(std::size_t row_index, const std::vector<XlsxCell>& cells, bool header) {
  const std::string row_number = std::to_string(row_index + 1);
  std::string xml = "<row r=\"" + row_number + "\">";
  for (std::size_t c = 0; c < cells.size(); ++c) {
    const XlsxCell& cell = cells[c];
    const std::string ref = column_name(c) + row_number;
    if (cell.kind == XlsxCell::Kind::Text) {
      xml += "<c r=\"" + ref + "\"";
      if (header) xml += " s=\"1\"";
      xml += " t=\"inlineStr\"><is><t xml:space=\"preserve\">" + xml_escape(cell.value) +
             "</t></is></c>";
    } else if (cell.kind == XlsxCell::Kind::Integer) {
      xml += "<c r=\"" + ref + "\"><v>" + cell.value + "</v></c>";
    } else {
      xml += "<c r=\"" + ref + "\" s=\"2\"><v>" + cell.value + "</v></c>";
    }
  }
  xml += "</row>";
  return xml;
}

std::string worksheet_xml(const std::vector<std::vector<XlsxCell>>& rows,
                          const std::vector<bool>& header_flags) {
  std::string xml;
  xml +=
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
      "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
      "<sheetViews><sheetView workbookViewId=\"0\"/></sheetViews>"
      "<sheetFormatPr defaultRowHeight=\"15\"/><sheetData>";
  for (std::size_t r = 0; r < rows.size(); ++r) {
    xml += row_xml(r, rows[r], r < header_flags.size() && header_flags[r]);
  }
  xml += "</sheetData></worksheet>";
  return xml;
}

constexpr const char* kSheetPrefix =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
    "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
    "<sheetViews><sheetView workbookViewId=\"0\"/></sheetViews>"
    "<sheetFormatPr defaultRowHeight=\"15\"/><sheetData>";
constexpr const char* kSheetSuffix = "</sheetData></worksheet>";

std::string workbook_xml(const std::string& sheet_name) {
  return "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
         "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
         "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
         "<sheets><sheet name=\"" +
         xml_escape(sheet_name) + "\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>";
}

std::string styles_xml() {
  return "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
         "<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
         "<numFmts count=\"1\"><numFmt numFmtId=\"164\" formatCode=\"0.00\"/></numFmts>"
         "<fonts count=\"2\">"
         "<font><sz val=\"11\"/><name val=\"Calibri\"/></font>"
         "<font><b/><sz val=\"11\"/><name val=\"Calibri\"/></font>"
         "</fonts>"
         "<fills count=\"2\">"
         "<fill><patternFill patternType=\"none\"/></fill>"
         "<fill><patternFill patternType=\"gray125\"/></fill>"
         "</fills>"
         "<borders count=\"1\"><border/></borders>"
         "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" "
         "borderId=\"0\"/></cellStyleXfs>"
         "<cellXfs count=\"3\">"
         "<xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/>"
         "<xf numFmtId=\"0\" fontId=\"1\" fillId=\"0\" borderId=\"0\" xfId=\"0\" "
         "applyFont=\"1\" applyAlignment=\"1\"><alignment horizontal=\"center\"/>"
         "</xf>"
         "<xf numFmtId=\"164\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\" "
         "applyNumberFormat=\"1\"/>"
         "</cellXfs>"
         "<cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" "
         "builtinId=\"0\"/></cellStyles>"
         "<dxfs count=\"0\"/>"
         "<tableStyles count=\"0\" defaultTableStyle=\"TableStyleMedium2\" "
         "defaultPivotStyle=\"PivotStyleLight16\"/>"
         "</styleSheet>";
}

std::string content_types_xml() {
  return "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
         "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
         "<Default Extension=\"rels\" "
         "ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
         "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
         "<Override PartName=\"/xl/workbook.xml\" "
         "ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet."
         "main+xml\"/>"
         "<Override PartName=\"/xl/worksheets/sheet1.xml\" "
         "ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+"
         "xml\"/>"
         "<Override PartName=\"/xl/styles.xml\" "
         "ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>"
         "</Types>";
}

std::string root_rels_xml() {
  return "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
         "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
         "<Relationship Id=\"rId1\" "
         "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/"
         "officeDocument\" Target=\"xl/workbook.xml\"/>"
         "</Relationships>";
}

std::string workbook_rels_xml() {
  return "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
         "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
         "<Relationship Id=\"rId1\" "
         "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" "
         "Target=\"worksheets/sheet1.xml\"/>"
         "<Relationship Id=\"rId2\" "
         "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" "
         "Target=\"styles.xml\"/>"
         "</Relationships>";
}

// ZIP（store，不压缩）：本地文件头 + 数据 + 中央目录 + 目录结束记录。
std::string zip_store(const std::vector<std::pair<std::string, std::string>>& files) {
  struct CentralEntry {
    std::string name;
    std::uint32_t crc = 0;
    std::uint32_t size = 0;
    std::uint32_t offset = 0;
  };

  std::string out;
  std::vector<CentralEntry> central;
  central.reserve(files.size());
  for (const auto& [name, data] : files) {
    const std::uint32_t crc = crc32(data);
    const std::uint32_t size = static_cast<std::uint32_t>(data.size());
    central.push_back(CentralEntry{name, crc, size, static_cast<std::uint32_t>(out.size())});

    put_u32(out, 0x04034b50u);
    put_u16(out, 10);  // version needed to extract（store）
    put_u16(out, 0);   // flags
    put_u16(out, 0);   // method: store
    put_u16(out, 0);   // mod time
    put_u16(out, 0x0021);  // mod date: 1980-01-01
    put_u32(out, crc);
    put_u32(out, size);
    put_u32(out, size);
    put_u16(out, static_cast<std::uint16_t>(name.size()));
    put_u16(out, 0);  // extra length
    out += name;
    out += data;
  }

  const std::uint32_t central_offset = static_cast<std::uint32_t>(out.size());
  for (const auto& entry : central) {
    put_u32(out, 0x02014b50u);
    put_u16(out, 20);  // version made by
    put_u16(out, 10);  // version needed
    put_u16(out, 0);
    put_u16(out, 0);
    put_u16(out, 0);
    put_u16(out, 0x0021);
    put_u32(out, entry.crc);
    put_u32(out, entry.size);
    put_u32(out, entry.size);
    put_u16(out, static_cast<std::uint16_t>(entry.name.size()));
    put_u16(out, 0);  // extra
    put_u16(out, 0);  // comment
    put_u16(out, 0);  // disk
    put_u16(out, 0);  // internal attributes
    put_u32(out, 0);  // external attributes
    put_u32(out, entry.offset);
    out += entry.name;
  }

  const std::uint32_t central_size = static_cast<std::uint32_t>(out.size()) - central_offset;
  put_u32(out, 0x06054b50u);
  put_u16(out, 0);
  put_u16(out, 0);
  put_u16(out, static_cast<std::uint16_t>(central.size()));
  put_u16(out, static_cast<std::uint16_t>(central.size()));
  put_u32(out, central_size);
  put_u32(out, central_offset);
  put_u16(out, 0);
  return out;
}

// ---------- 流式写出：ZIP 直接落到临时文件，工作表部分从临时工作文件拷贝 ----------

void write_u16(std::ostream& out, std::uint16_t value) {
  char bytes[2] = {static_cast<char>(value & 0xFF), static_cast<char>((value >> 8) & 0xFF)};
  out.write(bytes, sizeof(bytes));
}

void write_u32(std::ostream& out, std::uint32_t value) {
  char bytes[4] = {static_cast<char>(value & 0xFF), static_cast<char>((value >> 8) & 0xFF),
                   static_cast<char>((value >> 16) & 0xFF),
                   static_cast<char>((value >> 24) & 0xFF)};
  out.write(bytes, sizeof(bytes));
}

void write_local_header(std::ostream& out, const std::string& name, std::uint32_t crc,
                        std::uint32_t size) {
  write_u32(out, 0x04034b50u);
  write_u16(out, 10);  // version needed to extract（store）
  write_u16(out, 0);   // flags
  write_u16(out, 0);   // method: store
  write_u16(out, 0);   // mod time
  write_u16(out, 0x0021);  // mod date: 1980-01-01
  write_u32(out, crc);
  write_u32(out, size);
  write_u32(out, size);
  write_u16(out, static_cast<std::uint16_t>(name.size()));
  write_u16(out, 0);  // extra length
  out.write(name.data(), static_cast<std::streamsize>(name.size()));
}

void write_central_header(std::ostream& out, const std::string& name, std::uint32_t crc,
                          std::uint32_t size, std::uint32_t offset) {
  write_u32(out, 0x02014b50u);
  write_u16(out, 20);  // version made by
  write_u16(out, 10);  // version needed
  write_u16(out, 0);
  write_u16(out, 0);
  write_u16(out, 0);
  write_u16(out, 0x0021);
  write_u32(out, crc);
  write_u32(out, size);
  write_u32(out, size);
  write_u16(out, static_cast<std::uint16_t>(name.size()));
  write_u16(out, 0);  // extra
  write_u16(out, 0);  // comment
  write_u16(out, 0);  // disk
  write_u16(out, 0);  // internal attributes
  write_u32(out, 0);  // external attributes
  write_u32(out, offset);
  out.write(name.data(), static_cast<std::streamsize>(name.size()));
}

// 临时文件名带随机后缀，避免并发导出互相覆盖；进程退出后由调用方或析构清理。
std::string temp_path(const char* suffix) {
  std::random_device device;
  const std::uint64_t value =
      (static_cast<std::uint64_t>(device()) << 32) ^ static_cast<std::uint64_t>(device());
  char name[48] = {};
  std::snprintf(name, sizeof(name), "xiaotu-xlsx-%016llx%s",
                static_cast<unsigned long long>(value), suffix);
  return (std::filesystem::temp_directory_path() / name).string();
}

// 分块拷贝，峰值内存固定为 64KB。
bool copy_stream(std::istream& in, std::ostream& out, std::uint64_t expected) {
  std::vector<char> buffer(64 * 1024);
  std::uint64_t copied = 0;
  while (copied < expected) {
    const std::size_t chunk = static_cast<std::size_t>(
        std::min<std::uint64_t>(buffer.size(), expected - copied));
    in.read(buffer.data(), static_cast<std::streamsize>(chunk));
    const auto got = static_cast<std::size_t>(in.gcount());
    if (got == 0) return false;
    out.write(buffer.data(), static_cast<std::streamsize>(got));
    if (!out) return false;
    copied += got;
  }
  return true;
}

}  // namespace

XlsxCell XlsxCell::text(std::string value) {
  XlsxCell cell;
  cell.kind = Kind::Text;
  cell.value = std::move(value);
  return cell;
}

XlsxCell XlsxCell::integer(std::int64_t value) {
  XlsxCell cell;
  cell.kind = Kind::Integer;
  cell.value = std::to_string(value);
  return cell;
}

XlsxCell XlsxCell::money(std::int64_t cents) {
  XlsxCell cell;
  cell.kind = Kind::Money;
  cell.value = cents_to_yuan(cents);
  return cell;
}

XlsxSheet::XlsxSheet(std::string name) : name_(sanitize_sheet_name(name)) {}

void XlsxSheet::add_text_row(std::initializer_list<std::string> cells) {
  std::vector<XlsxCell> row;
  row.reserve(cells.size());
  for (const auto& cell : cells) row.push_back(XlsxCell::text(cell));
  rows_.push_back(std::move(row));
  header_flags_.push_back(false);
}

void XlsxSheet::add_header_row(std::initializer_list<std::string> cells) {
  std::vector<XlsxCell> row;
  row.reserve(cells.size());
  for (const auto& cell : cells) row.push_back(XlsxCell::text(cell));
  rows_.push_back(std::move(row));
  header_flags_.push_back(true);
}

void XlsxSheet::add_row(std::initializer_list<XlsxCell> cells) {
  rows_.emplace_back(cells);
  header_flags_.push_back(false);
}

void XlsxSheet::add_text_row(const std::vector<std::string>& cells) {
  std::vector<XlsxCell> row;
  row.reserve(cells.size());
  for (const auto& cell : cells) row.push_back(XlsxCell::text(cell));
  rows_.push_back(std::move(row));
  header_flags_.push_back(false);
}

void XlsxSheet::add_header_row(const std::vector<std::string>& cells) {
  std::vector<XlsxCell> row;
  row.reserve(cells.size());
  for (const auto& cell : cells) row.push_back(XlsxCell::text(cell));
  rows_.push_back(std::move(row));
  header_flags_.push_back(true);
}

void XlsxSheet::add_row(const std::vector<XlsxCell>& cells) {
  rows_.push_back(cells);
  header_flags_.push_back(false);
}

std::string XlsxSheet::build() const {
  return zip_store({
      {"[Content_Types].xml", content_types_xml()},
      {"_rels/.rels", root_rels_xml()},
      {"xl/workbook.xml", workbook_xml(name_)},
      {"xl/_rels/workbook.xml.rels", workbook_rels_xml()},
      {"xl/styles.xml", styles_xml()},
      {"xl/worksheets/sheet1.xml", worksheet_xml(rows_, header_flags_)},
  });
}

// ---------- XlsxStreamWriter ----------

XlsxStreamWriter::XlsxStreamWriter(std::string name) : name_(sanitize_sheet_name(name)) {
  try {
    sheet_path_ = temp_path(".sheet");
  } catch (const std::exception&) {
    ok_ = false;
    return;
  }
  sheet_.open(sheet_path_, std::ios::binary | std::ios::trunc);
  if (!sheet_.is_open()) {
    ok_ = false;
    return;
  }
  ok_ = true;
  append_raw(kSheetPrefix);
}

XlsxStreamWriter::~XlsxStreamWriter() {
  if (sheet_.is_open()) sheet_.close();
  if (!sheet_path_.empty()) {
    std::error_code error;
    std::filesystem::remove(sheet_path_, error);
  }
}

void XlsxStreamWriter::append_raw(const std::string& text) {
  if (!ok_) return;
  pending_ += text;
  if (pending_.size() >= 64 * 1024) flush_pending();
}

void XlsxStreamWriter::flush_pending() {
  if (pending_.empty()) return;
  if (!ok_) {
    pending_.clear();
    return;
  }
  crc_ = crc32_update(crc_, pending_.data(), pending_.size());
  sheet_size_ += pending_.size();
  sheet_.write(pending_.data(), static_cast<std::streamsize>(pending_.size()));
  pending_.clear();
  if (!sheet_) ok_ = false;
}

void XlsxStreamWriter::append_row(const std::vector<XlsxCell>& cells, bool header) {
  if (!ok_) return;
  append_raw(row_xml(static_cast<std::size_t>(row_count_), cells, header));
  ++row_count_;
}

void XlsxStreamWriter::add_text_row(std::initializer_list<std::string> cells) {
  add_text_row(std::vector<std::string>(cells));
}

void XlsxStreamWriter::add_header_row(std::initializer_list<std::string> cells) {
  add_header_row(std::vector<std::string>(cells));
}

void XlsxStreamWriter::add_row(std::initializer_list<XlsxCell> cells) {
  add_row(std::vector<XlsxCell>(cells));
}

void XlsxStreamWriter::add_text_row(const std::vector<std::string>& cells) {
  std::vector<XlsxCell> row;
  row.reserve(cells.size());
  for (const auto& cell : cells) row.push_back(XlsxCell::text(cell));
  append_row(row, false);
}

void XlsxStreamWriter::add_header_row(const std::vector<std::string>& cells) {
  std::vector<XlsxCell> row;
  row.reserve(cells.size());
  for (const auto& cell : cells) row.push_back(XlsxCell::text(cell));
  append_row(row, true);
}

void XlsxStreamWriter::add_row(const std::vector<XlsxCell>& cells) { append_row(cells, false); }

std::string XlsxStreamWriter::finish() {
  if (!ok_) return {};
  append_raw(kSheetSuffix);
  flush_pending();
  sheet_.flush();
  const bool sheet_ok = static_cast<bool>(sheet_);
  sheet_.close();
  if (!sheet_ok || sheet_size_ > 0xFFFFFFFFull) {
    ok_ = false;
    return {};
  }

  std::string output;
  try {
    output = temp_path(".xlsx");
  } catch (const std::exception&) {
    ok_ = false;
    return {};
  }

  std::ofstream out(output, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    ok_ = false;
    return {};
  }

  struct Entry {
    std::string name;
    std::uint32_t crc = 0;
    std::uint32_t size = 0;
    std::uint32_t offset = 0;
  };
  std::vector<Entry> central;
  const auto offset_now = [&out]() { return static_cast<std::uint32_t>(out.tellp()); };
  const auto write_memory_part = [&](const std::string& part, const std::string& data) {
    const std::uint32_t crc = crc32(data);
    const std::uint32_t size = static_cast<std::uint32_t>(data.size());
    central.push_back(Entry{part, crc, size, offset_now()});
    write_local_header(out, part, crc, size);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
  };

  write_memory_part("[Content_Types].xml", content_types_xml());
  write_memory_part("_rels/.rels", root_rels_xml());
  write_memory_part("xl/workbook.xml", workbook_xml(name_));
  write_memory_part("xl/_rels/workbook.xml.rels", workbook_rels_xml());
  write_memory_part("xl/styles.xml", styles_xml());

  const std::string sheet_part = "xl/worksheets/sheet1.xml";
  const std::uint32_t sheet_crc = crc_ ^ 0xFFFFFFFFu;
  const std::uint32_t sheet_bytes = static_cast<std::uint32_t>(sheet_size_);
  central.push_back(Entry{sheet_part, sheet_crc, sheet_bytes, offset_now()});
  write_local_header(out, sheet_part, sheet_crc, sheet_bytes);

  std::ifstream in(sheet_path_, std::ios::binary);
  const bool copied = in.is_open() && copy_stream(in, out, sheet_bytes);
  in.close();
  if (!copied) {
    out.close();
    std::error_code error;
    std::filesystem::remove(output, error);
    ok_ = false;
    return {};
  }

  const std::uint32_t central_offset = offset_now();
  for (const auto& entry : central) {
    write_central_header(out, entry.name, entry.crc, entry.size, entry.offset);
  }
  const std::uint32_t central_size = offset_now() - central_offset;
  write_u32(out, 0x06054b50u);
  write_u16(out, 0);
  write_u16(out, 0);
  write_u16(out, static_cast<std::uint16_t>(central.size()));
  write_u16(out, static_cast<std::uint16_t>(central.size()));
  write_u32(out, central_size);
  write_u32(out, central_offset);
  write_u16(out, 0);
  out.flush();
  const bool out_ok = static_cast<bool>(out);
  out.close();
  if (!out_ok) {
    std::error_code error;
    std::filesystem::remove(output, error);
    ok_ = false;
    return {};
  }

  std::error_code error;
  std::filesystem::remove(sheet_path_, error);
  sheet_path_.clear();
  ok_ = false;  // 已完成，防止重复调用再追加一次结尾标签
  return output;
}

}  // namespace xiaotu
