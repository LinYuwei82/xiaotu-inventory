#include "xiaotu/masterdata_io.h"

#include "xiaotu/db.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "xiaotu/audit.h"
#include "xiaotu/backup.h"
#include "xiaotu/crud.h"
#include "xiaotu/customer_service.h"
#include "xiaotu/error.h"
#include "xiaotu/gen/customer_dao.h"
#include "xiaotu/gen/item_dao.h"
#include "xiaotu/gen/supplier_dao.h"
#include "xiaotu/gen/unit_dao.h"
#include "xiaotu/item_barcode.h"
#include "xiaotu/item_category.h"
#include "xiaotu/xlsx_reader.h"

namespace xiaotu {
namespace {

constexpr const char* kItemColumns[] = {"商品编码", "商品名称", "规格", "计量单位",
                                        "分类路径", "条码",   "税率%", "启用"};
constexpr const char* kSupplierColumns[] = {"编码", "名称", "联系人", "电话",
                                            "税号", "地址", "启用"};
constexpr const char* kCustomerColumns[] = {"编码", "名称", "联系人", "电话", "税号",
                                            "信用额度（元）", "地址", "启用"};

bool is_yes(const std::string& value) {
  std::string lower;
  lower.reserve(value.size());
  for (const char ch : value) {
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return value == "是" || value == "启用" || lower == "true" || value == "1";
}

bool is_no(const std::string& value) {
  std::string lower;
  lower.reserve(value.size());
  for (const char ch : value) {
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return value == "否" || value == "停用" || lower == "false" || value == "0";
}

struct BoolCell {
  bool present = false;
  bool value = false;
  bool valid = true;
};

BoolCell parse_bool(const std::string& text) {
  BoolCell cell;
  if (text.empty()) return cell;
  cell.present = true;
  if (is_yes(text)) {
    cell.value = true;
  } else if (is_no(text)) {
    cell.value = false;
  } else {
    cell.valid = false;
  }
  return cell;
}

struct ItemRecord {
  gen::ItemRow row;
  bool deleted = false;
  std::vector<std::string> barcodes;  // 主码在前
};

struct PartnerRecord {
  gen::SupplierRow row;
  bool deleted = false;
  std::string tax_no;
  std::int64_t credit_limit_cents = 0;
};

std::vector<ItemRecord> load_items(Db& db) {
  std::vector<ItemRecord> records;
  db.read([&](Conn& conn) {
    Stmt stmt(conn,
              "SELECT id, code, name, spec, unit, unit_id, status, is_deleted,"
              " category_id, tax_rate_bp, barcode FROM item ORDER BY code;");
    while (stmt.step()) {
      ItemRecord record;
      record.row.id = stmt.i64(0);
      record.row.code = stmt.text(1);
      record.row.name = stmt.text(2);
      record.row.spec = stmt.text(3);
      record.row.unit = stmt.text(4);
      record.row.unit_id = stmt.i64(5);
      record.row.status = stmt.i64(6) != 0;
      record.deleted = stmt.i64(7) != 0;
      record.row.category_id = stmt.i64(8);
      record.row.tax_rate_bp = stmt.i64(9);
      record.row.barcode = stmt.text(10);
      records.push_back(std::move(record));
    }
    std::unordered_map<std::int64_t, std::vector<std::string>> codes;
    Stmt barcode_stmt(conn,
                      "SELECT item_id, barcode FROM item_barcode"
                      " ORDER BY item_id, is_primary DESC, id;");
    while (barcode_stmt.step()) {
      codes[barcode_stmt.i64(0)].push_back(barcode_stmt.text(1));
    }
    for (auto& record : records) {
      const auto found = codes.find(record.row.id);
      if (found != codes.end()) {
        record.barcodes = found->second;
      }
    }
  });
  return records;
}

std::vector<PartnerRecord> load_partners(Db& db, const char* table, bool with_credit) {
  std::vector<PartnerRecord> records;
  db.read([&](Conn& conn) {
    // 客户额度以 partner 为准，导出也读 partner；没有 partner 时回落到镜像。
    const std::string sql =
        with_credit
            ? "SELECT c.id, c.code, c.name, c.contact, c.phone, c.address, c.status,"
              " c.is_deleted, c.tax_no,"
              " COALESCE(p.credit_limit_cents, c.credit_limit_cents)"
              " FROM customer c LEFT JOIN partner p ON p.id = c.id AND p.is_customer = 1"
              " ORDER BY c.code;"
            : "SELECT id, code, name, contact, phone, address, status, is_deleted, tax_no" +
                  std::string(" FROM ") + table + " ORDER BY code;";
    Stmt stmt(conn, sql);
    while (stmt.step()) {
      PartnerRecord record;
      record.row.id = stmt.i64(0);
      record.row.code = stmt.text(1);
      record.row.name = stmt.text(2);
      record.row.contact = stmt.text(3);
      record.row.phone = stmt.text(4);
      record.row.address = stmt.text(5);
      record.row.status = stmt.i64(6) != 0;
      record.deleted = stmt.i64(7) != 0;
      record.tax_no = stmt.text(8);
      record.row.tax_no = record.tax_no;
      if (with_credit) record.credit_limit_cents = stmt.i64(9);
      records.push_back(std::move(record));
    }
  });
  return records;
}

struct UnitLookup {
  std::vector<gen::UnitRow> units;
  std::unordered_map<std::string, std::vector<std::size_t>> by_name;
  std::unordered_map<std::string, std::vector<std::size_t>> by_code;

  std::string resolve(const std::string& text, std::int64_t* id) const {
    const auto by_name_it = by_name.find(text);
    if (by_name_it != by_name.end()) {
      if (by_name_it->second.size() > 1) return "单位名称不唯一，请改为填写单位编码";
      const auto& unit = units[by_name_it->second.front()];
      *id = unit.id;
      return {};
    }
    const auto by_code_it = by_code.find(text);
    if (by_code_it != by_code.end()) {
      if (by_code_it->second.size() > 1) return "单位编码不唯一";
      const auto& unit = units[by_code_it->second.front()];
      *id = unit.id;
      return {};
    }
    return "单位「" + text + "」不存在";
  }
};

UnitLookup load_units(Db& db) {
  UnitLookup lookup;
  gen::UnitDao dao(db);
  lookup.units = dao.list();
  for (std::size_t i = 0; i < lookup.units.size(); ++i) {
    lookup.by_name[lookup.units[i].name].push_back(i);
    lookup.by_code[lookup.units[i].code].push_back(i);
  }
  return lookup;
}

struct ItemPlan {
  int row = 1;
  std::string code;
  const gen::ItemRow* current = nullptr;
  bool has_name = false;
  std::string name;
  bool has_spec = false;
  std::string spec;
  bool has_unit = false;
  std::int64_t unit_id = 0;
  std::string unit_name;
  bool has_category = false;
  std::int64_t category_id = 0;
  bool has_barcode = false;
  std::vector<std::string> barcodes;  // 主码在前
  bool has_tax = false;
  std::int64_t tax_rate_bp = 0;
  bool has_status = false;
  bool status = true;
};

// 分类路径按「父/子/孙」显示名匹配；导入时整批先解析，路径不存在直接拒绝。
struct CategoryLookup {
  std::vector<ItemCategoryRow> rows;
  std::vector<std::string> names;
  std::unordered_map<std::string, std::int64_t> by_name;

  std::string resolve(const std::string& text, std::int64_t* id) const {
    const auto it = by_name.find(text);
    if (it == by_name.end()) {
      return "分类路径「" + text + "」不存在，请先在商品分类中建好";
    }
    *id = it->second;
    return {};
  }
};

CategoryLookup load_categories(Db& db) {
  CategoryLookup lookup;
  ItemCategoryService service(db);
  lookup.rows = service.list();
  lookup.names.resize(lookup.rows.size());
  for (std::size_t i = 0; i < lookup.rows.size(); ++i) {
    std::string name = lookup.rows[i].name;
    const std::string& path = lookup.rows[i].path;
    const std::size_t last = path.find_last_of('/', path.size() - 2);
    if (last != std::string::npos && last != 0) {
      const std::string parent_path = path.substr(0, last + 1);
      for (std::size_t j = 0; j < i; ++j) {
        if (lookup.rows[j].path == parent_path) {
          name = lookup.names[j] + "/" + lookup.rows[i].name;
          break;
        }
      }
    }
    lookup.names[i] = name;
    lookup.by_name.emplace(name, lookup.rows[i].id);
  }
  return lookup;
}

// 百分数与金额都按「最多两位小数的十进制」解析，换成整数（万分比 / 分），不经过浮点。
bool parse_scaled_decimal(const std::string& text, std::int64_t scale, std::int64_t* value) {
  if (text.empty() || scale <= 0) return false;
  std::size_t index = 0;
  while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index]))) ++index;
  std::string whole;
  std::string fraction;
  bool dot = false;
  for (; index < text.size(); ++index) {
    const char ch = text[index];
    if (ch == '.') {
      if (dot) return false;
      dot = true;
      continue;
    }
    if (!std::isdigit(static_cast<unsigned char>(ch))) {
      if (std::isspace(static_cast<unsigned char>(ch))) break;
      return false;
    }
    (dot ? fraction : whole).push_back(ch);
  }
  while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index]))) ++index;
  if (index != text.size() || whole.empty()) return false;
  if (fraction.size() > 2) return false;
  while (fraction.size() < 2) fraction.push_back('0');
  try {
    const std::int64_t amount = std::stoll(whole) * scale + std::stoll(fraction) * (scale / 100);
    if (amount < 0) return false;
    *value = amount;
    return true;
  } catch (...) {
    return false;
  }
}

