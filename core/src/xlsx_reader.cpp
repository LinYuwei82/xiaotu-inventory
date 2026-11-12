#include "xiaotu/xlsx_reader.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <vector>

#include "miniz.h"
#include "xiaotu/error.h"

namespace xiaotu {
namespace {

constexpr std::size_t kMaxEntryBytes = 64U << 20U;   // 单个部件解压上限
constexpr std::size_t kMaxTotalBytes = 128U << 20U;  // 全部部件解压累计上限

std::uint16_t le16(const unsigned char* data) {
  return static_cast<std::uint16_t>(data[0] | (static_cast<std::uint16_t>(data[1]) << 8));
}

std::uint32_t le32(const unsigned char* data) {
  return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8) |
         (static_cast<std::uint32_t>(data[2]) << 16) |
         (static_cast<std::uint32_t>(data[3]) << 24);
}

struct ZipEntry {
  std::string name;
  std::uint16_t flags = 0;
  std::uint16_t method = 0;
  std::uint32_t compressed = 0;
  std::uint32_t uncompressed = 0;
  std::uint32_t local_offset = 0;
};

struct ZipArchive {
  std::string_view bytes;
  std::vector<ZipEntry> entries;

  const ZipEntry* find(std::string_view name) const {
    for (const auto& entry : entries) {
      if (entry.name == name) return &entry;
    }
    return nullptr;
  }

