// 主数据导入冒烟：新增 / 按编码 upsert / 空单元格保持原值 / 整批回滚 / 自动备份。
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "xiaotu/db.h"
#include "xiaotu/error.h"
#include "xiaotu/masterdata_io.h"
#include "xiaotu/migrations.h"
#include "xiaotu/xlsx.h"

namespace {

int g_failures = 0;

void check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << std::endl;
    ++g_failures;
  }
}

std::string item_xlsx(const std::vector<std::vector<std::string>>& rows) {
  xiaotu::XlsxSheet sheet("商品");
  sheet.add_header_row({"商品编码", "商品名称", "规格", "计量单位", "启用"});
  for (const auto& row : rows) {
    sheet.add_text_row({row[0], row[1], row[2], row[3], row[4]});
  }
  return sheet.build();
}

std::string partner_xlsx(bool supplier, const std::vector<std::vector<std::string>>& rows) {
  xiaotu::XlsxSheet sheet(supplier ? "供应商" : "客户");
  sheet.add_header_row({"编码", "名称", "联系人", "电话", "地址", "启用"});
  for (const auto& row : rows) {
    sheet.add_text_row({row[0], row[1], row[2], row[3], row[4], row[5]});
  }
  return sheet.build();
}

std::size_t backup_count(const std::filesystem::path& dir) {
  std::size_t count = 0;
  if (!std::filesystem::exists(dir)) return 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.path().filename().string().find("pre-import-") != std::string::npos) ++count;
  }
  return count;
}

const std::vector<std::string>* find_row(const xiaotu::MasterExport& table,
                                         const std::string& code) {
  for (const auto& row : table.rows) {
    if (!row.empty() && row[0] == code) return &row;
  }
  return nullptr;
}

}  // namespace

