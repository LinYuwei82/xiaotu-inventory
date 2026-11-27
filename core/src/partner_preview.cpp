#include "xiaotu/partner_preview.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <sstream>

#include "xiaotu/csv.h"
#include "xiaotu/money.h"

namespace xiaotu {
namespace {

struct ArchiveRow {
  PartnerPreviewRow row;
  std::int64_t archive_id = 0;
};

struct LedgerAgg {
  std::string key;
  std::string name;
  std::int64_t ar_open = 0;
  std::int64_t ap_open = 0;
  std::int64_t ar_docs = 0;
  std::int64_t ap_docs = 0;
  std::int64_t opening_rows = 0;
};

std::u32string decode_utf8(std::string_view text) {
  std::u32string out;
  out.reserve(text.size());
  std::size_t i = 0;
  while (i < text.size()) {
    const auto lead = static_cast<unsigned char>(text[i]);
    char32_t cp = 0;
    std::size_t extra = 0;
    if (lead < 0x80) {
      cp = lead;
      extra = 0;
    } else if ((lead & 0xE0) == 0xC0) {
      cp = lead & 0x1F;
      extra = 1;
    } else if ((lead & 0xF0) == 0xE0) {
      cp = lead & 0x0F;
      extra = 2;
    } else if ((lead & 0xF8) == 0xF0) {
      cp = lead & 0x07;
      extra = 3;
    } else {
      ++i;
      continue;
    }
    if (i + extra >= text.size() + (extra == 0 ? 1 : 0)) break;
    bool valid = true;
    for (std::size_t k = 1; k <= extra; ++k) {
      const auto ch = static_cast<unsigned char>(text[i + k]);
      if ((ch & 0xC0) != 0x80) {
        valid = false;
        break;
      }
      cp = (cp << 6) | (ch & 0x3F);
    }
    if (!valid) {
      ++i;
      continue;
    }
    out.push_back(cp);
    i += extra + 1;
  }
  return out;
}

std::string encode_utf8(const std::u32string& text) {
  std::string out;
  out.reserve(text.size() * 3);
  for (char32_t cp : text) {
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
  return out;
}

// 全角转半角、英文转大写、空白折叠；比较口径与方案第 3.1 节一致。
std::string normalize_common(std::string_view text) {
  const std::u32string decoded = decode_utf8(text);
  std::u32string buffer;
  buffer.reserve(decoded.size());
  for (char32_t cp : decoded) {
    if (cp >= 0xFF01 && cp <= 0xFF5E) cp -= 0xFEE0;
    if (cp == 0x3000) cp = U' ';
    if (cp >= U'a' && cp <= U'z') cp = cp - U'a' + U'A';
    if (cp == U'\t' || cp == U'\n' || cp == U'\r') cp = U' ';
    buffer.push_back(cp);
  }
  std::u32string collapsed;
  bool previous_space = true;
  for (char32_t cp : buffer) {
    if (cp == U' ') {
      if (!previous_space) collapsed.push_back(cp);
      previous_space = true;
    } else {
      collapsed.push_back(cp);
      previous_space = false;
    }
  }
  while (!collapsed.empty() && collapsed.back() == U' ') collapsed.pop_back();
  return encode_utf8(collapsed);
}

std::string normalize_name(std::string_view text) { return normalize_common(text); }

std::string normalize_tax_no(std::string_view text) {
  const std::string common = normalize_common(text);
  std::string out;
  for (const char ch : common) {
    if (ch != ' ' && ch != '-') out.push_back(ch);
  }
  return out;
}

std::string normalize_phone(std::string_view text) {
  const std::string common = normalize_common(text);
  std::string out;
  for (const char ch : common) {
    if (ch != ' ' && ch != '-' && ch != '(' && ch != ')' && ch != '+') out.push_back(ch);
  }
  if (out.size() > 11 && out.rfind("86", 0) == 0) out = out.substr(2);
  return out;
}

PreviewSide side_of_key(std::string_view key) {
  if (key.rfind("C:", 0) == 0) return PreviewSide::Customer;
  if (key.rfind("S:", 0) == 0) return PreviewSide::Supplier;
  if (key.rfind("P:", 0) == 0) return PreviewSide::Partner;
  return PreviewSide::LedgerOnly;
}

std::string json_escape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (const char ch : text) {
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out.push_back(ch);
    }
  }
  return out;
}

std::string origin_label(std::string_view origin) {
  if (origin == "AUTO_ARCHIVE") return "自动补建";
  if (origin == "MANUAL") return "手工/合并";
  return origin.empty() ? "—" : std::string(origin);
}

std::string row_label(const PartnerPreviewRow& row) {
  std::ostringstream out;
  out << preview_side_label(row.side) << " " << (row.code.empty() ? "（无编号）" : row.code) << " "
      << row.name << " | 税号 " << (row.tax_no.empty() ? "—" : row.tax_no) << " | 电话 "
      << (row.phone.empty() ? "—" : row.phone) << " | 应收 " << format_cents(row.ar_open_cents)
      << " | 应付 " << format_cents(row.ap_open_cents) << " | 单据 " << row.ar_docs << "/"
      << row.ap_docs << " | 期初 " << row.opening_rows << " | 键 "
      << (row.partner_key.empty() ? "—" : row.partner_key) << " | 来源 "
      << origin_label(row.origin);
  return out.str();
}

std::int64_t count_archive_rows(const PartnerPreviewGroup& group) {
  std::int64_t count = 0;
  for (const auto& row : group.rows) {
    if (row.side == PreviewSide::Customer || row.side == PreviewSide::Supplier) ++count;
  }
  return count;
}

}  // namespace

