#include "xiaotu/partner_merge.h"

#include <sqlite3.h>

#include <algorithm>
#include <exception>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "xiaotu/audit.h"
#include "xiaotu/error.h"

namespace xiaotu {
namespace {

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

std::string snapshot_json(const PartnerRow& row) {
  std::ostringstream out;
  out << "{\"id\":" << row.id << ",\"code\":\"" << json_escape(row.code) << "\",\"name\":\""
      << json_escape(row.name) << "\",\"is_customer\":" << row.is_customer
      << ",\"is_supplier\":" << row.is_supplier << ",\"merged_into\":" << row.merged_into
      << ",\"origin\":\"" << json_escape(row.origin) << "\",\"status\":" << row.status
      << ",\"is_deleted\":" << row.is_deleted << "}";
  return out.str();
}

bool load_partner(sqlite3* conn, std::int64_t id, PartnerRow& row) {
  Stmt stmt(conn,
            "SELECT id, code, name, is_customer, is_supplier, merged_into, origin, status,"
            " is_deleted FROM partner WHERE id = ?;");
  stmt.bind(1, id);
  if (!stmt.step()) return false;
  row.id = stmt.i64(0);
  row.code = stmt.text(1);
  row.name = stmt.text(2);
  row.is_customer = stmt.i64(3);
  row.is_supplier = stmt.i64(4);
  row.merged_into = stmt.i64(5);
  row.origin = stmt.text(6);
  row.status = stmt.i64(7);
  row.is_deleted = stmt.i64(8);
  return true;
}

std::vector<std::string> source_keys_of(sqlite3* conn, const PartnerRow& row) {
  std::vector<std::string> keys;
  keys.push_back("P:" + std::to_string(row.id));
  if (row.is_customer != 0) keys.push_back("C:" + std::to_string(row.id));
  Stmt maps(conn,
            "SELECT legacy_id FROM partner_legacy_map"
            " WHERE side = 'supplier' AND partner_id = ? ORDER BY legacy_id;");
  maps.bind(1, row.id);
  while (maps.step()) keys.push_back("S:" + std::to_string(maps.i64(0)));
  return keys;
}

std::string keys_json(const std::vector<std::string>& keys) {
  std::string out = "[";
  for (std::size_t i = 0; i < keys.size(); ++i) {
    if (i != 0) out += ",";
    out += "\"" + json_escape(keys[i]) + "\"";
  }
  out += "]";
  return out;
}

std::string maps_json(sqlite3* conn, const std::vector<std::int64_t>& ids) {
  std::string out = "[";
  bool first = true;
  for (const std::int64_t id : ids) {
    Stmt stmt(conn,
              "SELECT side, legacy_id, partner_id, legacy_code, partner_code"
              " FROM partner_legacy_map WHERE partner_id = ? ORDER BY side, legacy_id;");
    stmt.bind(1, id);
    while (stmt.step()) {
      if (!first) out += ",";
      first = false;
      out += "{\"side\":\"" + json_escape(stmt.text(0)) + "\",\"legacy_id\":" +
             std::to_string(stmt.i64(1)) + ",\"partner_id\":" + std::to_string(stmt.i64(2)) +
             ",\"legacy_code\":\"" + json_escape(stmt.text(3)) + "\",\"partner_code\":\"" +
             json_escape(stmt.text(4)) + "\"}";
    }
  }
  out += "]";
  return out;
}

std::string join_ids(const std::vector<std::int64_t>& ids) {
  std::string out;
  for (std::size_t i = 0; i < ids.size(); ++i) {
    if (i != 0) out += ",";
    out += std::to_string(ids[i]);
  }
  return out;
}

std::vector<std::int64_t> split_ids(std::string_view text) {
  std::vector<std::int64_t> ids;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t comma = text.find(',', start);
    const std::string_view part =
        text.substr(start, comma == std::string_view::npos ? text.size() - start
                                                           : comma - start);
    if (!part.empty()) {
      try {
        ids.push_back(std::stoll(std::string(part)));
      } catch (const std::exception&) {
        // 脏数据跳过，撤销时按状态校验兜底。
      }
    }
    if (comma == std::string_view::npos) break;
    start = comma + 1;
  }
  return ids;
}

std::int64_t max_id(sqlite3* conn, const char* table) {
  Stmt stmt(conn, std::string("SELECT COALESCE(MAX(id), 0) FROM ") + table + ";");
  return stmt.step() ? stmt.i64(0) : 0;
}

std::string in_marks(std::size_t count) {
  std::string marks;
  for (std::size_t i = 0; i < count; ++i) {
    if (i != 0) marks += ", ";
    marks += "?";
  }
  return marks;
}

PartnerMergeLogRow read_log(const Stmt& stmt) {
  PartnerMergeLogRow row;
  row.id = stmt.i64(0);
  row.target_id = stmt.i64(1);
  row.target_code = stmt.text(2);
  row.target_name = stmt.text(3);
  row.target_is_customer = stmt.i64(4);
  row.target_is_supplier = stmt.i64(5);
  row.target_snapshot = stmt.text(6);
  row.source_ids = stmt.text(7);
  row.sources_snapshot = stmt.text(8);
  row.moved_maps = stmt.text(9);
  row.source_keys = stmt.text(10);
  row.ledger_mark = stmt.i64(11);
  row.settlement_mark = stmt.i64(12);
  row.actor = stmt.text(13);
  row.created_at = stmt.text(14);
  row.can_undo = stmt.i64(15) != 0;
  row.undo_reason = stmt.text(16);
  row.undone_at = stmt.text(17);
  row.undone_by = stmt.text(18);
  return row;
}

constexpr const char* kLogColumns =
    "id, target_partner_id, target_code, target_name, target_is_customer,"
    " target_is_supplier, target_snapshot, source_ids, sources_snapshot, moved_maps,"
    " source_keys, ledger_mark, settlement_mark, actor, created_at, can_undo,"
    " undo_reason, undone_at, undone_by";

bool load_log(sqlite3* conn, std::int64_t log_id, PartnerMergeLogRow& row) {
  Stmt stmt(conn, std::string("SELECT ") + kLogColumns +
                      " FROM partner_merge_log WHERE id = ?;");
  stmt.bind(1, log_id);
  if (!stmt.step()) return false;
  row = read_log(stmt);
  return true;
}

// 日志里的 source_keys 是 JSON 字符串数组，这里只取其中的键文本。
std::vector<std::string> parse_key_json(std::string_view json) {
  std::vector<std::string> keys;
  std::size_t i = 0;
  while ((i = json.find('"', i)) != std::string_view::npos) {
    ++i;
    std::string key;
    while (i < json.size() && json[i] != '"') {
      if (json[i] == '\\' && i + 1 < json.size()) {
        ++i;
        switch (json[i]) {
          case 'n': key.push_back('\n'); break;
          case 'r': key.push_back('\r'); break;
          case 't': key.push_back('\t'); break;
          default: key.push_back(json[i]);
        }
      } else {
        key.push_back(json[i]);
      }
      ++i;
    }
    ++i;
    keys.push_back(std::move(key));
  }
  return keys;
}

// 撤销判定：目标/源档案状态是否仍与合并时一致，以及源档案名下是否有新业务。
std::string blocker_of(sqlite3* conn, const PartnerMergeLogRow& log) {
  if (!log.undone_at.empty()) {
    return "该合并已于 " + log.undone_at + " 撤销，不能重复撤销";
  }
  PartnerRow target;
  if (!load_partner(conn, log.target_id, target) || target.is_deleted != 0) {
    return "保留档案已不存在，无法完整还原";
  }
  if (target.merged_into != 0) {
    return "保留档案已被后续合并到其他单位，请先撤销后续合并";
  }

  const std::vector<std::int64_t> source_ids = split_ids(log.source_ids);
  if (source_ids.empty()) return "合并日志缺少被合并档案，无法撤销";
  for (const std::int64_t id : source_ids) {
    PartnerRow source;
    if (!load_partner(conn, id, source) || source.is_deleted != 0) {
      return "被合并档案 #" + std::to_string(id) + " 已不存在，无法完整还原";
    }
    if (source.merged_into != log.target_id) {
      return "被合并档案 #" + std::to_string(id) + " 的合并状态已变化，请先处理后续合并";
    }
  }

  // 合并后新写入源档案名下的业务会与新状态割裂，撤销前必须拦住。
  const std::vector<std::string> keys = parse_key_json(log.source_keys);
  if (!keys.empty()) {
    const std::string marks = in_marks(keys.size());
    const auto count_new = [&](const char* table, std::int64_t mark) {
      Stmt stmt(conn, std::string("SELECT COUNT(*) FROM ") + table +
                          " WHERE id > ? AND partner_key IN (" + marks + ");");
      stmt.bind(1, mark);
      for (std::size_t i = 0; i < keys.size(); ++i) {
        stmt.bind(static_cast<std::int64_t>(i + 2), keys[i]);
      }
      return stmt.step() ? stmt.i64(0) : 0;
    };
    const std::int64_t ledger = count_new("ar_ap_entry", log.ledger_mark);
    const std::int64_t settlements = count_new("settlement", log.settlement_mark);
    if (ledger > 0 || settlements > 0) {
      return "合并后已有新业务写入被合并单位（台账 " + std::to_string(ledger) + " 条、收付款 " +
             std::to_string(settlements) + " 条），撤销会割裂新单据";
    }
  }
  return {};
}

}  // namespace