  std::string extract(const ZipEntry& entry) const;
};

[[noreturn]] void invalid(const std::string& message) {
  throw AppError("IMPORT_INVALID", message);
}

std::vector<ZipEntry> read_central_directory(std::string_view bytes) {
  if (bytes.size() < 22) invalid("文件不是有效的 xlsx（ZIP 过短）");
  const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
  const std::size_t min_offset = bytes.size() > 22 + 65535 ? bytes.size() - 22 - 65535 : 0;
  std::size_t eocd = std::string_view::npos;
  for (std::size_t i = bytes.size() - 22;; --i) {
    if (le32(data + i) == 0x06054b50u) {
      eocd = i;
      break;
    }
    if (i == min_offset) break;
  }
  if (eocd == std::string_view::npos) invalid("文件不是有效的 xlsx（找不到 ZIP 目录）");

  const std::uint16_t count = le16(data + eocd + 10);
  const std::uint32_t offset = le32(data + eocd + 16);
  if (count == 0xFFFFu || offset == 0xFFFFFFFFu) invalid("暂不支持 ZIP64 格式的 xlsx");
  if (offset > bytes.size()) invalid("xlsx 的 ZIP 目录位置无效");

  std::vector<ZipEntry> entries;
  entries.reserve(count);
  std::size_t position = offset;
  for (std::uint16_t i = 0; i < count; ++i) {
    if (position + 46 > bytes.size() || le32(data + position) != 0x02014b50u) {
      invalid("xlsx 的 ZIP 目录损坏");
    }
    const std::uint16_t name_length = le16(data + position + 28);
    const std::uint16_t extra_length = le16(data + position + 30);
    const std::uint16_t comment_length = le16(data + position + 32);
    const std::size_t next =
        position + 46 + static_cast<std::size_t>(name_length) + extra_length + comment_length;
    if (next > bytes.size()) invalid("xlsx 的 ZIP 目录损坏");
    ZipEntry entry;
    entry.flags = le16(data + position + 8);
    entry.method = le16(data + position + 10);
    entry.compressed = le32(data + position + 20);
    entry.uncompressed = le32(data + position + 24);
    entry.local_offset = le32(data + position + 42);
    entry.name.assign(bytes.substr(position + 46, name_length));
    entries.push_back(std::move(entry));
    position = next;
  }
  return entries;
}

std::string ZipArchive::extract(const ZipEntry& entry) const {
  const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
  if ((entry.flags & 0x0001u) != 0) invalid("xlsx 已加密，无法读取");
  if (entry.local_offset + 30 > bytes.size() ||
      le32(data + entry.local_offset) != 0x04034b50u) {
    invalid("xlsx 的 ZIP 数据头损坏");
  }
  const std::uint16_t name_length = le16(data + entry.local_offset + 26);
  const std::uint16_t extra_length = le16(data + entry.local_offset + 28);
  const std::size_t start = entry.local_offset + 30 + static_cast<std::size_t>(name_length) +
                            extra_length;
  if (start + entry.compressed > bytes.size()) invalid("xlsx 的 ZIP 数据越界");
  if (entry.uncompressed > kMaxEntryBytes) invalid("xlsx 内容过大，请拆分后再导入");

  const std::string_view compressed = bytes.substr(start, entry.compressed);
  if (entry.method == 0) {
    if (compressed.size() != entry.uncompressed) invalid("xlsx 的 ZIP 数据长度不一致");
    return std::string(compressed);
  }
  if (entry.method != 8) invalid("xlsx 使用了不支持的压缩方式");
  if (entry.uncompressed == 0) return {};

  // ZIP 里的 deflate 是裸流（没有 zlib 头），用 tinfl 的内存接口解压。
  std::string output(entry.uncompressed, '\0');
  const std::size_t written = tinfl_decompress_mem_to_mem(
      reinterpret_cast<void*>(output.data()), output.size(),
      reinterpret_cast<const void*>(compressed.data()), compressed.size(), 0);
  if (written == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED || written != output.size()) {
    invalid("xlsx 解压失败（文件可能已损坏）");
  }
  return output;
}

bool is_space(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

// 找 <name ...> 的开始位置；要求标签名后紧跟空白或 '>'，避免匹配到 <sheets>。
std::size_t find_open_tag(std::string_view xml, std::size_t from, std::string_view name) {
  std::size_t position = from;
  const std::string needle = "<" + std::string(name);
  while ((position = xml.find(needle, position)) != std::string_view::npos) {
    const std::size_t after = position + needle.size();
    if (after < xml.size() &&
        (is_space(xml[after]) || xml[after] == '>' || xml[after] == '/')) {
      return position;
    }
    position = after;
  }
  return std::string_view::npos;
}

// 从 '<' 开始找到标签结束的 '>'，跳过引号内的内容。
std::size_t tag_end(std::string_view xml, std::size_t open) {
  char quote = '\0';
  for (std::size_t i = open + 1; i < xml.size(); ++i) {
    const char ch = xml[i];
    if (quote != '\0') {
      if (ch == quote) quote = '\0';
    } else if (ch == '"' || ch == '\'') {
      quote = ch;
    } else if (ch == '>') {
      return i;
    }
  }
  return std::string_view::npos;
}

std::string attribute(std::string_view tag, std::string_view name) {
  const std::string needle = std::string(name) + "=";
  std::size_t position = 0;
  while ((position = tag.find(needle, position)) != std::string_view::npos) {
    const bool boundary = position == 0 || is_space(tag[position - 1]);
    if (boundary) {
      const std::size_t value_start = position + needle.size();
      if (value_start < tag.size() && (tag[value_start] == '"' || tag[value_start] == '\'')) {
        const char quote = tag[value_start];
        const std::size_t value_end = tag.find(quote, value_start + 1);
        if (value_end != std::string_view::npos) {
          return std::string(tag.substr(value_start + 1, value_end - value_start - 1));
        }
      }
    }
    position += needle.size();
  }
  return {};
}

int hex_value(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

void append_utf8(std::string& out, std::uint32_t code) {
  if (code <= 0x7F) {
    out.push_back(static_cast<char>(code));
  } else if (code <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (code >> 6)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
  } else if (code <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (code >> 12)));
    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
  } else if (code <= 0x10FFFF) {
    out.push_back(static_cast<char>(0xF0 | (code >> 18)));
    out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
  }
}

std::string xml_unescape(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] != '&') {
      out.push_back(value[i]);
      continue;
    }
    const std::size_t end = value.find(';', i + 1);
    if (end == std::string_view::npos) {
      out.push_back(value[i]);
      continue;
    }
    const std::string_view entity = value.substr(i + 1, end - i - 1);
    if (entity == "amp") {
      out.push_back('&');
    } else if (entity == "lt") {
      out.push_back('<');
    } else if (entity == "gt") {
      out.push_back('>');
    } else if (entity == "quot") {
      out.push_back('"');
    } else if (entity == "apos") {
      out.push_back('\'');
    } else if (!entity.empty() && entity[0] == '#') {
      std::uint32_t code = 0;
      bool valid = false;
      if (entity.size() > 2 && (entity[1] == 'x' || entity[1] == 'X')) {
        for (std::size_t k = 2; k < entity.size(); ++k) {
          const int digit = hex_value(entity[k]);
          if (digit < 0) {
            valid = false;
            break;
          }
          code = code * 16 + static_cast<std::uint32_t>(digit);
          valid = true;
        }
      } else if (entity.size() > 1) {
        for (std::size_t k = 1; k < entity.size(); ++k) {
          if (!std::isdigit(static_cast<unsigned char>(entity[k]))) {
            valid = false;
            break;
          }
          code = code * 10 + static_cast<std::uint32_t>(entity[k] - '0');
          valid = true;
        }
      }
      if (valid) {
        append_utf8(out, code);
      } else {
        out.append(value.substr(i, end - i + 1));
      }
    } else {
      out.append(value.substr(i, end - i + 1));
    }
    i = end;
  }
  return out;
}

