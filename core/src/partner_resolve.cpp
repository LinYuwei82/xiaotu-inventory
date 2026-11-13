#include "xiaotu/partner_resolve.h"

#include <sqlite3.h>

#include <exception>
#include <string>
#include <vector>

#include "xiaotu/db.h"

namespace xiaotu {
namespace {

bool numeric_id(std::string_view text) {
  if (text.empty()) return false;
  for (const char ch : text) {
    if (ch < '0' || ch > '9') return false;
  }
  return true;
}

bool partner_code_taken(sqlite3* conn, const std::string& code) {
  Stmt stmt(conn, "SELECT 1 FROM partner WHERE code = ?;");
  stmt.bind(1, code);
  return stmt.step();
}

// 档案编码与已有 partner 撞车时追加后缀；极端情况下多试几次，仍冲突则由唯一索引兜底。
std::string unique_partner_code(sqlite3* conn, const std::string& code,
                                const std::string& suffix) {
  if (!partner_code_taken(conn, code)) return code;
  std::string candidate = code + suffix;
  for (int i = 0; i < 8 && partner_code_taken(conn, candidate); ++i) {
    candidate += suffix;
  }
  return candidate;
}

}  // namespace

std::int64_t find_partner_for_archive(sqlite3* conn, PartnerSide side,
                                      std::int64_t archive_id) {
  if (archive_id <= 0) return 0;
  if (side == PartnerSide::Customer) {
    Stmt stmt(conn, "SELECT id FROM partner WHERE id = ? AND is_customer = 1;");
    stmt.bind(1, archive_id);
    return stmt.step() ? stmt.i64(0) : 0;
  }
  Stmt stmt(conn,
            "SELECT m.partner_id FROM partner_legacy_map m"
            " JOIN partner p ON p.id = m.partner_id"
            " WHERE m.side = 'supplier' AND m.legacy_id = ?;");
  stmt.bind(1, archive_id);
  return stmt.step() ? stmt.i64(0) : 0;
}

std::int64_t ensure_partner_for_archive(sqlite3* conn, PartnerSide side,
                                        std::int64_t archive_id) {
  if (archive_id <= 0) return 0;

  if (side == PartnerSide::Customer) {
    for (int attempt = 0; attempt < 3; ++attempt) {
      const std::int64_t existing = find_partner_for_archive(conn, side, archive_id);
      if (existing > 0) return existing;
      Stmt src(conn,
               "SELECT code, name, contact, phone, address, tax_no, credit_limit_cents,"
               " status, is_deleted, created_at, created_by, updated_at, updated_by"
               " FROM customer WHERE id = ?;");
      src.bind(1, archive_id);
      if (!src.step()) return 0;
      const std::string code =
          unique_partner_code(conn, src.text(0), "-C" + std::to_string(archive_id));
      try {
        Stmt insert(conn,
                    "INSERT INTO partner (id, code, name, contact, phone, address, tax_no,"
                    " is_customer, is_supplier, credit_limit_cents, credit_days, merged_into,"
                    " status, is_deleted, origin, created_at, created_by, updated_at, updated_by)"
                    " VALUES (?, ?, ?, ?, ?, ?, ?, 1, 0, ?, 0, 0, ?, ?, 'AUTO_ARCHIVE',"
                    " ?, ?, ?, ?);");
        insert.bind(1, archive_id)
            .bind(2, code)
            .bind(3, src.text(1))
            .bind(4, src.text(2))
            .bind(5, src.text(3))
            .bind(6, src.text(4))
            .bind(7, src.text(5))
            .bind(8, src.i64(6))
            .bind(9, src.i64(7))
            .bind(10, src.i64(8))
            .bind(11, src.text(9))
            .bind(12, src.text(10))
            .bind(13, src.text(11))
            .bind(14, src.text(12))
            .run();
        return archive_id;
      } catch (const std::exception&) {
        // 并发补建同一客户：回环重查，已被写入就直接复用。
      }
    }
    return 0;
  }

  for (int attempt = 0; attempt < 3; ++attempt) {
    const std::int64_t existing = find_partner_for_archive(conn, side, archive_id);
    if (existing > 0) return existing;
    Stmt src(conn,
             "SELECT code, name, contact, phone, address, tax_no, status, is_deleted,"
             " created_at, created_by, updated_at, updated_by"
             " FROM supplier WHERE id = ?;");
    src.bind(1, archive_id);
    if (!src.step()) return 0;

    Stmt next(conn, "SELECT COALESCE(MAX(id), 0) + 1 FROM partner;");
    if (!next.step()) return 0;
    const std::int64_t partner_id = next.i64(0);
    const std::string legacy_code = src.text(0);
    const std::string code =
        unique_partner_code(conn, legacy_code, "-S" + std::to_string(archive_id));

    try {
      Stmt insert(conn,
                  "INSERT INTO partner (id, code, name, contact, phone, address, tax_no,"
                  " is_customer, is_supplier, credit_limit_cents, credit_days, merged_into,"
                  " status, is_deleted, origin, created_at, created_by, updated_at, updated_by)"
                  " VALUES (?, ?, ?, ?, ?, ?, ?, 0, 1, 0, 0, 0, ?, ?, 'AUTO_ARCHIVE',"
                  " ?, ?, ?, ?);");
      insert.bind(1, partner_id)
          .bind(2, code)
          .bind(3, src.text(1))
          .bind(4, src.text(2))
          .bind(5, src.text(3))
          .bind(6, src.text(4))
          .bind(7, src.text(5))
          .bind(8, src.i64(6))
          .bind(9, src.i64(7))
          .bind(10, src.text(8))
          .bind(11, src.text(9))
          .bind(12, src.text(10))
          .bind(13, src.text(11))
          .run();
    } catch (const std::exception&) {
      // id 或编码被并发占用：回环重新取号。
      continue;
    }

    // 映射是幂等的：同一 legacy id 已被并发写入时保留先写入者，直接复用其 partner。
    Stmt map(conn,
             "INSERT OR IGNORE INTO partner_legacy_map (side, legacy_id, partner_id,"
             " legacy_code, partner_code, created_at)"
             " VALUES ('supplier', ?, ?, ?, ?, datetime('now', 'localtime'));");
    map.bind(1, archive_id).bind(2, partner_id).bind(3, legacy_code).bind(4, code).run();
    Stmt owner(conn,
               "SELECT partner_id FROM partner_legacy_map"
               " WHERE side = 'supplier' AND legacy_id = ?;");
    owner.bind(1, archive_id);
    if (!owner.step()) continue;
    const std::int64_t winner = owner.i64(0);
    if (winner != partner_id) {
      // 这次插入的 partner 没抢到映射，也没有其他引用，清掉避免留下孤儿档案。
      Stmt cleanup(conn,
                   "DELETE FROM partner WHERE id = ? AND NOT EXISTS("
                   " SELECT 1 FROM partner_legacy_map WHERE partner_id = ?);");
      cleanup.bind(1, partner_id).bind(2, partner_id).run();
    }
    return winner;
  }
  return 0;
}

std::string partner_key_for_archive(sqlite3* conn, PartnerSide side, std::int64_t archive_id,
                                    const std::string& name, bool ensure) {
  if (archive_id > 0) {
    const std::int64_t partner_id =
        ensure ? ensure_partner_for_archive(conn, side, archive_id)
               : find_partner_for_archive(conn, side, archive_id);
    if (partner_id > 0) return "P:" + std::to_string(partner_id);
  }
  if (!name.empty()) return "T:" + name;
  return "T:（未指定）";
}

std::string partner_dimension_key(sqlite3* conn, std::string_view partner_key) {
  const std::string key(partner_key);
  if (key.rfind("P:", 0) == 0) {
    if (numeric_id(key.substr(2))) {
      Stmt stmt(conn, "SELECT 1 FROM partner WHERE id = ?;");
      stmt.bind(1, std::stoll(key.substr(2)));
      if (stmt.step()) return key;
    }
    return key;
  }
  if (key.rfind("C:", 0) == 0 && numeric_id(key.substr(2))) {
    const std::int64_t id = std::stoll(key.substr(2));
    if (find_partner_for_archive(conn, PartnerSide::Customer, id) > 0) {
      return "P:" + std::to_string(id);
    }
    return key;
  }
  if (key.rfind("S:", 0) == 0 && numeric_id(key.substr(2))) {
    const std::int64_t partner_id =
        find_partner_for_archive(conn, PartnerSide::Supplier, std::stoll(key.substr(2)));
    if (partner_id > 0) return "P:" + std::to_string(partner_id);
  }
  return key;
}

std::vector<std::string> partner_key_aliases(sqlite3* conn, std::string_view partner_key) {
  const std::string key(partner_key);
  const std::string dimension = partner_dimension_key(conn, key);
  if (dimension.rfind("P:", 0) != 0 || !numeric_id(dimension.substr(2))) {
    return {key};
  }
  const std::int64_t partner_id = std::stoll(dimension.substr(2));
  std::vector<std::string> aliases;
  aliases.push_back("P:" + std::to_string(partner_id));
  {
    Stmt customer(conn, "SELECT 1 FROM customer WHERE id = ?;");
    customer.bind(1, partner_id);
    if (customer.step()) aliases.push_back("C:" + std::to_string(partner_id));
  }
  {
    Stmt suppliers(conn,
                   "SELECT legacy_id FROM partner_legacy_map"
                   " WHERE side = 'supplier' AND partner_id = ? ORDER BY legacy_id;");
    suppliers.bind(1, partner_id);
    while (suppliers.step()) {
      aliases.push_back("S:" + std::to_string(suppliers.i64(0)));
    }
  }
  return aliases;
}

}  // namespace xiaotu
