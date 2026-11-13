#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;

namespace xiaotu {

// 往来键与 partner 维度的桥接。写入端从 v29 起产出 P:{partner_id}，
// 历史上仍是 C:{customer_id} / S:{supplier_id} / T:{名称}，读取时统一折成 partner 维度。

enum class PartnerSide { Customer, Supplier };

// 仅查询：客户看 partner.id，供应商看 partner_legacy_map。找不到返回 0。
std::int64_t find_partner_for_archive(sqlite3* conn, PartnerSide side, std::int64_t archive_id);

// 确保档案在 partner 有对应记录：客户沿用原 id；供应商分配新 id 并写映射。
// 档案不存在或补建失败返回 0。必须在写事务里调用。
std::int64_t ensure_partner_for_archive(sqlite3* conn, PartnerSide side, std::int64_t archive_id);

// 产出往来键：能找到 partner（ensure=true 时会补建）则 P:{partner_id}，
// 否则退回 T:{名称}（未选档案、档案已删除或补建失败）。
std::string partner_key_for_archive(sqlite3* conn, PartnerSide side, std::int64_t archive_id,
                                    const std::string& name, bool ensure);

// 归集维度：P:/C:/S: 统一折成 P:{partner_id}；T: 与查不到映射的键原样返回。
std::string partner_dimension_key(sqlite3* conn, std::string_view partner_key);

// 同一单位在库里可能同时存在历史 C:/S: 与新 P: 键；返回本次查询应覆盖的全部键。
// T: 与查不到归属的键只返回自身。
std::vector<std::string> partner_key_aliases(sqlite3* conn, std::string_view partner_key);

}  // namespace xiaotu
