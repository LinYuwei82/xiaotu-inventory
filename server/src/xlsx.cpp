#include "xiaotu/xlsx.h"

#include <array>
#include <cstddef>
#include <cstdint>
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

std::uint32_t crc32(const std::string& data) {
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
  std::uint32_t crc = 0xFFFFFFFFu;
  for (const unsigned char ch : data) {
    crc = table[(crc ^ ch) & 0xFFu] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
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

std::string worksheet_xml(const std::vector<std::vector<XlsxCell>>& rows,
                          const std::vector<bool>& header_flags) {
  std::string xml;
  xml +=
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
      "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
      "<sheetViews><sheetView workbookViewId=\"0\"/></sheetViews>"
      "<sheetFormatPr defaultRowHeight=\"15\"/><sheetData>";
  for (std::size_t r = 0; r < rows.size(); ++r) {
    const std::string row_number = std::to_string(r + 1);
    xml += "<row r=\"" + row_number + "\">";
    for (std::size_t c = 0; c < rows[r].size(); ++c) {
      const XlsxCell& cell = rows[r][c];
      const std::string ref = column_name(c) + row_number;
      if (cell.kind == XlsxCell::Kind::Text) {
        xml += "<c r=\"" + ref + "\"";
        if (r < header_flags.size() && header_flags[r]) xml += " s=\"1\"";
        xml += " t=\"inlineStr\"><is><t xml:space=\"preserve\">" + xml_escape(cell.value) +
               "</t></is></c>";
      } else if (cell.kind == XlsxCell::Kind::Integer) {
        xml += "<c r=\"" + ref + "\"><v>" + cell.value + "</v></c>";
      } else {
        xml += "<c r=\"" + ref + "\" s=\"2\"><v>" + cell.value + "</v></c>";
      }
    }
    xml += "</row>";
  }
  xml += "</sheetData></worksheet>";
  return xml;
}

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

}  // namespace xiaotu