bool parse_tax_bp(const std::string& text, std::int64_t* bp) {
  return parse_scaled_decimal(text, 100, bp);
}

bool parse_money_cents(const std::string& text, std::int64_t* cents) {
  return parse_scaled_decimal(text, 100, cents);
}

struct PartnerPlan {
  int row = 1;
  std::string code;
  const gen::SupplierRow* current = nullptr;
  bool has_name = false;
  std::string name;
  bool has_contact = false;
  std::string contact;
  bool has_phone = false;
  std::string phone;
  bool has_tax_no = false;
  std::string tax_no;
  bool has_credit = false;
  std::int64_t credit_limit_cents = 0;
  std::int64_t current_credit_limit_cents = 0;
  bool has_address = false;
  std::string address;
  bool has_status = false;
  bool status = true;
};

// 供应商与客户字段完全相同，但生成器产出的是两个独立结构体。
gen::CustomerRow as_customer(const gen::SupplierRow& source) {
  gen::CustomerRow row;
  row.id = source.id;
  row.code = source.code;
  row.name = source.name;
  row.contact = source.contact;
  row.phone = source.phone;
  row.tax_no = source.tax_no;
  row.address = source.address;
  row.status = source.status;
  return row;
}

void reject_row(std::vector<ImportIssue>& issues, int row, std::string column,
                std::string reason) {
  issues.push_back(ImportIssue{row, std::move(column), std::move(reason)});
}

