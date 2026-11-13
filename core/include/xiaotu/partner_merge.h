#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "xiaotu/db.h"

namespace xiaotu {

// 客户/供应商合并：
//   - 只写 partner.merged_into 与保留档案的角色位，不改业务表的历史键；
//   - 被合并档案的原 P:/C:/S: 键、partner_legacy_map、别名解析都在读取时沿
//     merged_into 链走到保留档案；
//   - partner_merge_log 记录合并前后快照与撤销判定依据，撤销只清 merged_into。

struct PartnerRow {
  std::int64_t id = 0;
  std::string code;
  std::string name;
  std::int64_t is_customer = 0;
  std::int64_t is_supplier = 0;
  std::int64_t merged_into = 0;
  std::string origin;
  std::int64_t status = 1;
  std::int64_t is_deleted = 0;
  std::vector<std::int64_t> supplier_ids;  // partner_legacy_map 的供应商旧 id
};

struct PartnerMergeOutcome {
  std::int64_t log_id = 0;
  std::int64_t target_id = 0;
  std::string target_code;
  std::string target_name;
  std::vector<std::int64_t> source_ids;
};

struct PartnerMergeLogRow {
  std::int64_t id = 0;
  std::int64_t target_id = 0;
  std::string target_code;
  std::string target_name;
  std::int64_t target_is_customer = 0;
  std::int64_t target_is_supplier = 0;
  std::string target_snapshot;    // 合并前保留档案快照（JSON）
  std::string source_ids;         // 被合并档案 id，逗号分隔
  std::string sources_snapshot;   // 被合并档案快照（JSON 数组）
  std::string moved_maps;         // 相关 partner_legacy_map 快照（JSON 数组）
  std::string source_keys;        // 被合并档案名下的历史键（JSON 数组）
  std::int64_t ledger_mark = 0;   // 合并时的 ar_ap_entry 最大 id
  std::int64_t settlement_mark = 0;  // 合并时的 settlement 最大 id
  std::string actor;
  std::string created_at;
  bool can_undo = false;
  std::string undo_reason;
  std::string undone_at;
  std::string undone_by;
};

// 合并多个 partner 到 target。source_ids 至少一条，不含 target。
// 任一校验失败抛 AppError，整个事务回滚。
PartnerMergeOutcome partner_merge(Db& db, std::int64_t target_id,
                                  const std::vector<std::int64_t>& source_ids,
                                  std::string_view actor);

// 撤销合并。仅当被合并档案在合并后没有新增台账/收付款且状态未再变化时允许；
// 否则抛 AppError("MERGE_UNDO_BLOCKED", 原因)。
PartnerMergeOutcome partner_merge_undo(Db& db, std::int64_t log_id, std::string_view actor);

// 撤销前预检：返回空串表示可以撤销，否则为拒绝原因。
std::string partner_merge_undo_blocker(Db& db, std::int64_t log_id);

std::vector<PartnerMergeLogRow> partner_merge_logs(Db& db, int limit = 50);

// 全部未删除 partner（含已合并记录，merged_into > 0 供界面隐藏）。
std::vector<PartnerRow> partner_rows(Db& db);

}  // namespace xiaotu
