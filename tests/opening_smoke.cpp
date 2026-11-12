#include <sqlite3.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "xiaotu/db.h"
#include "xiaotu/error.h"
#include "xiaotu/gen/customer_service.h"
#include "xiaotu/gen/item_service.h"
#include "xiaotu/gen/supplier_service.h"
#include "xiaotu/migrations.h"
#include "xiaotu/opening_import.h"
#include "xiaotu/xlsx.h"

namespace {

int failures = 0;

void check(bool condition, const std::string& label) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", label.c_str());
    ++failures;
  }
}

std::string xlsx(const std::vector<std::string>& header,
                 const std::vector<std::vector<std::string>>& rows,
                 const std::string& sheet_name = "期初") {
  xiaotu::XlsxSheet table(sheet_name);
  table.add_header_row(header);
  for (const auto& row : rows) table.add_text_row(row);
  return table.build();
}

std::int64_t count_rows(xiaotu::Db& db, const std::string& sql) {
  std::int64_t value = 0;
  db.read([&](sqlite3* conn) {
    xiaotu::Stmt stmt(conn, sql);
    if (stmt.step()) value = stmt.i64(0);
  });
  return value;
}

}  // namespace

int main() {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() /
      ("xiaotu-opening-" + std::to_string(std::rand()));
  std::filesystem::create_directories(dir);
  {
    xiaotu::Db db(dir / "app.db");
    xiaotu::migrate(db);
    check(db.schema_version() == 23, "schema 应为 v23");

    xiaotu::gen::ItemService items(db);
    const auto item = items.create(xiaotu::gen::ItemRow{.code = "OP001", .name = "期初商品"});
    xiaotu::gen::CustomerService customers(db);
    const auto customer =
        customers.create(xiaotu::gen::CustomerRow{.code = "OPC", .name = "期初客户"});
    xiaotu::gen::SupplierService suppliers(db);
    const auto supplier =
        suppliers.create(xiaotu::gen::SupplierRow{.code = "OPS", .name = "期初供应商"});

    // 成本单价为空必须整批拒绝，且不写入任何单据。
    auto report = xiaotu::import_opening(
        db, xiaotu::OpeningKind::Stock,
        xlsx({"仓库编码", "商品编码", "数量", "成本单价（元）"},
             {{"MAIN", "OP001", "5", ""}}),
        "stock.xlsx");
    check(!report.ok() && report.issues.size() == 1 &&
              report.issues[0].column == "成本单价（元）",
          "成本单价为空应整批拒绝");
    check(count_rows(db, "SELECT COUNT(*) FROM doc_header;") == 0,
          "被拒绝的导入不应写入单据");

    // 正常库存期初：5 个 @ 10.00 → 结存 5000 分、流水 1 条。
    report = xiaotu::import_opening(
        db, xiaotu::OpeningKind::Stock,
        xlsx({"仓库编码", "商品编码", "数量", "成本单价（元）"},
             {{"MAIN", "OP001", "5", "10.00"}}),
        "stock.xlsx");
    check(report.ok() && report.inserted == 1 && report.doc_nos.size() == 1,
          "库存期初应生成 1 张期初入库单");
    check(!report.backup.empty(), "库存期初应留下导入前备份");
    {
      std::int64_t qty = 0;
      std::int64_t amount = 0;
      db.read([&](sqlite3* conn) {
        xiaotu::Stmt stmt(conn,
                          "SELECT qty, amount_cents FROM stock_balance"
                          " WHERE warehouse_id = 1 AND item_id = ?;");
        stmt.bind(1, item.id);
        if (stmt.step()) {
          qty = stmt.i64(0);
          amount = stmt.i64(1);
        }
      });
      check(qty == 50000 && amount == 5000, "库存期初结存应为 5 个 / 50.00 元");
    }
    {
      std::string category;
      std::string status;
      db.read([&](sqlite3* conn) {
        xiaotu::Stmt stmt(conn,
                          "SELECT biz_category, status FROM doc_header"
                          " WHERE doc_type = 'OTH_IN';");
        if (stmt.step()) {
          category = stmt.text(0);
          status = stmt.text(1);
        }
      });
      check(category == "OPENING" && status == "APPROVED",
            "库存期初应为已审核的其他入库 + 期初类别");
    }

    // 已有库存流水后再次导入应拒绝。
    report = xiaotu::import_opening(
        db, xiaotu::OpeningKind::Stock,
        xlsx({"仓库编码", "商品编码", "数量", "成本单价（元）"},
             {{"MAIN", "OP001", "1", "10.00"}}),
        "stock.xlsx");
    check(!report.ok(), "已有库存流水后应拒绝再次导入期初");

    // 应收 / 应付期初：写 ar_ap_entry 并打期初标记，doc_id 为 0。
    report = xiaotu::import_opening(
        db, xiaotu::OpeningKind::Receivable,
        xlsx({"客户编码", "金额（元）", "业务日期", "到期日期"},
             {{"OPC", "1234.56", "2026-01-01", "2026-02-01"}}),
        "ar.xlsx");
    check(report.ok() && report.inserted == 1, "应收期初应写入 1 条");
    {
      std::int64_t opening = 0;
      std::int64_t direction = 0;
      std::int64_t amount = 0;
      std::int64_t doc_id = -1;
      std::int64_t partner_id = 0;
      db.read([&](sqlite3* conn) {
        xiaotu::Stmt stmt(conn,
                          "SELECT opening, direction, amount_cents, doc_id, partner_id"
                          " FROM ar_ap_entry WHERE doc_type = 'OPENING_AR';");
        if (stmt.step()) {
          opening = stmt.i64(0);
          direction = stmt.i64(1);
          amount = stmt.i64(2);
          doc_id = stmt.i64(3);
          partner_id = stmt.i64(4);
        }
      });
      check(opening == 1 && direction == 1 && amount == 123456 && doc_id == 0 &&
                partner_id == customer.id,
            "应收期初标记、方向、金额、doc_id 应正确");
    }
    report = xiaotu::import_opening(
        db, xiaotu::OpeningKind::Payable,
        xlsx({"供应商编码", "金额（元）", "业务日期", "到期日期"},
             {{"OPS", "500.00", "2026-01-02", ""}}),
        "ap.xlsx");
    check(report.ok() && report.inserted == 1, "应付期初应写入 1 条");
    {
      std::int64_t opening = 0;
      std::int64_t direction = 0;
      std::int64_t amount = 0;
      std::int64_t partner_id = 0;
      db.read([&](sqlite3* conn) {
        xiaotu::Stmt stmt(conn,
                          "SELECT opening, direction, amount_cents, partner_id"
                          " FROM ar_ap_entry WHERE doc_type = 'OPENING_AP';");
        if (stmt.step()) {
          opening = stmt.i64(0);
          direction = stmt.i64(1);
          amount = stmt.i64(2);
          partner_id = stmt.i64(3);
        }
      });
      check(opening == 1 && direction == -1 && amount == 50000 && partner_id == supplier.id,
            "应付期初方向应为 -1");
    }
    // 应收重复导入应拒绝，但与应付互不影响。
    report = xiaotu::import_opening(
        db, xiaotu::OpeningKind::Receivable,
        xlsx({"客户编码", "金额（元）", "业务日期", "到期日期"},
             {{"OPC", "1.00", "2026-01-03", ""}}),
        "ar.xlsx");
    check(!report.ok(), "应收期初重复导入应拒绝");

    // 资金账户期初：更新已有账户的期初余额，不新建记录。
    std::string account_code;
    db.read([&](sqlite3* conn) {
      xiaotu::Stmt stmt(conn, "SELECT code FROM fund_account WHERE is_deleted = 0 LIMIT 1;");
      if (stmt.step()) account_code = stmt.text(0);
    });
    check(!account_code.empty(), "应存在默认资金账户");
    report = xiaotu::import_opening(
        db, xiaotu::OpeningKind::Fund,
        xlsx({"账户编码", "期初余额（元）"}, {{account_code, "-123.45"}}), "fund.xlsx");
    check(report.ok() && report.inserted == 1, "资金账户期初应更新 1 个账户");
    {
      std::int64_t opening = 0;
      db.read([&](sqlite3* conn) {
        xiaotu::Stmt stmt(conn, "SELECT opening_cents FROM fund_account WHERE code = ?;");
        stmt.bind(1, account_code);
        if (stmt.step()) opening = stmt.i64(0);
      });
      check(opening == -12345, "资金账户期初可为负且按分存储");
    }
    report = xiaotu::import_opening(
        db, xiaotu::OpeningKind::Fund,
        xlsx({"账户编码", "期初余额（元）"}, {{account_code, "1.00"}}), "fund.xlsx");
    check(!report.ok(), "资金账户期初重复导入应拒绝");
  }
  std::error_code error;
  std::filesystem::remove_all(dir, error);
  if (failures == 0) std::printf("opening_smoke passed\n");
  return failures == 0 ? 0 : 1;
}