std::vector<ItemPlan> validate_items(const SheetTable& table, const ColumnMap& columns,
                                     const std::vector<ItemRecord>& records,
                                     const UnitLookup& units, const CategoryLookup& categories,
                                     std::vector<ImportIssue>& issues) {
  std::unordered_map<std::string, ItemRecord const*> by_code;
  std::unordered_map<std::string, std::string> barcode_owner;
  for (const auto& record : records) {
    by_code.emplace(record.row.code, &record);
    if (record.barcodes.empty()) {
      if (!record.row.barcode.empty()) {
        barcode_owner.emplace(record.row.barcode, record.row.code);
      }
    } else {
      for (const auto& code : record.barcodes) {
        barcode_owner.emplace(code, record.row.code);
      }
    }
  }

  std::unordered_map<std::string, int> seen;
  std::unordered_map<std::string, int> seen_barcode;
  std::vector<ItemPlan> plans;
  for (const auto& sheet_row : table.rows) {
    ItemPlan plan;
    plan.row = sheet_row.number;
    plan.code = columns.cell(sheet_row, "商品编码");
    if (plan.code.empty()) {
      reject_row(issues, plan.row, "商品编码", "不能为空");
    } else if (const auto it = seen.find(plan.code); it != seen.end()) {
      reject_row(issues, plan.row, "商品编码", "与第 " + std::to_string(it->second) + " 行重复");
    } else {
      seen.emplace(plan.code, plan.row);
      const auto found = by_code.find(plan.code);
      if (found != by_code.end()) {
        if (found->second->deleted) {
          reject_row(issues, plan.row, "商品编码",
                     "编码已被已删除的档案占用，请改用其它编码");
        } else {
          plan.current = &found->second->row;
        }
      }
    }

    const std::string name = columns.cell(sheet_row, "商品名称");
    if (name.empty()) {
      if (plan.current == nullptr) reject_row(issues, plan.row, "商品名称", "新增商品时不能为空");
    } else {
      plan.has_name = true;
      plan.name = name;
    }

    const std::string spec = columns.cell(sheet_row, "规格");
    if (!spec.empty()) {
      plan.has_spec = true;
      plan.spec = spec;
    }

    const std::string unit_text = columns.cell(sheet_row, "计量单位");
    if (unit_text.empty()) {
      if (plan.current == nullptr) {
        reject_row(issues, plan.row, "计量单位", "新增商品时不能为空");
      }
    } else {
      std::int64_t unit_id = 0;
      const std::string reason = units.resolve(unit_text, &unit_id);
      if (reason.empty()) {
        plan.has_unit = true;
        plan.unit_id = unit_id;
        for (const auto& unit : units.units) {
          if (unit.id == unit_id) plan.unit_name = unit.name;
        }
      } else {
        reject_row(issues, plan.row, "计量单位", reason);
      }
    }

    const BoolCell status = parse_bool(columns.cell(sheet_row, "启用"));
    if (!status.valid) {
      reject_row(issues, plan.row, "启用", "应为 是/否、1/0、TRUE/FALSE 或 启用/停用");
    } else if (status.present) {
      plan.has_status = true;
      plan.status = status.value;
    }

    const std::string category = columns.cell(sheet_row, "分类路径");
    if (!category.empty()) {
      std::int64_t category_id = 0;
      const std::string reason = categories.resolve(category, &category_id);
      if (reason.empty()) {
        plan.has_category = true;
        plan.category_id = category_id;
      } else {
        reject_row(issues, plan.row, "分类路径", reason);
      }
    }

    const std::string barcode_cell = columns.cell(sheet_row, "条码");
    if (!barcode_cell.empty()) {
      // 多个码用 | 分隔，第一个是主码；同一行重复、与其他商品冲突都整批拒绝。
      std::vector<std::string> codes;
      std::unordered_set<std::string> local;
      bool valid = true;
      std::size_t begin = 0;
      while (begin <= barcode_cell.size()) {
        const auto end = barcode_cell.find('|', begin);
        std::string code = barcode_cell.substr(begin, end == std::string::npos
                                                          ? std::string::npos
                                                          : end - begin);
        const auto first = code.find_first_not_of(" \t\r\n");
        if (first != std::string::npos) {
          code = code.substr(first, code.find_last_not_of(" \t\r\n") - first + 1);
        } else {
          code.clear();
        }
        if (!code.empty()) {
          if (!local.insert(code).second) {
            reject_row(issues, plan.row, "条码", "同一行条码重复：" + code);
            valid = false;
          } else if (const auto owner = barcode_owner.find(code);
                     owner != barcode_owner.end() && owner->second != plan.code) {
            reject_row(issues, plan.row, "条码",
                       "条码已被商品 " + owner->second + " 占用：" + code);
            valid = false;
          } else if (const auto duplicate = seen_barcode.find(code);
                     duplicate != seen_barcode.end()) {
            reject_row(issues, plan.row, "条码",
                       "与第 " + std::to_string(duplicate->second) + " 行重复：" + code);
            valid = false;
          } else {
            seen_barcode.emplace(code, plan.row);
            codes.push_back(code);
          }
        }
        if (end == std::string::npos) {
          break;
        }
        begin = end + 1;
      }
      if (valid && !codes.empty()) {
        plan.has_barcode = true;
        plan.barcodes = std::move(codes);
      }
    }

    const std::string tax = columns.cell(sheet_row, "税率%");
    if (!tax.empty()) {
      std::int64_t bp = 0;
      if (!parse_tax_bp(tax, &bp) || bp > 10000) {
        reject_row(issues, plan.row, "税率%", "应为 0–100 的百分数，最多两位小数");
      } else {
        plan.has_tax = true;
        plan.tax_rate_bp = bp;
      }
    }

    plans.push_back(std::move(plan));
  }
  return plans;
}

