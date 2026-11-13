#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "xiaotu/db.h"

namespace xiaotu {

// 期初导入只做四类：库存（数量 + 成本）、应收、应付、资金账户期初余额。
// 主数据档案走既有的商品/客户/供应商导入，不并入这里。
enum class OpeningKind {
  Stock,        // 其他入库 + 期初类别，审核后进流水与结存
  Receivable,   // 直接写 ar_ap_entry 并打期初标记，走现有核销
  Payable,
  Fund,         // 更新资金账户已有的期初余额字段
};

struct OpeningIssue {
  int row = 1;
  std::string column;
  std::string reason;
};

struct OpeningReport {
  bool ok() const { return issues.empty(); }
  int inserted = 0;
  std::string backup;
  std::vector<std::string> doc_nos;
  std::vector<OpeningIssue> issues;
};

std::string opening_key(OpeningKind kind);
std::string opening_label(OpeningKind kind);
std::vector<std::string> opening_columns(OpeningKind kind);

// 是否还允许导入期初；空串表示允许，否则是拒绝原因（判定依据）。
std::string opening_blocker(Db& db, OpeningKind kind);

// 有业务流水之后一律拒绝导入期初（不覆盖、不追加），错误信息说明判定依据。
// backup_dir 为空时备份到 <数据库所在目录>/backups。
OpeningReport import_opening(Db& db, OpeningKind kind, std::string_view bytes,
                             std::string_view filename,
                             const std::filesystem::path& backup_dir = {});

}  // namespace xiaotu