PartnerMergeOutcome partner_merge(Db& db, std::int64_t target_id,
                                  const std::vector<std::int64_t>& source_ids,
                                  std::string_view actor) {
  PartnerMergeOutcome outcome;
  db.write([&](sqlite3* conn) {
    PartnerRow target;
    if (!load_partner(conn, target_id, target) || target.is_deleted != 0) {
      throw AppError("NOT_FOUND", "保留档案不存在");
    }
    if (target.merged_into != 0) {
      throw AppError("INVALID_INPUT", "保留档案已被合并，不能作为目标");
    }

    std::vector<std::int64_t> sources;
    std::set<std::int64_t> seen;
    for (const std::int64_t id : source_ids) {
      if (id <= 0 || id == target_id) continue;
      if (!seen.insert(id).second) continue;
      sources.push_back(id);
    }
    if (sources.empty()) {
      throw AppError("INVALID_INPUT", "请选择至少一个要合并的档案，且不能与保留档案相同");
    }

    std::string sources_snapshot = "[";
    bool combined_customer = target.is_customer != 0;
    bool combined_supplier = target.is_supplier != 0;
    for (std::size_t i = 0; i < sources.size(); ++i) {
      PartnerRow source;
      if (!load_partner(conn, sources[i], source) || source.is_deleted != 0) {
        throw AppError("NOT_FOUND", "被合并档案 #" + std::to_string(sources[i]) + " 不存在");
      }
      if (source.merged_into != 0) {
        throw AppError("INVALID_INPUT",
                       "档案「" + source.name + "」已被合并，不能重复合并");
      }
      if (i != 0) sources_snapshot += ",";
      sources_snapshot += snapshot_json(source);
      combined_customer = combined_customer || source.is_customer != 0;
      combined_supplier = combined_supplier || source.is_supplier != 0;
    }
    sources_snapshot += "]";

    // 源档案名下的历史键：撤销判定与日志追溯都用它，合并本身不改这些键。
    std::vector<std::string> all_keys;
    for (const std::int64_t id : sources) {
      PartnerRow source;
      load_partner(conn, id, source);
      for (auto& key : source_keys_of(conn, source)) all_keys.push_back(std::move(key));
    }
    std::sort(all_keys.begin(), all_keys.end());
    all_keys.erase(std::unique(all_keys.begin(), all_keys.end()), all_keys.end());

    const std::string moved_maps = maps_json(conn, sources);
    const std::int64_t ledger_mark = max_id(conn, "ar_ap_entry");
    const std::int64_t settlement_mark = max_id(conn, "settlement");
    const std::string at = now_text();

    const bool can_undo = true;
    const std::int64_t log_id = max_id(conn, "partner_merge_log") + 1;
    {
      Stmt insert(conn,
                  "INSERT INTO partner_merge_log (id, target_partner_id, target_code,"
                  " target_name, target_is_customer, target_is_supplier, target_snapshot,"
                  " source_ids, sources_snapshot, moved_maps, source_keys, ledger_mark,"
                  " settlement_mark, actor, created_at, can_undo, undo_reason, undone_at,"
                  " undone_by) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, '',"
                  " '', '');");
      insert.bind(1, log_id)
          .bind(2, target_id)
          .bind(3, target.code)
          .bind(4, target.name)
          .bind(5, static_cast<std::int64_t>(target.is_customer != 0))
          .bind(6, static_cast<std::int64_t>(target.is_supplier != 0))
          .bind(7, snapshot_json(target))
          .bind(8, join_ids(sources))
          .bind(9, sources_snapshot)
          .bind(10, moved_maps)
          .bind(11, keys_json(all_keys))
          .bind(12, ledger_mark)
          .bind(13, settlement_mark)
          .bind(14, actor)
          .bind(15, at)
          .bind(16, static_cast<std::int64_t>(can_undo ? 1 : 0))
          .run();
    }

    for (const std::int64_t id : sources) {
      Stmt update(conn,
                  "UPDATE partner SET merged_into = ?, updated_at = ?, updated_by = ?"
                  " WHERE id = ? AND merged_into = 0;");
      update.bind(1, target_id).bind(2, at).bind(3, actor).bind(4, id).run();
    }
    {
      Stmt update(conn,
                  "UPDATE partner SET is_customer = ?, is_supplier = ?, updated_at = ?,"
                  " updated_by = ? WHERE id = ?;");
      update.bind(1, static_cast<std::int64_t>(combined_customer ? 1 : 0))
          .bind(2, static_cast<std::int64_t>(combined_supplier ? 1 : 0))
          .bind(3, at)
          .bind(4, actor)
          .bind(5, target_id)
          .run();
    }

    const std::string detail =
        "{\"log_id\":" + std::to_string(log_id) + ",\"target_id\":" +
        std::to_string(target_id) + ",\"source_ids\":[" + join_ids(sources) + "]}";
    audit_write_as(conn, actor, "partner_merge", "partner", target_id, target.code, detail);

    outcome.log_id = log_id;
    outcome.target_id = target_id;
    outcome.target_code = target.code;
    outcome.target_name = target.name;
    outcome.source_ids = sources;
  });
  return outcome;
}