// 去掉注音（<rPh>）区间，避免把日文注音当成正文。
std::string strip_phonetic(std::string text) {
  std::size_t position = 0;
  while ((position = text.find("<rPh", position)) != std::string::npos) {
    const std::size_t end = text.find("</rPh>", position);
    if (end == std::string::npos) break;
    text.erase(position, end + 6 - position);
  }
  return text;
}

// 拼接一段 XML 中所有 <t> 的文本（富文本按段拼接）。
std::string collect_text(std::string_view xml) {
  const std::string cleaned = strip_phonetic(std::string(xml));
  std::string out;
  std::size_t position = 0;
  while (true) {
    const std::size_t open = find_open_tag(cleaned, position, "t");
    if (open == std::string_view::npos) break;
    const std::size_t end_of_tag = tag_end(cleaned, open);
    if (end_of_tag == std::string_view::npos) break;
    if (cleaned[end_of_tag - 1] == '/') {
      position = end_of_tag + 1;
      continue;
    }
    const std::size_t close = cleaned.find("</t>", end_of_tag + 1);
    if (close == std::string_view::npos) break;
    out += xml_unescape(
        std::string_view(cleaned).substr(end_of_tag + 1, close - end_of_tag - 1));
    position = close + 4;
  }
  return out;
}

std::vector<std::string> parse_shared_strings(const ZipArchive& archive) {
  std::vector<std::string> values;
  const ZipEntry* entry = archive.find("xl/sharedStrings.xml");
  if (entry == nullptr) return values;

  const std::string xml = archive.extract(*entry);
  std::size_t position = 0;
  while (true) {
    const std::size_t open = find_open_tag(xml, position, "si");
    if (open == std::string_view::npos) break;
    const std::size_t end_of_tag = tag_end(xml, open);
    if (end_of_tag == std::string_view::npos) invalid("xlsx 的共享字符串损坏");
    if (xml[end_of_tag - 1] == '/') {
      values.emplace_back();
      position = end_of_tag + 1;
      continue;
    }
    const std::size_t close = xml.find("</si>", end_of_tag + 1);
    if (close == std::string::npos) invalid("xlsx 的共享字符串损坏");
    values.push_back(collect_text(
        std::string_view(xml).substr(end_of_tag + 1, close - end_of_tag - 1)));
    position = close + 5;
  }
  return values;
}

std::string resolve_first_sheet(const ZipArchive& archive) {
  const ZipEntry* workbook = archive.find("xl/workbook.xml");
  if (workbook == nullptr) invalid("xlsx 缺少 workbook.xml");
  const std::string workbook_xml = archive.extract(*workbook);

  std::string relationship;
  const std::size_t sheet = find_open_tag(workbook_xml, 0, "sheet");
  if (sheet != std::string_view::npos) {
    const std::size_t end_of_tag = tag_end(workbook_xml, sheet);
    if (end_of_tag == std::string_view::npos) invalid("xlsx 的 workbook.xml 损坏");
    relationship =
        attribute(std::string_view(workbook_xml).substr(sheet, end_of_tag - sheet + 1), "r:id");
  }

  if (!relationship.empty()) {
    const ZipEntry* rels = archive.find("xl/_rels/workbook.xml.rels");
    if (rels != nullptr) {
      const std::string rels_xml = archive.extract(*rels);
      std::size_t position = 0;
      while (true) {
        const std::size_t open = find_open_tag(rels_xml, position, "Relationship");
        if (open == std::string_view::npos) break;
        const std::size_t end_of_tag = tag_end(rels_xml, open);
        if (end_of_tag == std::string_view::npos) break;
        const std::string_view tag = std::string_view(rels_xml).substr(open, end_of_tag - open + 1);
        if (attribute(tag, "Id") == relationship) {
          std::string target = attribute(tag, "Target");
          if (target.empty()) break;
          if (target.front() == '/') {
            target.erase(target.begin());
          } else {
            target = "xl/" + target;
          }
          return target;
        }
        position = end_of_tag + 1;
      }
    }
  }

  if (archive.find("xl/worksheets/sheet1.xml") != nullptr) return "xl/worksheets/sheet1.xml";
  invalid("xlsx 中找不到工作表");
}

