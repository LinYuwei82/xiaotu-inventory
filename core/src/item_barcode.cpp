#include "xiaotu/item_barcode.h"

#include <sqlite3.h>

#include <string>
#include <unordered_set>
#include <vector>

#include "xiaotu/error.h"
#include "xiaotu/ids.h"

namespace xiaotu {
namespace {

std::string trimmed(const std::string& value) {
  const auto begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return {};
  }
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

std::string item_code(sqlite3* conn, std::int64_t item_id) {
  Stmt stmt(conn, "SELECT code FROM item WHERE id = ?;");
  stmt.bind(1, item_id);
  return stmt.step() ? stmt.text(0) : std::string{};
}

void ensure_unused(sqlite3* conn, std::int64_t item_id, const std::string& barcode) {
  {
    Stmt stmt(conn,
              "SELECT i.code FROM item_barcode b JOIN item i ON i.id = b.item_id"
              " WHERE b.barcode = ? AND b.item_id <> ? LIMIT 1;");
    stmt.bind(1, barcode).bind(2, item_id);
    if (stmt.step()) {
      throw AppError("DUPLICATE_BARCODE",
                     "条码已被商品占用：" + barcode + "（" + stmt.text(0) + "）");
    }
  }
  {
    Stmt stmt(conn, "SELECT code FROM item WHERE barcode = ? AND id <> ? LIMIT 1;");
    stmt.bind(1, barcode).bind(2, item_id);
    if (stmt.step()) {
      throw AppError("DUPLICATE_BARCODE",
                     "条码已被商品占用：" + barcode + "（" + stmt.text(0) + "）");
    }
  }
}

}  // namespace

std::vector<ItemBarcodeRow> item_barcodes(Db& db, std::int64_t item_id) {
  std::vector<ItemBarcodeRow> rows;
  db.read([&](sqlite3* conn) {
    Stmt stmt(conn,
              "SELECT barcode, is_primary FROM item_barcode WHERE item_id = ?"
              " ORDER BY is_primary DESC, id;");
    stmt.bind(1, item_id);
    while (stmt.step()) {
      rows.push_back(ItemBarcodeRow{.barcode = stmt.text(0), .primary = stmt.i64(1) != 0});
    }
  });
  return rows;
}

std::vector<std::string> normalize_barcodes(const std::vector<std::string>& raw) {
  std::vector<std::string> codes;
  std::unordered_set<std::string> seen;
  for (const auto& value : raw) {
    const std::string code = trimmed(value);
    if (code.empty()) {
      continue;
    }
    if (!seen.insert(code).second) {
      throw AppError("INVALID_INPUT", "条码重复：" + code);
    }
    codes.push_back(code);
  }
  return codes;
}

void replace_item_barcodes(sqlite3* conn, std::int64_t item_id,
                           const std::vector<std::string>& raw) {
  const std::string code = item_code(conn, item_id);
  if (code.empty()) {
    throw AppError("NOT_FOUND", "商品不存在");
  }
  const std::vector<std::string> codes = normalize_barcodes(raw);
  for (const auto& barcode : codes) {
    ensure_unused(conn, item_id, barcode);
  }

  {
    Stmt stmt(conn, "DELETE FROM item_barcode WHERE item_id = ?;");
    stmt.bind(1, item_id).run();
  }
  const std::string stamp = now_text();
  for (std::size_t i = 0; i < codes.size(); ++i) {
    Stmt stmt(conn,
              "INSERT INTO item_barcode (id, item_id, barcode, is_primary, created_at)"
              " VALUES (?, ?, ?, ?, ?);");
    stmt.bind(1, next_id())
        .bind(2, item_id)
        .bind(3, codes[i])
        .bind(4, static_cast<std::int64_t>(i == 0 ? 1 : 0))
        .bind(5, stamp)
        .run();
  }
  // item.barcode 只作为主码镜像存在，主码始终取列表第一项。
  Stmt update(conn, "UPDATE item SET barcode = ? WHERE id = ?;");
  update.bind(1, codes.empty() ? std::string{} : codes.front()).bind(2, item_id).run();
}

void replace_item_barcodes(Db& db, std::int64_t item_id,
                           const std::vector<std::string>& raw) {
  db.write([&](sqlite3* conn) { replace_item_barcodes(conn, item_id, raw); });
}

}  // namespace xiaotu