std::vector<PartnerPlan> validate_partners(const SheetTable& table, const ColumnMap& columns,
                                           const std::vector<PartnerRecord>& records,
                                           std::vector<ImportIssue>& issues,
                                           const std::string& label) {
  std::unordered_map<std::string, PartnerRecord const*> by_code;
  for (const auto& record : records) by_code.emplace(record.row.code, &record);

  std::unordered_map<std::string, int> seen;
  std::vector<PartnerPlan> plans;
  for (const auto& sheet_row : table.rows) {
    PartnerPlan plan;
    plan.row = sheet_row.number;
    plan.code = columns.cell(sheet_row, "编码");
    if (plan.code.empty()) {
      reject_row(issues, plan.row, "编码", "不能为空");
    } else if (const auto it = seen.find(plan.code); it != seen.end()) {
      reject_row(issues, plan.row, "编码", "与第 " + std::to_string(it->second) + " 行重复");
    } else {
      seen.emplace(plan.code, plan.row);
      const auto found = by_code.find(plan.code);
      if (found != by_code.end()) {
        if (found->second->deleted) {
          reject_row(issues, plan.row, "编码", "编码已被已删除的档案占用，请改用其它编码");
        } else {
          plan.current = &found->second->row;
          plan.current_credit_limit_cents = found->second->credit_limit_cents;
        }
      }
    }

    const std::string name = columns.cell(sheet_row, "名称");
    if (name.empty()) {
      if (plan.current == nullptr) {
        reject_row(issues, plan.row, "名称", "新增" + label + "时不能为空");
      }
    } else {
      plan.has_name = true;
      plan.name = name;
    }

    const std::string contact = columns.cell(sheet_row, "联系人");
    if (!contact.empty()) {
      plan.has_contact = true;
      plan.contact = contact;
    }
    const std::string phone = columns.cell(sheet_row, "电话");
    if (!phone.empty()) {
      plan.has_phone = true;
      plan.phone = phone;
    }
    const std::string tax_no = columns.cell(sheet_row, "税号");
    if (!tax_no.empty()) {
      plan.has_tax_no = true;
      plan.tax_no = tax_no;
    }
    const std::string credit = columns.cell(sheet_row, "信用额度（元）");
    if (!credit.empty()) {
      std::int64_t cents = 0;
      if (!parse_money_cents(credit, &cents)) {
        reject_row(issues, plan.row, "信用额度（元）", "应为不小于 0 的金额，最多两位小数");
      } else {
        plan.has_credit = true;
        plan.credit_limit_cents = cents;
      }
    }
    const std::string address = columns.cell(sheet_row, "地址");
    if (!address.empty()) {
      plan.has_address = true;
      plan.address = address;
    }

    const BoolCell status = parse_bool(columns.cell(sheet_row, "启用"));
    if (!status.valid) {
      reject_row(issues, plan.row, "启用", "应为 是/否、1/0、TRUE/FALSE 或 启用/停用");
    } else if (status.present) {
      plan.has_status = true;
      plan.status = status.value;
    }

    plans.push_back(std::move(plan));
  }
  return plans;
}

