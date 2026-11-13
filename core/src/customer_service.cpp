#include "xiaotu/customer_service.h"

#include <sqlite3.h>

#include <string>
#include <utility>
#include <vector>

#include "xiaotu/audit.h"
#include "xiaotu/crud.h"
#include "xiaotu/error.h"
#include "xiaotu/partner_resolve.h"

namespace xiaotu {
namespace {

// partner 里的合并目标可能又指向别处，读取额度统一走既有解析。
std::int64_t resolve_partner(sqlite3* conn, std::int64_t customer_id, bool ensure) {
  if (customer_id <= 0) return 0;
  std::int64_t partner_id = find_partner_for_archive(conn, PartnerSide::Customer, customer_id);
  if (partner_id == 0 && ensure) {
    partner_id = ensure_partner_for_archive(conn, PartnerSide::Customer, customer_id);
  }
  return partner_id;
}

}  // namespace

std::int64_t CustomerService::credit_limit_cents(std::int64_t customer_id) {
  std::int64_t limit = 0;
  db_.read([&](sqlite3* conn) {
    const std::int64_t partner_id = resolve_partner(conn, customer_id, false);
    if (partner_id > 0) {
      Stmt stmt(conn, "SELECT credit_limit_cents FROM partner WHERE id = ?;");
      stmt.bind(1, partner_id);
      if (stmt.step()) limit = stmt.i64(0);
      return;
    }
    // 迁移前或异常数据没有 partner：回落到镜像，避免把额度当成 0（不限额）。
    Stmt stmt(conn, "SELECT credit_limit_cents FROM customer WHERE id = ? AND is_deleted = 0;");
    stmt.bind(1, customer_id);
    if (stmt.step()) limit = stmt.i64(0);
  });
  return limit;
}

std::vector<gen::CustomerRow> CustomerService::list() {
  std::vector<gen::CustomerRow> rows = base_.list();
  // 列表展示的额度以 partner 为准；没有 partner 的行保留镜像值。
  db_.read([&](sqlite3* conn) {
    for (auto& row : rows) {
      const std::int64_t partner_id = resolve_partner(conn, row.id, false);
      if (partner_id <= 0) continue;
      Stmt stmt(conn, "SELECT credit_limit_cents FROM partner WHERE id = ?;");
      stmt.bind(1, partner_id);
      if (stmt.step()) row.credit_limit_cents = stmt.i64(0);
    }
  });
  return rows;
}

void set_customer_credit_limit(sqlite3* conn, std::int64_t customer_id, std::int64_t cents) {
  if (customer_id <= 0) {
    throw AppError("INVALID_INPUT", "客户不能为空");
  }
  const std::int64_t partner_id = resolve_partner(conn, customer_id, true);
  if (partner_id == 0) {
    throw AppError("NOT_FOUND", "客户不存在，无法写入信用额度");
  }
  {
    Stmt partner(conn, "UPDATE partner SET credit_limit_cents = ? WHERE id = ?;");
    partner.bind(1, cents).bind(2, partner_id).run();
  }
  {
    Stmt mirror(conn, "UPDATE customer SET credit_limit_cents = ? WHERE id = ?;");
    mirror.bind(1, cents).bind(2, customer_id).run();
  }
}

std::size_t repair_customer_credit_mirror(sqlite3* conn) {
  std::vector<std::pair<std::int64_t, std::int64_t>> pending;
  {
    Stmt stmt(conn,
              "SELECT c.id, c.credit_limit_cents FROM customer c"
              " JOIN partner p ON p.id = c.id AND p.is_customer = 1"
              " WHERE c.is_deleted = 0 AND p.merged_into = 0"
              " AND p.credit_limit_cents = 0 AND c.credit_limit_cents <> 0;");
    while (stmt.step()) {
      pending.emplace_back(stmt.i64(0), stmt.i64(1));
    }
  }
  for (const auto& [customer_id, cents] : pending) {
    Stmt update(conn, "UPDATE partner SET credit_limit_cents = ? WHERE id = ?;");
    update.bind(1, cents).bind(2, customer_id).run();
  }
  return pending.size();
}

gen::CustomerRow CustomerService::create(gen::CustomerRow input) {
  validate(input);
  // 直接复用生成层的写入逻辑，但把额度写进同一个事务：
  // customer 行负责编码/名称等字段与镜像，partner 才是额度的事实来源。
  db_.write([&](sqlite3* conn) {
    ensure_unique(conn, "customer", "code", input.code, 0, "编码");
    gen::CustomerDao::insert(conn, input);
    set_customer_credit_limit(conn, input.id, input.credit_limit_cents);
    audit_write(conn, "create", "customer", input.id, input.code);
  });
  return input;
}

gen::CustomerRow CustomerService::update(std::int64_t id, gen::CustomerRow input) {
  validate(input);
  input.id = id;
  db_.write([&](sqlite3* conn) {
    ensure_unique(conn, "customer", "code", input.code, id, "编码");
    gen::CustomerDao::update(conn, input);
    set_customer_credit_limit(conn, id, input.credit_limit_cents);
    audit_write(conn, "update", "customer", id, input.code);
  });
  return input;
}

void CustomerService::remove(std::int64_t id) { base_.remove(id); }

void CustomerService::validate(const gen::CustomerRow& row) {
  if (row.code.empty()) {
    throw AppError("INVALID_INPUT", "编码不能为空");
  }
  if (row.name.empty()) {
    throw AppError("INVALID_INPUT", "名称不能为空");
  }
}

}  // namespace xiaotu
