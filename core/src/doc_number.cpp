#include "xiaotu/doc_number.h"

#include "xiaotu/db.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include "xiaotu/error.h"

namespace xiaotu {

std::string allocate_doc_no(Conn& conn, std::string_view prefix, std::string_view doc_date) {
  std::string period(doc_date);
  period.erase(std::remove(period.begin(), period.end(), '-'), period.end());
  if (period.size() != 8) {
    throw AppError("INVALID_INPUT", "单据日期格式应为 YYYY-MM-DD");
  }

  Stmt stmt(conn,
            "INSERT INTO doc_seq (doc_type, period_key, next_val) VALUES (?, ?, 1)"
            " ON CONFLICT (doc_type, period_key) DO UPDATE SET next_val = next_val + 1"
            " RETURNING next_val;");
  stmt.bind(1, prefix).bind(2, period);
  if (!stmt.step()) {
    throw AppError("DOC_NO", "单据号取号失败");
  }
  const std::int64_t sequence = stmt.i64(0);

  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%s%s-%04lld", std::string(prefix).c_str(),
                period.c_str(), static_cast<long long>(sequence));
  return std::string(buffer);
}

}  // namespace xiaotu