std::string safe_filename(std::string_view filename) {
  std::string value(filename);
  for (char& ch : value) {
    if (static_cast<unsigned char>(ch) < 0x20 || ch == '/' || ch == '\\') ch = '_';
  }
  if (value.empty()) value = "import.xlsx";
  if (value.size() > 120) value.resize(120);
  return value;
}

BackupResult backup_before_import(Db& db, MasterKind kind,
                                  const std::filesystem::path& backup_dir) {
  try {
    const std::filesystem::path directory =
        backup_dir.empty() ? db.file().parent_path() / "backups" : backup_dir;
    return backup_database(db, directory, "pre-import-" + master_key(kind));
  } catch (const AppError& error) {
    if (error.code() == "BACKUP_DIR_UNWRITABLE") {
      throw AppError("BACKUP_DIR_UNWRITABLE",
                     "导入前自动备份失败，已中止：" + std::string(error.what()));
    }
    throw AppError("BACKUP_FAILED", "导入前自动备份失败，已中止：" + std::string(error.what()));
  } catch (const std::exception& error) {
    throw AppError("BACKUP_FAILED", "导入前自动备份失败，已中止：" + std::string(error.what()));
  }
}

}  // namespace

std::string master_key(MasterKind kind) {
  switch (kind) {
    case MasterKind::Item:
      return "item";
    case MasterKind::Supplier:
      return "supplier";
    case MasterKind::Customer:
      return "customer";
  }
  return "item";
}