int column_index(std::string_view reference) {
  int index = 0;
  for (const char ch : reference) {
    if (ch >= 'A' && ch <= 'Z') {
      index = index * 26 + (ch - 'A' + 1);
    } else if (ch >= 'a' && ch <= 'z') {
      index = index * 26 + (ch - 'a' + 1);
    } else {
      break;
    }
  }
  return index - 1;
}

std::string column_letters(std::size_t index) {
  std::string name;
  for (++index; index > 0; index = (index - 1) / 26) {
    name.insert(name.begin(), static_cast<char>('A' + (index - 1) % 26));
  }
  return name;
}

struct ParsedCell {
  std::size_t column = 0;
  std::string value;
};

std::vector<ParsedCell> parse_row_cells(std::string_view xml,
                                        const std::vector<std::string>& shared) {
  std::vector<ParsedCell> cells;
  std::size_t position = 0;
  std::size_t next_column = 0;
  while (true) {
    const std::size_t open = find_open_tag(xml, position, "c");
    if (open == std::string_view::npos) break;
    const std::size_t end_of_tag = tag_end(xml, open);
    if (end_of_tag == std::string_view::npos) break;
    const std::string_view tag = xml.substr(open, end_of_tag - open + 1);
    const std::string reference = attribute(tag, "r");
    std::size_t column = reference.empty() ? next_column
                                           : static_cast<std::size_t>(std::max(0, column_index(reference)));
    const std::string type = attribute(tag, "t");

    std::string value;
    if (xml[end_of_tag - 1] != '/') {
      const std::size_t close = xml.find("</c>", end_of_tag + 1);
      if (close == std::string_view::npos) break;
      const std::string_view content = xml.substr(end_of_tag + 1, close - end_of_tag - 1);
      if (type == "inlineStr") {
        const std::size_t is = content.find("<is");
        if (is != std::string_view::npos) value = collect_text(content.substr(is));
      } else {
        const std::size_t v_open = find_open_tag(content, 0, "v");
        if (v_open != std::string_view::npos) {
          const std::size_t v_end = tag_end(content, v_open);
          if (v_end != std::string_view::npos && content[v_end - 1] != '/') {
            const std::size_t v_close = content.find("</v>", v_end + 1);
            if (v_close != std::string_view::npos) {
              value = xml_unescape(content.substr(v_end + 1, v_close - v_end - 1));
            }
          }
        }
        if (type == "s" && !value.empty()) {
          try {
            const std::size_t shared_index = static_cast<std::size_t>(std::stoull(value));
            if (shared_index >= shared.size()) invalid("xlsx 的共享字符串索引越界");
            value = shared[shared_index];
          } catch (const std::exception&) {
            invalid("xlsx 的共享字符串索引无效");
          }
        }
      }
      position = close + 4;
    } else {
      position = end_of_tag + 1;
    }
    cells.push_back(ParsedCell{column, std::move(value)});
    next_column = column + 1;
  }
  return cells;
}

std::vector<SheetRow> parse_sheet_rows(const std::string& xml,
                                       const std::vector<std::string>& shared) {
  const std::size_t data_open = xml.find("<sheetData");
  if (data_open == std::string::npos) invalid("xlsx 的工作表缺少 sheetData");
  const std::size_t data_start = xml.find('>', data_open);
  const std::size_t data_end = xml.find("</sheetData>", data_start);
  if (data_start == std::string::npos || data_end == std::string::npos) {
    invalid("xlsx 的工作表损坏");
  }

  std::vector<SheetRow> rows;
  std::size_t position = data_start + 1;
  int last_number = 0;
  while (position < data_end) {
    const std::size_t open = find_open_tag(xml, position, "row");
    if (open == std::string::npos || open >= data_end) break;
    const std::size_t end_of_tag = tag_end(xml, open);
    if (end_of_tag == std::string::npos || end_of_tag >= data_end) break;
    const std::string_view tag = std::string_view(xml).substr(open, end_of_tag - open + 1);
    int number = last_number + 1;
    const std::string reference = attribute(tag, "r");
    if (!reference.empty()) {
      try {
        number = std::stoi(reference);
      } catch (const std::exception&) {
        number = last_number + 1;
      }
    }
    last_number = number;

    std::vector<ParsedCell> cells;
    if (xml[end_of_tag - 1] != '/') {
      const std::size_t close = xml.find("</row>", end_of_tag + 1);
      if (close == std::string::npos || close > data_end) break;
      cells = parse_row_cells(
          std::string_view(xml).substr(end_of_tag + 1, close - end_of_tag - 1), shared);
      position = close + 6;
    } else {
      position = end_of_tag + 1;
    }

    std::size_t width = 0;
    bool non_empty = false;
    for (const auto& cell : cells) {
      width = std::max(width, cell.column + 1);
      if (!trim_text(cell.value).empty()) non_empty = true;
    }
    if (!non_empty) continue;

    SheetRow row;
    row.number = number;
    row.cells.assign(width, std::string{});
    for (const auto& cell : cells) row.cells[cell.column] = cell.value;
    rows.push_back(std::move(row));
  }
  return rows;
}

}  // namespace