std::string_view preview_side_label(PreviewSide side) {
  switch (side) {
    case PreviewSide::Customer: return "客户";
    case PreviewSide::Supplier: return "供应商";
    case PreviewSide::Partner: return "往来单位";
    case PreviewSide::LedgerOnly: return "无档案流水";
  }
  return "未知";
}

std::string_view preview_match_label(PreviewMatch match) {
  switch (match) {
    case PreviewMatch::TaxNo: return "税号相同";
    case PreviewMatch::NamePhone: return "名称+电话相同";
    case PreviewMatch::NameOnly: return "仅名称相同";
  }
  return "未知";
}

PartnerPreviewReport partner_merge_preview(const Db& db) {
  PartnerPreviewReport report;
  std::vector<ArchiveRow> archives;
  std::map<std::string, LedgerAgg> ledger;
  std::map<std::string, std::string> origin_by_key;

  db.read([&](Conn& conn) {
    {
      Stmt stmt(conn,
                "SELECT id, code, name, COALESCE(tax_no, ''), COALESCE(phone, '')"
                " FROM customer c WHERE c.is_deleted = 0"
                " AND NOT EXISTS (SELECT 1 FROM partner p"
                "                 WHERE p.id = c.id AND p.merged_into <> 0)"
                " ORDER BY code;");
      while (stmt.step()) {
        ArchiveRow row;
        row.archive_id = stmt.i64(0);
        row.row.side = PreviewSide::Customer;
        row.row.code = stmt.text(1);
        row.row.name = stmt.text(2);
        row.row.tax_no = stmt.text(3);
        row.row.phone = stmt.text(4);
        row.row.partner_key = "C:" + std::to_string(row.archive_id);
        archives.push_back(std::move(row));
      }
    }
    {
      Stmt stmt(conn,
                "SELECT id, code, name, COALESCE(tax_no, ''), COALESCE(phone, '')"
                " FROM supplier s WHERE s.is_deleted = 0"
                " AND NOT EXISTS (SELECT 1 FROM partner_legacy_map m"
                "                 JOIN partner p ON p.id = m.partner_id"
                "                 WHERE m.side = 'supplier' AND m.legacy_id = s.id"
                "                   AND p.merged_into <> 0)"
                " ORDER BY code;");
      while (stmt.step()) {
        ArchiveRow row;
        row.archive_id = stmt.i64(0);
        row.row.side = PreviewSide::Supplier;
        row.row.code = stmt.text(1);
        row.row.name = stmt.text(2);
        row.row.tax_no = stmt.text(3);
        row.row.phone = stmt.text(4);
        row.row.partner_key = "S:" + std::to_string(row.archive_id);
        archives.push_back(std::move(row));
      }
    }
    Stmt stmt(conn,
              "SELECT partner_key, partner_name,"
              " SUM(CASE WHEN direction = 1 AND status <> 'VOIDED'"
              "          THEN amount_cents - settled_cents ELSE 0 END),"
              " SUM(CASE WHEN direction = -1 AND status <> 'VOIDED'"
              "          THEN amount_cents - settled_cents ELSE 0 END),"
              " COUNT(DISTINCT CASE WHEN direction = 1 AND doc_id > 0 THEN doc_id END),"
              " COUNT(DISTINCT CASE WHEN direction = -1 AND doc_id > 0 THEN doc_id END),"
              " SUM(CASE WHEN doc_id = 0 THEN 1 ELSE 0 END)"
              " FROM ar_ap_entry GROUP BY partner_key, partner_name;");
    while (stmt.step()) {
      LedgerAgg agg;
      agg.key = stmt.text(0);
      agg.name = stmt.text(1);
      agg.ar_open = stmt.i64(2);
      agg.ap_open = stmt.i64(3);
      agg.ar_docs = stmt.i64(4);
      agg.ap_docs = stmt.i64(5);
      agg.opening_rows = stmt.i64(6);
      ledger[agg.key] = agg;
    }
    // 来源标记：档案键与 P: 键都映射到 partner.origin，供归并时区分业务补建与手工记录。
    Stmt origin_stmt(conn,
                     "SELECT 'C:' || c.id, COALESCE(p.origin, '')"
                     " FROM customer c LEFT JOIN partner p ON p.id = c.id"
                     " UNION ALL"
                     " SELECT 'S:' || m.legacy_id, COALESCE(p.origin, '')"
                     " FROM partner_legacy_map m JOIN partner p ON p.id = m.partner_id"
                     " UNION ALL"
                     " SELECT 'P:' || p.id, p.origin FROM partner p;");
    while (origin_stmt.step()) {
      origin_by_key[origin_stmt.text(0)] = origin_stmt.text(1);
    }
  });

  for (auto& archive : archives) {
    const auto found = origin_by_key.find(archive.row.partner_key);
    if (found != origin_by_key.end()) archive.row.origin = found->second;
  }

  report.customer_total = static_cast<std::int64_t>(std::count_if(
      archives.begin(), archives.end(),
      [](const ArchiveRow& row) { return row.row.side == PreviewSide::Customer; }));
  report.supplier_total = static_cast<std::int64_t>(archives.size()) - report.customer_total;
  report.ledger_key_total = static_cast<std::int64_t>(ledger.size());

  std::map<std::string, LedgerAgg> orphan_ledger = ledger;
  for (auto& archive : archives) {
    const auto found = ledger.find(archive.row.partner_key);
    if (found == ledger.end()) continue;
    archive.row.ar_open_cents = found->second.ar_open;
    archive.row.ap_open_cents = found->second.ap_open;
    archive.row.ar_docs = found->second.ar_docs;
    archive.row.ap_docs = found->second.ap_docs;
    archive.row.opening_rows = found->second.opening_rows;
    orphan_ledger.erase(archive.row.partner_key);
  }

  // 每个档案最多归入一个组：税号优先，其次名称+电话，最后仅名称。
  std::vector<int> group_of(archives.size(), -1);
  auto add_group = [&](PreviewMatch match, std::string value,
                       const std::vector<std::size_t>& members) {
    PartnerPreviewGroup group;
    group.match = match;
    group.match_value = std::move(value);
    for (const std::size_t index : members) {
      group.rows.push_back(archives[index].row);
      group_of[index] = static_cast<int>(report.groups.size());
    }
    report.groups.push_back(std::move(group));
  };

  {
    std::map<std::string, std::vector<std::size_t>> by_tax;
    for (std::size_t i = 0; i < archives.size(); ++i) {
      const std::string tax = normalize_tax_no(archives[i].row.tax_no);
      if (!tax.empty()) by_tax[tax].push_back(i);
    }
    for (const auto& [tax, members] : by_tax) {
      if (members.size() >= 2) add_group(PreviewMatch::TaxNo, tax, members);
    }
  }
  {
    std::map<std::string, std::vector<std::size_t>> by_phone;
    for (std::size_t i = 0; i < archives.size(); ++i) {
      if (group_of[i] >= 0) continue;
      const std::string name = normalize_name(archives[i].row.name);
      const std::string phone = normalize_phone(archives[i].row.phone);
      if (!name.empty() && !phone.empty()) by_phone[name + '\x1f' + phone].push_back(i);
    }
    for (const auto& [key, members] : by_phone) {
      if (members.size() < 2) continue;
      const std::string value = normalize_name(archives[members.front()].row.name) + " / " +
                                normalize_phone(archives[members.front()].row.phone);
      add_group(PreviewMatch::NamePhone, value, members);
    }
  }
  {
    std::map<std::string, std::vector<std::size_t>> by_name;
    for (std::size_t i = 0; i < archives.size(); ++i) {
      if (group_of[i] >= 0) continue;
      const std::string name = normalize_name(archives[i].row.name);
      if (!name.empty()) by_name[name].push_back(i);
    }
    for (const auto& [name, members] : by_name) {
      if (members.size() >= 2) add_group(PreviewMatch::NameOnly, name, members);
    }
  }

  // 只有流水、没有档案的 key：同名档案存在时并入候选组，暴露 C:/S:/T: 并存。
  for (const auto& [key, agg] : orphan_ledger) {
    PartnerPreviewRow row;
    row.side = side_of_key(key);
    row.name = agg.name;
    row.partner_key = key;
    const auto origin = origin_by_key.find(key);
    if (origin != origin_by_key.end()) row.origin = origin->second;
    row.ar_open_cents = agg.ar_open;
    row.ap_open_cents = agg.ap_open;
    row.ar_docs = agg.ar_docs;
    row.ap_docs = agg.ap_docs;
    row.opening_rows = agg.opening_rows;
    const std::string norm = normalize_name(agg.name);
    if (norm.empty()) continue;
    std::vector<std::size_t> matches;
    std::set<int> groups;
    for (std::size_t i = 0; i < archives.size(); ++i) {
      if (normalize_name(archives[i].row.name) != norm) continue;
      matches.push_back(i);
      if (group_of[i] >= 0) groups.insert(group_of[i]);
    }
    if (matches.size() == 1) {
      const std::size_t index = matches.front();
      if (group_of[index] >= 0) {
        report.groups[static_cast<std::size_t>(group_of[index])].rows.push_back(row);
      } else {
        add_group(PreviewMatch::NameOnly, norm, matches);
        report.groups.back().rows.push_back(row);
      }
    } else if (!matches.empty() && groups.size() == 1) {
      report.groups[static_cast<std::size_t>(*groups.begin())].rows.push_back(row);
    } else if (!matches.empty() && groups.empty()) {
      add_group(PreviewMatch::NameOnly, norm, matches);
      report.groups.back().rows.push_back(row);
    }
    // 命中多个组时不强行并入，交给多 key 清单呈现。
  }

  for (auto& group : report.groups) {
    std::set<std::string> keys;
    for (const auto& row : group.rows) {
      if (!row.partner_key.empty()) keys.insert(row.partner_key);
    }
    group.keys_conflict = keys.size() >= 2;
    report.group_records += count_archive_rows(group);
    if (group.match == PreviewMatch::TaxNo) ++report.high_count;
    else if (group.match == PreviewMatch::NamePhone) ++report.medium_count;
    else ++report.name_only_count;
  }

  // 同一名称出现两个及以上 partner_key：这才是台账真正会分裂的地方。
  {
    std::map<std::string, std::vector<const LedgerAgg*>> by_name;
    for (const auto& [key, agg] : ledger) {
      const std::string norm = normalize_name(agg.name);
      if (norm.empty()) continue;
      by_name[norm].push_back(&agg);
    }
    for (const auto& [norm, aggs] : by_name) {
      std::set<std::string> keys;
      for (const LedgerAgg* agg : aggs) keys.insert(agg->key);
      if (keys.size() < 2) continue;
      PartnerMultiKeyUnit unit;
      unit.name = aggs.front()->name;
      for (const LedgerAgg* agg : aggs) {
        PartnerPreviewRow row;
        row.side = side_of_key(agg->key);
        row.name = agg->name;
        row.partner_key = agg->key;
        const auto origin = origin_by_key.find(agg->key);
        if (origin != origin_by_key.end()) row.origin = origin->second;
        row.ar_open_cents = agg->ar_open;
        row.ap_open_cents = agg->ap_open;
        row.ar_docs = agg->ar_docs;
        row.ap_docs = agg->ap_docs;
        row.opening_rows = agg->opening_rows;
        unit.keys.push_back(std::move(row));
      }
      std::sort(unit.keys.begin(), unit.keys.end(),
                [](const PartnerPreviewRow& left, const PartnerPreviewRow& right) {
                  return left.partner_key < right.partner_key;
                });
      report.multi_key_units.push_back(std::move(unit));
    }
  }

  return report;
}