std::string partner_merge_undo_blocker(Db& db, std::int64_t log_id) {
  std::string reason;
  db.read([&](sqlite3* conn) {
    PartnerMergeLogRow log;
    if (!load_log(conn, log_id, log)) {
      reason = "合并日志不存在";
      return;
    }
    reason = blocker_of(conn, log);
  });
  return reason;
}

PartnerMergeOutcome partner_merge_undo(Db& db, std::int64_t log_id, std::string_view actor) {
  PartnerMergeOutcome outcome;
  db.write([&](sqlite3* conn) {
    PartnerMergeLogRow log;
    if (!load_log(conn, log_id, log)) {
      throw AppError("NOT_FOUND", "合并日志不存在");
    }
    const std::string blocker = blocker_of(conn, log);
    if (!blocker.empty()) {
      throw AppError("MERGE_UNDO_BLOCKED", blocker);
    }

    const std::string at = now_text();
    const std::vector<std::int64_t> source_ids = split_ids(log.source_ids);
    for (const std::int64_t id : source_ids) {
      // 只清 merged_into；历史键与 partner_legacy_map 在合并时未改动，天然保持原状。
      Stmt update(conn,
                  "UPDATE partner SET merged_into = 0, updated_at = ?, updated_by = ?"
                  " WHERE id = ? AND merged_into = ?;");
      update.bind(1, at).bind(2, actor).bind(3, id).bind(4, log.target_id).run();
    }
    {
      // 还原保留档案合并前的角色位；合并时只可能把 0 补成 1。
      Stmt update(conn,
                  "UPDATE partner SET is_customer = ?, is_supplier = ?, updated_at = ?,"
                  " updated_by = ? WHERE id = ?;");
      update.bind(1, log.target_is_customer)
          .bind(2, log.target_is_supplier)
          .bind(3, at)
          .bind(4, actor)
          .bind(5, log.target_id)
          .run();
    }
    // partner_legacy_map 在合并时原样保留，读取时沿 merged_into 走到目标；
    // 撤销清掉 merged_into 后，映射与历史键即回到合并前状态，无需回写。
    {
      Stmt update(conn,
                  "UPDATE partner_merge_log SET can_undo = 0, undo_reason = ?, undone_at = ?,"
                  " undone_by = ? WHERE id = ?;");
      update.bind(1, std::string("已撤销")).bind(2, at).bind(3, actor).bind(4, log_id).run();
    }

    const std::string detail =
        "{\"log_id\":" + std::to_string(log_id) + ",\"target_id\":" +
        std::to_string(log.target_id) + ",\"source_ids\":[" + join_ids(source_ids) + "]}";
    audit_write_as(conn, actor, "partner_merge_undo", "partner", log.target_id, log.target_code,
                   detail);

    outcome.log_id = log_id;
    outcome.target_id = log.target_id;
    outcome.target_code = log.target_code;
    outcome.target_name = log.target_name;
    outcome.source_ids = source_ids;
  });
  return outcome;
}