std::string trim_text(std::string_view value) {
  std::size_t start = 0;
  std::size_t end = value.size();
  while (start < end && is_space(value[start])) ++start;
  while (end > start && is_space(value[end - 1])) --end;
  return std::string(value.substr(start, end - start));
}

SheetTable read_first_sheet(std::string_view bytes) {
  ZipArchive archive;
  archive.bytes = bytes;
  archive.entries = read_central_directory(bytes);

  const std::string sheet_path = resolve_first_sheet(archive);
  const ZipEntry* sheet = archive.find(sheet_path);
  if (sheet == nullptr) invalid("xlsx 中找不到工作表文件: " + sheet_path);

  std::size_t total = 0;
  for (const auto& entry : archive.entries) {
    total += entry.uncompressed;
    if (total > kMaxTotalBytes) invalid("xlsx 解压后体积过大，请拆分后再导入");
  }

  const std::string sheet_xml = archive.extract(*sheet);
  if (sheet_xml.find("<mergeCell") != std::string::npos ||
      sheet_xml.find("<mergeCells") != std::string::npos) {
    invalid("文件中存在合并单元格，请先取消合并再导入");
  }
  const auto shared = parse_shared_strings(archive);
  auto rows = parse_sheet_rows(sheet_xml, shared);

  SheetTable table;
  auto header_row = std::find_if(rows.begin(), rows.end(),
                                 [](const SheetRow& row) { return row.number == 1; });
  if (header_row == rows.end()) invalid("第 1 行必须是表头");
  table.header = header_row->cells;
  for (auto& cell : table.header) cell = trim_text(cell);
  while (!table.header.empty() && table.header.back().empty()) table.header.pop_back();
  if (table.header.empty()) invalid("第 1 行必须是表头，且不能为空");

  for (auto& row : rows) {
    if (row.number == 1) continue;
    if (row.cells.size() > table.header.size()) {
      table.header.resize(row.cells.size());
    }
    table.rows.push_back(std::move(row));
  }
  return table;
}

std::size_t ColumnMap::index(std::string_view column) const {
  const auto it = index_.find(std::string(column));
  if (it == index_.end()) invalid("表头缺少列「" + std::string(column) + "」");
  return it->second;
}

bool ColumnMap::has(std::string_view column) const {
  return index_.find(std::string(column)) != index_.end();
}

std::string ColumnMap::cell(const SheetRow& row, std::string_view column) const {
  const auto it = index_.find(std::string(column));
  if (it == index_.end() || it->second >= row.cells.size()) return {};
  return trim_text(row.cells[it->second]);
}

ColumnMap map_columns(const std::vector<std::string>& header,
                      const std::vector<std::string>& known,
                      const std::vector<std::string>& required) {
  std::set<std::string> allowed(known.begin(), known.end());
  ColumnMap map;
  for (std::size_t i = 0; i < header.size(); ++i) {
    const std::string& name = header[i];
    if (name.empty()) {
      invalid("表头第 " + column_letters(i) + " 列没有列名");
    }
    if (allowed.count(name) == 0) {
      std::string available;
      for (const auto& item : known) {
        if (!available.empty()) available += "、";
        available += item;
      }
      invalid("表头「" + name + "」不是可导入的列；可用列：" + available);
    }
    if (map.index_.count(name) > 0) {
      invalid("表头「" + name + "」重复出现");
    }
    map.index_[name] = i;
  }
  for (const auto& name : required) {
    if (map.index_.count(name) == 0) invalid("缺少必填列「" + name + "」");
  }
  return map;
}

}  // namespace xiaotu
