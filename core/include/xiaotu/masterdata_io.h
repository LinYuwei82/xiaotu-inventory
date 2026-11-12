#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "xiaotu/db.h"

namespace xiaotu {

// 支持批量导入导出的三张主数据表。
enum class MasterKind { Item, Supplier, Customer };

// 单次导入上限：5000 行、5MB 文件。
constexpr std::size_t kImportMaxBytes = 5U << 20U;
constexpr int kImportMaxRows = 5000;

struct ImportIssue {
  int row = 0;             // Excel 实际行号，表头为 1
  std::string column;      // 中文列名
  std::string reason;
};

struct ImportOutcome {
  int inserted = 0;
  int updated = 0;
  std::string backup;      // 导入前自动备份的文件名
};

struct ImportReport {
  std::vector<ImportIssue> issues;
  ImportOutcome outcome;

  bool ok() const { return issues.empty(); }
};

std::string master_key(MasterKind kind);      // item / supplier / customer
std::string master_label(MasterKind kind);    // 商品 / 供应商 / 客户
std::vector<std::string> master_columns(MasterKind kind);
std::vector<std::string> master_required_columns(MasterKind kind);

struct MasterExport {
  std::vector<std::string> header;
  std::vector<std::vector<std::string>> rows;
};

// 导出当前全部（不含系统字段），按编码排序。
MasterExport export_master(Db& db, MasterKind kind);

// 解析 → 全量校验 → 备份 → 单事务写入。
// 行级错误通过 ImportReport::issues 返回，不写库；文件级错误（不是 xlsx、
// 表头不认识、超过上限、备份失败）抛 AppError。
ImportReport import_master(Db& db, MasterKind kind, std::string_view bytes,
                           std::string_view filename);

}  // namespace xiaotu