std::string partner_preview_text(const PartnerPreviewReport& report) {
  std::ostringstream out;
  out << "往来单位归并预览（只读，不写库）\n";
  out << "档案: 客户 " << report.customer_total << " 条，供应商 " << report.supplier_total
      << " 条；台账 key: " << report.ledger_key_total << " 个\n";
  out << "候选合并组: " << report.groups.size() << " 组，涉及档案 " << report.group_records
      << " 条；高置信 " << report.high_count << " 组，中高 " << report.medium_count
      << " 组，仅名称 " << report.name_only_count << " 组\n";
  if (report.groups.empty()) {
    out << "候选合并组: 0 组（现有档案中没有命中税号、名称+电话或同名规则的两条以上记录）\n";
  }
  for (std::size_t i = 0; i < report.groups.size(); ++i) {
    const auto& group = report.groups[i];
    out << "\n[" << preview_match_label(group.match) << "] " << group.match_value
        << (group.keys_conflict ? "（存在多个 partner_key）" : "") << "\n";
    for (const auto& row : group.rows) out << "  " << row_label(row) << "\n";
  }
  out << "\n同一单位存在多个 partner_key: " << report.multi_key_units.size() << " 个单位\n";
  for (const auto& unit : report.multi_key_units) {
    out << "  名称: " << unit.name << "（" << unit.keys.size() << " 个 key）\n";
    for (const auto& row : unit.keys) {
      out << "    " << preview_side_label(row.side) << " " << row.partner_key << " | 应收 "
          << format_cents(row.ar_open_cents) << " | 应付 " << format_cents(row.ap_open_cents)
          << " | 单据 " << row.ar_docs << "/" << row.ap_docs << " | 期初 " << row.opening_rows
          << "\n";
    }
  }
  return out.str();
}

