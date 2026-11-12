#include "xiaotu/masterdata_io.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "xiaotu/audit.h"
#include "xiaotu/backup.h"
#include "xiaotu/crud.h"
#include "xiaotu/error.h"
#include "xiaotu/gen/customer_dao.h"
#include "xiaotu/gen/item_dao.h"
#include "xiaotu/gen/supplier_dao.h"
#include "xiaotu/gen/unit_dao.h"
#include "xiaotu/xlsx_reader.h"

namespace xiaotu {
namespace {

constexpr const char* kItemColumns[] = {"商品编码", "商品名称", "规格", "计量单位", "启用"};
constexpr const char* kPartnerColumns[] = {"编码", "名称", "联系人", "电话", "地址", "启用"};

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
};

struct PartnerRecord {
  gen::SupplierRow row;
  bool deleted = false;
};

std::vector<ItemRecord> load_items(Db& db) {
  std::vector<ItemRecord> records;
  db.read([&](sqlite3* conn) {
    Stmt stmt(conn,
              "SELECT id, code, name, spec, unit, unit_id, status, is_deleted FROM item"
              " ORDER BY code;");
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
      records.push_back(std::move(record));
    }
  });
  return records;
}

std::vector<PartnerRecord> load_partners(Db& db, const char* table) {
  std::vector<PartnerRecord> records;
  db.read([&](sqlite3* conn) {
    Stmt stmt(conn, "SELECT id, code, name, contact, phone, address, status, is_deleted FROM " +
                        std::string(table) + " ORDER BY code;");
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
  bool has_status = false;
  bool status = true;
};

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
                                     const UnitLookup& units,
                                     std::vector<ImportIssue>& issues) {
  std::unordered_map<std::string, ItemRecord const*> by_code;
  for (const auto& record : records) {
    by_code.emplace(record.row.code, &record);
  }

  std::unordered_map<std::string, int> seen;
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

BackupResult backup_before_import(Db& db, MasterKind kind) {
  try {
    return backup_database(db, db.file().parent_path() / "backups",
                           "pre-import-" + master_key(kind));
  } catch (const AppError& error) {
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
  return std::vector<std::string>(std::begin(kPartnerColumns), std::end(kPartnerColumns));
}

std::vector<std::string> master_required_columns(MasterKind kind) {
  if (kind == MasterKind::Item) return {"商品编码", "商品名称", "计量单位"};
  return {"编码", "名称"};
}

MasterExport export_master(Db& db, MasterKind kind) {
  MasterExport result;
  result.header = master_columns(kind);
  if (kind == MasterKind::Item) {
    gen::ItemDao dao(db);
    for (const auto& row : dao.list()) {
      result.rows.push_back({row.code, row.name, row.spec, row.unit, row.status ? "是" : "否"});
    }
    return result;
  }
  if (kind == MasterKind::Supplier) {
    gen::SupplierDao dao(db);
    for (const auto& row : dao.list()) {
      result.rows.push_back({row.code, row.name, row.contact, row.phone, row.address,
                             row.status ? "是" : "否"});
    }
    return result;
  }
  gen::CustomerDao dao(db);
  for (const auto& row : dao.list()) {
    result.rows.push_back({row.code, row.name, row.contact, row.phone, row.address,
                           row.status ? "是" : "否"});
  }
  return result;
}

ImportReport import_master(Db& db, MasterKind kind, std::string_view bytes,
                           std::string_view filename) {
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
    auto plans = validate_items(table, columns, records, units, report.issues);
    if (!report.ok()) return report;

    report.outcome.backup = backup_before_import(db, kind).path.filename().string();
    const std::string name = safe_filename(filename);
    db.write([&](sqlite3* conn) {
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
        if (plan.has_status) row.status = plan.status;
        if (plan.current != nullptr) {
          gen::ItemDao::update(conn, row);
          ++report.outcome.updated;
        } else {
          gen::ItemDao::insert(conn, row);
          ++report.outcome.inserted;
        }
      }
      audit_write(conn, "import", "item", 0, name,
                  "批量导入新增 " + std::to_string(report.outcome.inserted) + " 条，更新 " +
                      std::to_string(report.outcome.updated) + " 条");
    });
    return report;
  }

  const char* table_name = kind == MasterKind::Supplier ? "supplier" : "customer";
  const auto records = load_partners(db, table_name);
  auto plans = validate_partners(table, columns, records, report.issues, label);
  if (!report.ok()) return report;

  report.outcome.backup = backup_before_import(db, kind).path.filename().string();
  const std::string name = safe_filename(filename);
  db.write([&](sqlite3* conn) {
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
      if (plan.has_address) row.address = plan.address;
      if (plan.has_status) row.status = plan.status;
      if (plan.current != nullptr) {
        if (kind == MasterKind::Supplier) {
          gen::SupplierDao::update(conn, row);
        } else {
          gen::CustomerRow customer = as_customer(row);
          gen::CustomerDao::update(conn, customer);
        }
        ++report.outcome.updated;
      } else {
        if (kind == MasterKind::Supplier) {
          gen::SupplierDao::insert(conn, row);
        } else {
          gen::CustomerRow customer = as_customer(row);
          gen::CustomerDao::insert(conn, customer);
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