std::vector<PartnerMergeLogRow> partner_merge_logs(Db& db, int limit) {
  std::vector<PartnerMergeLogRow> rows;
  db.read([&](sqlite3* conn) {
    Stmt stmt(conn, std::string("SELECT ") + kLogColumns +
                        " FROM partner_merge_log ORDER BY id DESC LIMIT ?;");
    stmt.bind(1, static_cast<std::int64_t>(limit <= 0 ? 50 : limit));
    while (stmt.step()) rows.push_back(read_log(stmt));
  });
  return rows;
}

std::vector<PartnerRow> partner_rows(Db& db) {
  std::vector<PartnerRow> rows;
  db.read([&](sqlite3* conn) {
    Stmt stmt(conn,
              "SELECT id, code, name, is_customer, is_supplier, merged_into, origin, status,"
              " is_deleted FROM partner WHERE is_deleted = 0 ORDER BY code;");
    while (stmt.step()) {
      PartnerRow row;
      row.id = stmt.i64(0);
      row.code = stmt.text(1);
      row.name = stmt.text(2);
      row.is_customer = stmt.i64(3);
      row.is_supplier = stmt.i64(4);
      row.merged_into = stmt.i64(5);
      row.origin = stmt.text(6);
      row.status = stmt.i64(7);
      row.is_deleted = stmt.i64(8);
      Stmt legacy(conn,
                  "SELECT legacy_id FROM partner_legacy_map"
                  " WHERE side = 'supplier' AND partner_id = ? ORDER BY legacy_id;");
      legacy.bind(1, row.id);
      while (legacy.step()) row.supplier_ids.push_back(legacy.i64(0));
      rows.push_back(std::move(row));
    }
  });
  return rows;
}

}  // namespace xiaotu