std::string partner_preview_json(const PartnerPreviewReport& report) {
  std::ostringstream out;
  out << "{\"customer_total\":" << report.customer_total
      << ",\"supplier_total\":" << report.supplier_total
      << ",\"ledger_key_total\":" << report.ledger_key_total
      << ",\"groups\":" << report.groups.size()
      << ",\"group_records\":" << report.group_records
      << ",\"high\":" << report.high_count << ",\"medium\":" << report.medium_count
      << ",\"name_only\":" << report.name_only_count << ",\"multi_key_units\":" <<
      report.multi_key_units.size() << ",\"candidates\":[";
  bool first_group = true;
  for (const auto& group : report.groups) {
    if (!first_group) out << ",";
    first_group = false;
    out << "{\"match\":\"" << preview_match_label(group.match) << "\",\"match_value\":\""
        << json_escape(group.match_value) << "\",\"keys_conflict\":"
        << (group.keys_conflict ? "true" : "false") << ",\"rows\":[";
    bool first_row = true;
    for (const auto& row : group.rows) {
      if (!first_row) out << ",";
      first_row = false;
      out << "{\"side\":\"" << preview_side_label(row.side) << "\",\"code\":\""
          << json_escape(row.code) << "\",\"name\":\"" << json_escape(row.name) << "\",\"tax_no\":\""
          << json_escape(row.tax_no) << "\",\"phone\":\"" << json_escape(row.phone)
          << "\",\"ar_open_cents\":" << row.ar_open_cents << ",\"ap_open_cents\":"
          << row.ap_open_cents << ",\"ar_docs\":" << row.ar_docs << ",\"ap_docs\":"
          << row.ap_docs << ",\"opening_rows\":" << row.opening_rows << ",\"partner_key\":\""
          << json_escape(row.partner_key) << "\",\"origin\":\"" << json_escape(row.origin)
          << "\"}";
    }
    out << "]}";
  }
  out << "],\"multi_key\":[";
  bool first_unit = true;
  for (const auto& unit : report.multi_key_units) {
    if (!first_unit) out << ",";
    first_unit = false;
    out << "{\"name\":\"" << json_escape(unit.name) << "\",\"keys\":[";
    bool first_key = true;
    for (const auto& row : unit.keys) {
      if (!first_key) out << ",";
      first_key = false;
      out << "{\"partner_key\":\"" << json_escape(row.partner_key) << "\",\"side\":\""
          << preview_side_label(row.side) << "\",\"ar_open_cents\":" << row.ar_open_cents
          << ",\"ap_open_cents\":" << row.ap_open_cents << ",\"ar_docs\":" << row.ar_docs
          << ",\"ap_docs\":" << row.ap_docs << ",\"opening_rows\":" << row.opening_rows
          << ",\"origin\":\"" << json_escape(row.origin) << "\"}";
    }
    out << "]}";
  }
  out << "]}\n";
  return out.str();
}