std::string master_label(MasterKind kind) {
  switch (kind) {
    case MasterKind::Item:
      return "商品";
    case MasterKind::Supplier:
      return "供应商";
    case MasterKind::Customer:
      return "客户";
  }
  return "商品";
}

std::vector<std::string> master_columns(MasterKind kind) {
  if (kind == MasterKind::Item) {
    return std::vector<std::string>(std::begin(kItemColumns), std::end(kItemColumns));
  }
  if (kind == MasterKind::Supplier) {
    return std::vector<std::string>(std::begin(kSupplierColumns), std::end(kSupplierColumns));
  }
  return std::vector<std::string>(std::begin(kCustomerColumns), std::end(kCustomerColumns));
}

std::vector<std::string> master_required_columns(MasterKind kind) {
  if (kind == MasterKind::Item) return {"商品编码", "商品名称", "计量单位"};
  return {"编码", "名称"};
}

std::string format_tax_bp(std::int64_t bp) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%lld.%02lld",
                static_cast<long long>(bp / 100), static_cast<long long>(bp % 100));
  return std::string(buffer);
}

std::string money_text(std::int64_t cents) {
  return format_tax_bp(cents);
}

MasterExport export_master(Db& db, MasterKind kind) {
  MasterExport result;
  result.header = master_columns(kind);
  if (kind == MasterKind::Item) {
    const CategoryLookup categories = load_categories(db);
    std::unordered_map<std::int64_t, std::string> category_names;
    for (std::size_t i = 0; i < categories.rows.size(); ++i) {
      category_names.emplace(categories.rows[i].id, categories.names[i]);
    }
    gen::ItemDao dao(db);
    std::unordered_map<std::int64_t, std::string> barcode_text;
    for (const auto& record : load_items(db)) {
      if (record.barcodes.empty()) {
        barcode_text.emplace(record.row.id, record.row.barcode);
        continue;
      }
      std::string joined;
      for (std::size_t i = 0; i < record.barcodes.size(); ++i) {
        if (i != 0) joined += "|";
        joined += record.barcodes[i];
      }
      barcode_text.emplace(record.row.id, joined);
    }
    for (const auto& row : dao.list()) {
      const auto category = category_names.find(row.category_id);
      const auto barcodes = barcode_text.find(row.id);
      result.rows.push_back({row.code, row.name, row.spec, row.unit,
                             category == category_names.end() ? "" : category->second,
                             barcodes == barcode_text.end() ? row.barcode : barcodes->second,
                             format_tax_bp(row.tax_rate_bp), row.status ? "是" : "否"});
    }
    return result;
  }
  if (kind == MasterKind::Supplier) {
    gen::SupplierDao dao(db);
    for (const auto& row : dao.list()) {
      result.rows.push_back({row.code, row.name, row.contact, row.phone, row.tax_no, row.address,
                             row.status ? "是" : "否"});
    }
    return result;
  }
  gen::CustomerDao dao(db);
  for (const auto& row : dao.list()) {
    result.rows.push_back({row.code, row.name, row.contact, row.phone, row.tax_no,
                           money_text(row.credit_limit_cents), row.address,
                           row.status ? "是" : "否"});
  }
  return result;
}

