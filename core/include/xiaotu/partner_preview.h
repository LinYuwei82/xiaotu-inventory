#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "xiaotu/db.h"

namespace xiaotu {

// 客户/供应商归并预览：只读 customer、supplier、ar_ap_entry，不写任何表。
// 规则与 docs/客户供应商合表方案.md 第 3 节一致：
//   税号相同 = 高置信；名称 + 电话都相同 = 中高；仅名称相同 = 需人工确认。
enum class PreviewSide { Customer, Supplier, LedgerOnly, Partner };
enum class PreviewMatch { TaxNo, NamePhone, NameOnly };

struct PartnerPreviewRow {
  PreviewSide side = PreviewSide::LedgerOnly;
  std::string code;
  std::string name;
  std::string tax_no;
  std::string phone;
  std::int64_t ar_open_cents = 0;   // 未核销应收
  std::int64_t ap_open_cents = 0;   // 未核销应付
  std::int64_t ar_docs = 0;         // 应收单据数（doc_id > 0）
  std::int64_t ap_docs = 0;         // 应付单据数（doc_id > 0）
  std::int64_t opening_rows = 0;    // 期初记录条数（doc_id = 0）
  std::string partner_key;          // C:x / S:y / T:名称 / P:id
  std::string origin;               // AUTO_ARCHIVE（业务顺手补建）/ MANUAL（历史、手工或合并）
};

struct PartnerPreviewGroup {
  PreviewMatch match = PreviewMatch::NameOnly;
  std::string match_value;          // 命中的税号、名称+电话或名称
  bool keys_conflict = false;       // 同一单位存在两个及以上 partner_key
  std::vector<PartnerPreviewRow> rows;
};

// 同一个名称对应多个 partner_key：真正会导致台账分裂的情况。
struct PartnerMultiKeyUnit {
  std::string name;
  std::vector<PartnerPreviewRow> keys;
};

struct PartnerPreviewReport {
  std::int64_t customer_total = 0;
  std::int64_t supplier_total = 0;
  std::int64_t ledger_key_total = 0;
  std::int64_t high_count = 0;
  std::int64_t medium_count = 0;
  std::int64_t name_only_count = 0;
  std::int64_t group_records = 0;   // 候选组涉及的档案条数（不含仅有流水的行）
  std::vector<PartnerPreviewGroup> groups;
  std::vector<PartnerMultiKeyUnit> multi_key_units;
};

PartnerPreviewReport partner_merge_preview(const Db& db);

std::string partner_preview_text(const PartnerPreviewReport& report);
std::string partner_preview_json(const PartnerPreviewReport& report);
std::string partner_preview_csv(const PartnerPreviewReport& report);

std::string_view preview_side_label(PreviewSide side);
std::string_view preview_match_label(PreviewMatch match);

}  // namespace xiaotu