std::string partner_preview_csv(const PartnerPreviewReport& report) {
  std::ostringstream out;
  out << "section,match,side,code,name,tax_no,phone,ar_open,ap_open,ar_docs,ap_docs,opening_rows,"
         "partner_key,origin\n";
  for (const auto& group : report.groups) {
    for (const auto& row : group.rows) {
      out << "group," << csv_escape(preview_match_label(group.match)) << ","
          << csv_escape(preview_side_label(row.side)) << "," << csv_escape(row.code) << ","
          << csv_escape(row.name) << "," << csv_escape(row.tax_no) << ","
          << csv_escape(row.phone) << "," << format_cents(row.ar_open_cents) << ","
          << format_cents(row.ap_open_cents) << "," << row.ar_docs << "," << row.ap_docs << ","
          << row.opening_rows << "," << csv_escape(row.partner_key) << ","
          << csv_escape(row.origin) << "\n";
    }
  }
  for (const auto& unit : report.multi_key_units) {
    for (const auto& row : unit.keys) {
      out << "multi_key,multi_key," << csv_escape(preview_side_label(row.side)) << ","
          << csv_escape(row.code) << "," << csv_escape(row.name) << ","
          << csv_escape(row.tax_no) << "," << csv_escape(row.phone) << ","
          << format_cents(row.ar_open_cents) << "," << format_cents(row.ap_open_cents) << ","
          << row.ar_docs << "," << row.ap_docs << "," << row.opening_rows << ","
          << csv_escape(row.partner_key) << "," << csv_escape(row.origin) << "\n";
    }
  }
  return out.str();
}

}  // namespace xiaotu