ImportReport import_master(Db& db, MasterKind kind, std::string_view bytes,
                           std::string_view filename,
                           const std::filesystem::path& backup_dir) {
  if (bytes.empty()) throw AppError("IMPORT_INVALID", "请选择要导入的 .xlsx 文件");
  if (bytes.size() > kImportMaxBytes) {
    throw AppError("IMPORT_TOO_LARGE", "文件超过 5MB 上限，请分批导入");
  }

  const SheetTable table = read_first_sheet(bytes);
  if (static_cast<int>(table.rows.size()) > kImportMaxRows) {
    throw AppError("IMPORT_TOO_LARGE", "单次最多导入 5000 行，请分批导入");
  }
  const ColumnMap columns =
      map_columns(table.header, master_columns(kind), master_required_columns(kind));

  ImportReport report;
  const std::string label = master_label(kind);
  if (kind == MasterKind::Item) {
    const auto records = load_items(db);
    const UnitLookup units = load_units(db);
    const CategoryLookup categories = load_categories(db);
    auto plans = validate_items(table, columns, records, units, categories, report.issues);
    if (!report.ok()) return report;

    report.outcome.backup = backup_before_import(db, kind, backup_dir).path.filename().string();
    const std::string name = safe_filename(filename);
    db.write([&](Conn& conn) {
      for (const auto& plan : plans) {
        gen::ItemRow row;
        if (plan.current != nullptr) {
          row = *plan.current;
        } else {
          row.code = plan.code;
          row.status = true;
        }
        row.code = plan.code;
        if (plan.has_name) row.name = plan.name;
        if (plan.has_spec) row.spec = plan.spec;
        if (plan.has_unit) {
          row.unit_id = plan.unit_id;
          row.unit = plan.unit_name;
        }
        if (plan.has_category) row.category_id = plan.category_id;
        if (plan.has_barcode) row.barcode = plan.barcodes.front();
        if (plan.has_tax) row.tax_rate_bp = plan.tax_rate_bp;
        if (plan.has_status) row.status = plan.status;
        if (plan.current != nullptr) {
          gen::ItemDao::update(conn, row);
          ++report.outcome.updated;
        } else {
          gen::ItemDao::insert(conn, row);
          ++report.outcome.inserted;
        }
        // 条码单元非空才整体替换；空单元保持原有条码不动。
        if (plan.has_barcode) {
          replace_item_barcodes(conn, row.id, plan.barcodes);
        }
      }
      audit_write(conn, "import", "item", 0, name,
                  "批量导入新增 " + std::to_string(report.outcome.inserted) + " 条，更新 " +
                      std::to_string(report.outcome.updated) + " 条");
    });
    return report;
  }

  const char* table_name = kind == MasterKind::Supplier ? "supplier" : "customer";
  const auto records = load_partners(db, table_name, kind == MasterKind::Customer);
  auto plans = validate_partners(table, columns, records, report.issues, label);
  if (!report.ok()) return report;

  report.outcome.backup = backup_before_import(db, kind, backup_dir).path.filename().string();
  const std::string name = safe_filename(filename);
  db.write([&](Conn& conn) {
    for (const auto& plan : plans) {
      gen::SupplierRow row;
      if (plan.current != nullptr) {
        row = *plan.current;
      } else {
        row.code = plan.code;
        row.status = true;
      }
      row.code = plan.code;
      if (plan.has_name) row.name = plan.name;
      if (plan.has_contact) row.contact = plan.contact;
      if (plan.has_phone) row.phone = plan.phone;
      if (plan.has_tax_no) row.tax_no = plan.tax_no;
      if (plan.has_address) row.address = plan.address;
      if (plan.has_status) row.status = plan.status;
      if (plan.current != nullptr) {
        if (kind == MasterKind::Supplier) {
          gen::SupplierDao::update(conn, row);
        } else {
          gen::CustomerRow customer = as_customer(row);
          customer.credit_limit_cents =
              plan.has_credit ? plan.credit_limit_cents : plan.current_credit_limit_cents;
          gen::CustomerDao::update(conn, customer);
          // 额度单一写入点：partner 为主，customer 行只做镜像（上面已按 partner 值回填）。
          set_customer_credit_limit(conn, customer.id, customer.credit_limit_cents);
        }
        ++report.outcome.updated;
      } else {
        if (kind == MasterKind::Supplier) {
          gen::SupplierDao::insert(conn, row);
        } else {
          gen::CustomerRow customer = as_customer(row);
          if (plan.has_credit) customer.credit_limit_cents = plan.credit_limit_cents;
          gen::CustomerDao::insert(conn, customer);
          set_customer_credit_limit(conn, customer.id, customer.credit_limit_cents);
        }
        ++report.outcome.inserted;
      }
    }
    audit_write(conn, "import", table_name, 0, name,
                "批量导入新增 " + std::to_string(report.outcome.inserted) + " 条，更新 " +
                    std::to_string(report.outcome.updated) + " 条");
  });
  return report;
}

}  // namespace xiaotu