int main() {
  namespace fs = std::filesystem;
#if defined(_WIN32)
  const long process_id = _getpid();
#else
  const long process_id = ::getpid();
#endif
  const fs::path dir =
      fs::temp_directory_path() / ("xiaotu-masterdata-" + std::to_string(process_id));

  try {
    fs::remove_all(dir);
    xiaotu::Db db(dir / "app.db", 2);
    xiaotu::migrate(db);
    const fs::path backup_dir = dir / "backups";

    // 1. 新增两条商品。
    auto report = xiaotu::import_master(
        db, xiaotu::MasterKind::Item,
        item_xlsx({{"SP001", "矿泉水", "550ml", "瓶", "是"},
                   {"SP002", "方便面", "", "件", ""}}),
        "items.xlsx");
    check(report.ok(), "首次导入应成功");
    check(report.outcome.inserted == 2 && report.outcome.updated == 0, "首次导入应新增 2 条");
    check(!report.outcome.backup.empty(), "导入前应生成备份");
    check(backup_count(backup_dir) == 1, "首次导入应有且只有一份 pre-import 备份");

    // 2. upsert：只改名称与单位，规格与启用留空应保持原值。
    report = xiaotu::import_master(
        db, xiaotu::MasterKind::Item,
        item_xlsx({{"SP001", "矿泉水（新）", "", "件", ""}}), "items.xlsx");
    check(report.ok(), "按编码更新应成功");
    check(report.outcome.inserted == 0 && report.outcome.updated == 1, "应更新 1 条");
    {
      const auto items = xiaotu::export_master(db, xiaotu::MasterKind::Item);
      const auto* row = find_row(items, "SP001");
      check(row != nullptr, "更新后应能导出 SP001");
      if (row != nullptr) {
        check((*row)[1] == "矿泉水（新）", "名称应被更新");
        check((*row)[2] == "550ml", "规格留空应保持原值");
        check((*row)[3] == "件", "单位应被更新");
        check((*row)[4] == "是", "启用留空应保持原值");
      }
    }

    // 3. 一行有错：整批不写，且不产生备份。
    const std::size_t before = backup_count(backup_dir);
    report = xiaotu::import_master(
        db, xiaotu::MasterKind::Item,
        item_xlsx({{"SP003", "新品", "", "瓶", "是"}, {"SP004", "坏行", "", "箱箱", "是"}}),
        "items.xlsx");
    check(!report.ok(), "单位不存在时应拒绝整批");
    check(report.issues.size() == 1 && report.issues[0].row == 3, "错误行号应为 Excel 第 3 行");
    check(report.issues[0].column == "计量单位", "错误列名应为「计量单位」");
    check(backup_count(backup_dir) == before, "校验失败不应产生备份");
    {
      const auto items = xiaotu::export_master(db, xiaotu::MasterKind::Item);
      check(find_row(items, "SP003") == nullptr, "校验失败时任何一行都不应写入");
    }

    // 4. 文件内编码重复。
    report = xiaotu::import_master(
        db, xiaotu::MasterKind::Item,
        item_xlsx({{"SP005", "甲", "", "瓶", "是"}, {"SP005", "乙", "", "瓶", "是"}}),
        "items.xlsx");
    check(!report.ok() && report.issues.size() == 1, "文件内重复编码应报错");

    // 5. 未知表头。
    {
      xiaotu::XlsxSheet sheet("商品");
      sheet.add_header_row({"商品编码", "商品名称", "备注"});
      sheet.add_text_row({"SP006", "测试", "随便写"});
      bool rejected = false;
      try {
        xiaotu::import_master(db, xiaotu::MasterKind::Item, sheet.build(), "items.xlsx");
      } catch (const xiaotu::AppError& error) {
        rejected = error.code() == "IMPORT_INVALID" &&
                   std::string(error.what()).find("备注") != std::string::npos;
      }
      check(rejected, "未知表头应被拒绝并指出列名");
    }

    // 6. 供应商 / 客户：新增 + 空单元格保持原值。
    report = xiaotu::import_master(
        db, xiaotu::MasterKind::Supplier,
        partner_xlsx(true, {{"S001", "供应商甲", "张三", "13800138000", "上海", "是"}}),
        "suppliers.xlsx");
    check(report.ok() && report.outcome.inserted == 1, "供应商应新增 1 条");
    report = xiaotu::import_master(
        db, xiaotu::MasterKind::Supplier,
        partner_xlsx(true, {{"S001", "供应商甲（新）", "", "", "", ""}}), "suppliers.xlsx");
    check(report.ok() && report.outcome.updated == 1, "供应商应更新 1 条");
    {
      const auto suppliers = xiaotu::export_master(db, xiaotu::MasterKind::Supplier);
      const auto* row = find_row(suppliers, "S001");
      check(row != nullptr, "应能导出供应商 S001");
      if (row != nullptr) {
        check((*row)[1] == "供应商甲（新）", "供应商名称应更新");
        check((*row)[2] == "张三", "联系人留空应保持原值");
        check((*row)[3] == "13800138000", "电话留空应保持原值");
        check((*row)[4] == "上海", "地址留空应保持原值");
      }
    }

    report = xiaotu::import_master(
        db, xiaotu::MasterKind::Customer,
        partner_xlsx(false, {{"C001", "客户甲", "", "", "", "否"}}), "customers.xlsx");
    check(report.ok() && report.outcome.inserted == 1, "客户应新增 1 条");
    {
      const auto customers = xiaotu::export_master(db, xiaotu::MasterKind::Customer);
      const auto* row = find_row(customers, "C001");
      check(row != nullptr && (*row)[5] == "否", "客户的启用状态应为否");
    }

    // 7. 超大文件直接拒绝。
    bool too_large = false;
    try {
      xiaotu::import_master(db, xiaotu::MasterKind::Item,
                            std::string(xiaotu::kImportMaxBytes + 1, 'x'), "big.xlsx");
    } catch (const xiaotu::AppError& error) {
      too_large = error.code() == "IMPORT_TOO_LARGE";
    }
    check(too_large, "超过 5MB 应被拒绝");
  } catch (const std::exception& error) {
    std::cerr << "FAIL: 未预期异常: " << error.what() << std::endl;
    ++g_failures;
  }

  if (g_failures == 0) {
    std::cout << "masterdata_smoke OK" << std::endl;
    return 0;
  }
  std::cerr << "masterdata_smoke 失败: " << g_failures << " 项" << std::endl;
  return 1;
}
