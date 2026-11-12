// M0 冒烟测试：迁移 → 建商品 → 销售出库 → 采购入库与加权平均 → 反审核 → 查库存。
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <latch>
#include <string>
#include <thread>
#include <vector>

#include "xiaotu/audit.h"
#include "xiaotu/backup.h"
#include "xiaotu/document.h"
#include "xiaotu/edition.h"
#include "xiaotu/error.h"
#include "xiaotu/finance_service.h"
#include "xiaotu/gen/customer_service.h"
#include "xiaotu/gen/item_service.h"
#include "xiaotu/log.h"
#include "xiaotu/password.h"
#include "xiaotu/gen/supplier_service.h"
#include "xiaotu/gen/unit_service.h"
#include "xiaotu/gen/warehouse_service.h"
#include "xiaotu/migrations.h"
#include "xiaotu/cost_recalc.h"
#include "xiaotu/permissions.h"
#include "xiaotu/print_template.h"
#include "xiaotu/qty.h"
#include "xiaotu/report_service.h"
#include "xiaotu/settlement_service.h"
#include "xiaotu/stock_service.h"
#include "xiaotu/user_service.h"

namespace {

int g_failures = 0;

void check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << std::endl;
    ++g_failures;
  }
}

// 数量按 4 位定点整数存储；测试里用 q(件数) 换算，便于阅读。
std::int64_t q(std::int64_t units) { return units * 10000; }

}  // namespace

int main() {
  namespace fs = std::filesystem;
#if defined(_WIN32)
  const long process_id = _getpid();
#else
  const long process_id = ::getpid();
#endif
  const fs::path dir =
      fs::temp_directory_path() / ("xiaotu-smoke-" + std::to_string(process_id));

  try {
    fs::remove_all(dir);
    {
      xiaotu::Db db(dir / "app.db", 2);
      xiaotu::migrate(db);
      check(db.schema_version() == 13, "迁移后 schema 版本应为 13");

      xiaotu::gen::ItemService items(db);
      xiaotu::gen::WarehouseService warehouses(db);
      xiaotu::gen::SupplierService suppliers(db);
      xiaotu::gen::CustomerService customers(db);
      xiaotu::gen::UnitService units(db);
      xiaotu::DocumentEngine documents(db);
      xiaotu::StockService stock(db);

      const auto warehouse_list = warehouses.list();
      check(warehouse_list.size() == 1, "应内置一个默认仓库");
      check(warehouse_list.at(0).code == "MAIN", "默认仓库编码应为 MAIN");

      // 用户与角色：权限从表载入，内置 admin 不允许被删改到不可用。
      xiaotu::load_role_permissions(db);
      check(xiaotu::role_has_permission("admin", "user.write"), "管理员应拥有通配权限");
      check(xiaotu::role_has_permission("operator", "item.write"), "操作员应能维护商品");
      check(!xiaotu::role_has_permission("operator", "user.write"), "操作员不应能管理用户");
      check(xiaotu::role_has_permission("viewer", "item.read"), "只读角色应能查询商品");
      check(!xiaotu::role_has_permission("viewer", "item.write"), "只读角色不应能修改商品");
      check(!xiaotu::role_has_permission("ghost", "item.read"), "未登记角色应一律拒绝");
      check(xiaotu::role_has_permission("operator", "print.template.write"),
            "操作员应能维护打印模板");
      check(xiaotu::role_has_permission("viewer", "print.template.read"),
            "只读角色应能查看打印模板");
      check(!xiaotu::role_has_permission("viewer", "print.template.write"),
            "只读角色不应能修改打印模板");
      check(xiaotu::role_has_permission("admin", "cost.adjust"),
            "成本调整审核应只由管理员通配覆盖");
      check(!xiaotu::role_has_permission("operator", "cost.adjust"),
            "操作员不应能审核成本调整单");
      check(!xiaotu::role_has_permission("viewer", "cost.adjust"),
            "只读角色不应能审核成本调整单");

      xiaotu::UserService users(db);
      const auto seeded_users = users.list();
      check(seeded_users.size() == 1 && seeded_users.at(0).username == "admin",
            "迁移应内置 admin 用户");
      check(seeded_users.at(0).role == "admin", "内置用户角色应为 admin");
      check(users.count_active() == 1, "内置用户应计入活跃用户");

      const auto operator_user = users.create(
          xiaotu::UserInput{.username = "op1",
                            .display_name = "操作员一",
                            .role = "operator",
                            .password = "Passw0rd!"});
      check(operator_user.id > 0 && operator_user.role == "operator", "应能创建操作员");

      bool duplicate_user_rejected = false;
      try {
        users.create(xiaotu::UserInput{.username = "op1",
                                       .display_name = "重复",
                                       .role = "viewer",
                                       .password = "Passw0rd!"});
      } catch (const xiaotu::AppError& error) {
        duplicate_user_rejected = error.code() == "DUPLICATE_CODE";
      }
      check(duplicate_user_rejected, "重复用户名应被拒绝");

      bool bad_role_rejected = false;
      try {
        users.create(xiaotu::UserInput{.username = "ghost1", .role = "ghost",
                                       .password = "Passw0rd!"});
      } catch (const xiaotu::AppError& error) {
        bad_role_rejected = error.code() == "INVALID_ROLE";
      }
      check(bad_role_rejected, "未登记角色应被拒绝");

      users.update(operator_user.id, xiaotu::UserInput{.username = "op1",
                                                      .display_name = "只读一",
                                                      .role = "viewer",
                                                      .active = true});
      check(users.get(operator_user.id).role == "viewer", "用户角色应可调整");

      users.update(operator_user.id, xiaotu::UserInput{.username = "op1",
                                                      .display_name = "只读一",
                                                      .role = "viewer",
                                                      .active = false});
      check(users.count_active() == 1, "停用用户后活跃数应回到 1");

      users.remove(operator_user.id);
      check(users.list().size() == 1, "删除用户后列表只剩内置 admin");

      bool admin_delete_rejected = false;
      try {
        users.remove(1);
      } catch (const xiaotu::AppError& error) {
        admin_delete_rejected = error.code() == "USER_PROTECTED";
      }
      check(admin_delete_rejected, "内置管理员不应被删除");

      bool admin_demote_rejected = false;
      try {
        users.update(1, xiaotu::UserInput{.username = "admin", .display_name = "管理员",
                                          .role = "viewer"});
      } catch (const xiaotu::AppError& error) {
        admin_demote_rejected = error.code() == "USER_PROTECTED";
      }
      check(admin_demote_rejected, "内置管理员不应被降级");

      // 口令：Argon2id 散列、校验与登录失败统一错误。
      const std::string hashed = xiaotu::hash_password("s3cret-pass");
      check(hashed.rfind("$argon2id$", 0) == 0, "口令应使用 Argon2id 散列");
      check(xiaotu::verify_password(hashed, "s3cret-pass"), "正确口令应校验通过");
      check(!xiaotu::verify_password(hashed, "wrong-pass"), "错误口令应校验失败");
      check(!xiaotu::verify_password("", "s3cret-pass"), "空散列不应校验通过");
      check(!xiaotu::password_acceptable("short"), "少于 8 位的口令应被拒绝");

      const auto auth_user = users.create(xiaotu::UserInput{.username = "auth1",
                                                           .display_name = "登录测试",
                                                           .role = "operator",
                                                           .password = "Passw0rd!"});
      check(users.has_password(auth_user.id), "新建用户应存有口令散列");
      const auto logged_in = users.authenticate("auth1", "Passw0rd!");
      check(logged_in.id == auth_user.id, "口令登录应返回对应用户");
      bool wrong_login_rejected = false;
      try {
        users.authenticate("auth1", "bad-password");
      } catch (const xiaotu::AppError& error) {
        wrong_login_rejected = error.code() == "INVALID_CREDENTIALS";
      }
      check(wrong_login_rejected, "错误口令应统一返回 INVALID_CREDENTIALS");

      users.set_password(auth_user.id, "NewPassw0rd!");
      check(users.authenticate("auth1", "NewPassw0rd!").id == auth_user.id,
            "改口令后新口令应可登录");
      bool old_password_rejected = false;
      try {
        users.authenticate("auth1", "Passw0rd!");
      } catch (const xiaotu::AppError&) {
        old_password_rejected = true;
      }
      check(old_password_rejected, "旧口令在改密后应失效");

      // 版本能力：代码与构建参数一致，未登记能力默认不开放。
      const auto& edition = xiaotu::EditionPolicy::instance();
#ifdef XIAOTU_EDITION_CODE
      const std::string expected_edition = XIAOTU_EDITION_CODE;
#else
      const std::string expected_edition = "full";
#endif
      check(std::string(edition.code()) == expected_edition, "版本代码应与构建参数一致");
      check(edition.state("system.user") != "hidden", "两个版本都应开放用户管理");
      check(!edition.available("edition.not-exist"), "未登记能力应按不开放处理");

      if (expected_edition == "community") {
        check(edition.limit("max_users") == 1, "社区版用户额度应为 1");
        check(!edition.available("deployment.network"), "社区版不开放网络部署");
        bool community_quota_full = false;
        try {
          edition.check_quota("max_users", 1, 1);
        } catch (const xiaotu::AppError& error) {
          community_quota_full = error.code() == "QUOTA_EXCEEDED";
        }
        check(community_quota_full, "社区版超出 1 个用户应被拒绝");
      } else {
        check(edition.limit("max_users") == 30, "全功能版用户额度应为 30");
        edition.check_quota("max_users", 29, 1);
        bool full_quota_full = false;
        try {
          edition.check_quota("max_users", 30, 1);
        } catch (const xiaotu::AppError& error) {
          full_quota_full = error.code() == "QUOTA_EXCEEDED";
        }
        check(full_quota_full, "全功能版超出 30 个用户应被拒绝");
      }

      auto item = items.create(xiaotu::gen::ItemRow{.code = "T001", .name = "测试商品"});
      check(!item.code.empty(), "商品应创建成功");

      bool duplicate_rejected = false;
      try {
        items.create(xiaotu::gen::ItemRow{.code = "T001", .name = "重复编码"});
      } catch (const xiaotu::AppError& error) {
        duplicate_rejected = error.code() == "DUPLICATE_CODE";
      }
      check(duplicate_rejected, "重复编码应被拒绝");

      // 库存不足：审核前整单校验，拒绝后不留下单据与流水。
      xiaotu::DocInput insufficient;
      insufficient.doc_type = "SAL_OUT";
      insufficient.warehouse_id = warehouse_list.at(0).id;
      insufficient.doc_date = "2026-11-12";
      insufficient.partner = "测试客户";
      insufficient.lines.push_back(
          xiaotu::DocLineInput{.item_id = item.id, .qty = q(5), .price_cents = 1000});
      bool insufficient_rejected = false;
      try {
        documents.save_and_approve(insufficient);
      } catch (const xiaotu::AppError& error) {
        insufficient_rejected = error.code() == "STOCK_INSUFFICIENT";
      }
      check(insufficient_rejected, "库存不足的出库应被拒绝");
      check(stock.balances().empty() && stock.ledger(item.id).empty() &&
                documents.list("SAL_OUT").empty(),
            "库存不足时不应写入任何单据或流水");

      xiaotu::DocInput opening;
      opening.doc_type = "PUR_IN";
      opening.warehouse_id = warehouse_list.at(0).id;
      opening.doc_date = "2026-11-12";
      opening.lines.push_back(
          xiaotu::DocLineInput{.item_id = item.id, .qty = q(5), .price_cents = 1000});
      const auto opening_doc = documents.save_and_approve(opening);

      xiaotu::DocInput input;
      input.doc_type = "SAL_OUT";
      input.warehouse_id = warehouse_list.at(0).id;
      input.doc_date = "2026-11-12";
      input.partner = "测试客户";
      input.lines.push_back(xiaotu::DocLineInput{.item_id = item.id, .qty = q(5), .price_cents = 1000});

      const auto created = documents.save_and_approve(input);
      check(!created.doc_no.empty(), "应生成单据号");

      const auto balances = stock.balances();
      check(balances.size() == 1, "应有一条库存余额");
      check(balances.at(0).qty == q(0), "出库 5 件后余额应为 0");

      const auto ledger = stock.ledger(item.id);
      check(ledger.size() == 2, "应有一条入库流水和一条出库流水");
      check(ledger.at(0).qty == q(-5), "出库流水数量应为 -5");

      const auto listed = documents.list("SAL_OUT");
      check(listed.size() == 1 && listed.at(0).status == "APPROVED", "单据应为已审核状态");

      documents.reverse(created.id);
      const auto after_reverse = stock.balances();
      check(after_reverse.at(0).qty == q(5), "反审核后库存余额应回到 5");
      check(stock.ledger(item.id).size() == 3, "反审核应追加一条反向流水，而不是删除原流水");

      bool double_reverse_rejected = false;
      try {
        documents.reverse(created.id);
      } catch (const xiaotu::AppError& error) {
        double_reverse_rejected = error.code() == "INVALID_STATUS";
      }
      check(double_reverse_rejected, "重复反审核应被拒绝");

      documents.reverse(opening_doc.id);
      check(stock.balances().at(0).qty == q(0), "反审核期初入库后余额应回到 0");

      // 采购入库：按单据金额入账，出库成本走移动加权平均。
      auto purchased = items.create(xiaotu::gen::ItemRow{.code = "P001", .name = "采购商品"});
      const auto balance_of = [&stock](std::int64_t item_id) {
        for (const auto& row : stock.balances()) {
          if (row.item_id == item_id && row.warehouse_name == "主仓库") return row;
        }
        return xiaotu::StockBalanceRow{};
      };
      const auto make_doc = [&](const std::string& doc_type, std::int64_t item_id,
                                std::int64_t qty, std::int64_t price_cents) {
        xiaotu::DocInput doc;
        doc.doc_type = doc_type;
        doc.warehouse_id = warehouse_list.at(0).id;
        doc.doc_date = "2026-11-12";
        doc.partner = "测试往来单位";
        doc.lines.push_back(xiaotu::DocLineInput{.item_id = item_id, .qty = qty,
                                                 .price_cents = price_cents});
        return documents.save_and_approve(doc);
      };

      const auto first_purchase = make_doc("PUR_IN", purchased.id, q(10), 1000);
      check(first_purchase.doc_no.rfind("CGRK", 0) == 0, "采购入库单号应以 CGRK 开头");
      auto purchased_balance = balance_of(purchased.id);
      check(purchased_balance.qty == q(10), "采购入库后数量应为 10");
      check(purchased_balance.amount_cents == 10000, "采购入库后金额应为 10000 分");

      make_doc("PUR_IN", purchased.id, q(10), 1400);
      purchased_balance = balance_of(purchased.id);
      check(purchased_balance.qty == q(20), "两笔入库后数量应为 20");
      check(purchased_balance.amount_cents == 24000, "两笔入库后金额应为 24000 分");

      // 出库 5 件，成本 = 24000 * 5 / 20 = 6000 分，结存应为 15 件 / 18000 分。
      xiaotu::DocInput sale;
      sale.doc_type = "SAL_OUT";
      sale.warehouse_id = warehouse_list.at(0).id;
      sale.doc_date = "2026-11-12";
      sale.lines.push_back(xiaotu::DocLineInput{.item_id = purchased.id, .qty = q(5),
                                                .price_cents = 2000});
      const auto sale_doc = documents.save_and_approve(sale);
      purchased_balance = balance_of(purchased.id);
      check(purchased_balance.qty == q(15), "加权平均出库后数量应为 15");
      check(purchased_balance.amount_cents == 18000, "加权平均出库后金额应为 18000 分");

      documents.reverse(sale_doc.id);
      purchased_balance = balance_of(purchased.id);
      check(purchased_balance.qty == q(20) && purchased_balance.amount_cents == 24000,
            "反审核出库后应回到 20 件 / 24000 分");

      documents.reverse(first_purchase.id);
      purchased_balance = balance_of(purchased.id);
      check(purchased_balance.qty == q(10), "反审核首笔入库后数量应为 10");
      check(purchased_balance.amount_cents == 14000, "反审核首笔入库后金额应为 14000 分");
      check(stock.ledger(purchased.id).size() == 5,
            "采购入库链路应保留原流水并追加 REVERSE（共 5 条）");

      const auto purchase_docs = documents.list("PUR_IN");
      int voided_purchases = 0;
      for (const auto& doc : purchase_docs) {
        if (doc.status == "VOIDED") ++voided_purchases;
      }
      check(purchase_docs.size() == 3 && voided_purchases == 2,
            "应有三张采购入库单，期初与首笔采购被反审核为 VOIDED");

      // 调拨：源仓出、目标仓入，同一事务；不足时整单拒绝。
      const auto aux = warehouses.create(
          xiaotu::gen::WarehouseRow{.code = "AUX", .name = "分仓"});
      const auto balance_in = [&stock](const std::string& warehouse_name, std::int64_t item_id) {
        for (const auto& row : stock.balances()) {
          if (row.warehouse_name == warehouse_name && row.item_id == item_id) return row;
        }
        return xiaotu::StockBalanceRow{};
      };

      xiaotu::DocInput transfer;
      transfer.doc_type = "STK_TRF";
      transfer.warehouse_id = warehouse_list.at(0).id;
      transfer.target_warehouse_id = aux.id;
      transfer.doc_date = "2026-11-12";
      transfer.lines.push_back(
          xiaotu::DocLineInput{.item_id = purchased.id, .qty = q(4), .price_cents = 0});
      const auto transfer_doc = documents.save_and_approve(transfer);
      check(transfer_doc.doc_no.rfind("DBD", 0) == 0, "调拨单号应以 DBD 开头");
      auto source_balance = balance_in("主仓库", purchased.id);
      auto target_balance = balance_in("分仓", purchased.id);
      check(source_balance.qty == q(6) && source_balance.amount_cents == 8400,
            "调拨后源仓应为 6 件 / 8400 分");
      check(target_balance.qty == q(4) && target_balance.amount_cents == 5600,
            "调拨后目标仓应为 4 件 / 5600 分");

      bool over_transfer_rejected = false;
      try {
        xiaotu::DocInput too_much = transfer;
        too_much.lines.clear();
        too_much.lines.push_back(
            xiaotu::DocLineInput{.item_id = purchased.id, .qty = q(999), .price_cents = 0});
        documents.save_and_approve(too_much);
      } catch (const xiaotu::AppError& error) {
        over_transfer_rejected = error.code() == "STOCK_INSUFFICIENT";
      }
      check(over_transfer_rejected, "调拨数量超过源仓结存应被拒绝");

      documents.reverse(transfer_doc.id);
      source_balance = balance_in("主仓库", purchased.id);
      target_balance = balance_in("分仓", purchased.id);
      check(source_balance.qty == q(10) && source_balance.amount_cents == 14000,
            "反审核调拨后源仓应回到 10 件 / 14000 分");
      check(target_balance.qty == q(0) && target_balance.amount_cents == 0,
            "反审核调拨后目标仓应清零");

      // 盘点：明细为实盘数，差额生成调整流水。
      xiaotu::DocInput stocktake;
      stocktake.doc_type = "STK_CNT";
      stocktake.warehouse_id = warehouse_list.at(0).id;
      stocktake.doc_date = "2026-11-12";
      stocktake.lines.push_back(
          xiaotu::DocLineInput{.item_id = purchased.id, .qty = q(7), .price_cents = 0});
      const auto stocktake_doc = documents.save_and_approve(stocktake);
      check(stocktake_doc.doc_no.rfind("PDD", 0) == 0, "盘点单号应以 PDD 开头");
      auto counted = balance_of(purchased.id);
      check(counted.qty == q(7) && counted.amount_cents == 9800,
            "盘亏 3 件后应为 7 件 / 9800 分");

      documents.reverse(stocktake_doc.id);
      counted = balance_of(purchased.id);
      check(counted.qty == q(10) && counted.amount_cents == 14000, "反审核盘点后应回到 10 件 / 14000 分");

      xiaotu::DocInput surplus;
      surplus.doc_type = "STK_CNT";
      surplus.warehouse_id = warehouse_list.at(0).id;
      surplus.doc_date = "2026-11-12";
      surplus.lines.push_back(
          xiaotu::DocLineInput{.item_id = purchased.id, .qty = q(12), .price_cents = 0});
      const auto surplus_doc = documents.save_and_approve(surplus);
      counted = balance_of(purchased.id);
      check(counted.qty == q(12) && counted.amount_cents == 16800,
            "盘盈 2 件后应为 12 件 / 16800 分");
      documents.reverse(surplus_doc.id);
      check(balance_of(purchased.id).qty == q(10), "反审核盘盈后应回到 10 件");

      bool negative_stocktake_rejected = false;
      try {
        xiaotu::DocInput negative = surplus;
        negative.lines.clear();
        negative.lines.push_back(
            xiaotu::DocLineInput{.item_id = purchased.id, .qty = q(-1), .price_cents = 0});
        documents.save_and_approve(negative);
      } catch (const xiaotu::AppError& error) {
        negative_stocktake_rejected = error.code() == "INVALID_INPUT";
      }
      check(negative_stocktake_rejected, "实盘数量为负数应被拒绝");

      // 数量小数：千克 3 位；金额仍按行金额四舍五入到分。
      check(xiaotu::qty_text(xiaotu::qty_from_int(5)) == "5", "整数数量展示不应带小数点");
      check(xiaotu::qty_text(105000) == "10.5", "定点数量应去掉末尾 0");
      check(xiaotu::mul_qty_price(15000, 2000) == 3000, "1.5 × ¥20.00 应为 30.00 元");
      check(!xiaotu::parse_qty("1.2345", 3).has_value(), "千克不应接受 4 位小数");
      check(xiaotu::qty_fits_decimals(105000, 3), "10.5 千克应满足 3 位精度");

      const auto decimal_item = items.create(
          xiaotu::gen::ItemRow{.code = "KG001", .name = "散装面粉", .unit_id = 7});
      check(decimal_item.unit == "千克", "引用千克单位时应回填单位名称");

      make_doc("PUR_IN", decimal_item.id, 105000, 1234);
      auto decimal_balance = balance_of(decimal_item.id);
      check(decimal_balance.qty == 105000 && decimal_balance.amount_cents == 12957,
            "10.5 千克 × 12.34 元应入账 129.57 元");

      make_doc("PUR_IN", decimal_item.id, 32500, 1200);
      decimal_balance = balance_of(decimal_item.id);
      check(decimal_balance.qty == 137500 && decimal_balance.amount_cents == 16857,
            "两次入库后应为 13.75 千克 / 168.57 元");

      make_doc("OTH_OUT", decimal_item.id, 40000, 2000);
      decimal_balance = balance_of(decimal_item.id);
      check(decimal_balance.qty == 97500 && decimal_balance.amount_cents == 11953,
            "出库 4 千克应结转 49.04 元成本");

      // 盘点实盘 0 必须允许：货卖光是正常业务。
      xiaotu::DocInput empty_count;
      empty_count.doc_type = "STK_CNT";
      empty_count.warehouse_id = warehouse_list.at(0).id;
      empty_count.doc_date = "2026-11-12";
      empty_count.lines.push_back(
          xiaotu::DocLineInput{.item_id = decimal_item.id, .qty = 0, .price_cents = 0});
      const auto empty_count_doc = documents.save_and_approve(empty_count);
      decimal_balance = balance_of(decimal_item.id);
      check(decimal_balance.qty == 0 && decimal_balance.amount_cents == 0,
            "实盘 0 应把账面数量与金额一起清零");
      documents.reverse(empty_count_doc.id);
      check(balance_of(decimal_item.id).qty == 97500, "反审核实盘 0 后应恢复 9.75 千克");

      bool decimal_rejected = false;
      try {
        make_doc("PUR_IN", decimal_item.id, 12345, 1000);  // 1.2345 千克
      } catch (const xiaotu::AppError& error) {
        decimal_rejected = error.code() == "INVALID_INPUT";
      }
      check(decimal_rejected, "超过单位精度的数量应被拒绝");

      // 供应商档案：CRUD、编码唯一，并能被采购单据引用。
      const auto supplier = suppliers.create(
          xiaotu::gen::SupplierRow{.code = "GYS001", .name = "测试供应商", .contact = "张三"});
      check(supplier.id > 0 && supplier.name == "测试供应商", "供应商应创建成功");
      bool duplicate_supplier_rejected = false;
      try {
        suppliers.create(xiaotu::gen::SupplierRow{.code = "GYS001", .name = "重复供应商"});
      } catch (const xiaotu::AppError& error) {
        duplicate_supplier_rejected = error.code() == "DUPLICATE_CODE";
      }
      check(duplicate_supplier_rejected, "重复供应商编码应被拒绝");

      const auto supplier_item =
          items.create(xiaotu::gen::ItemRow{.code = "G001", .name = "供应商商品"});
      xiaotu::DocInput supplier_doc;
      supplier_doc.doc_type = "PUR_IN";
      supplier_doc.warehouse_id = warehouse_list.at(0).id;
      supplier_doc.doc_date = "2026-11-12";
      supplier_doc.partner_id = supplier.id;
      supplier_doc.partner = "手工填写的名称应被档案覆盖";
      supplier_doc.lines.push_back(
          xiaotu::DocLineInput{.item_id = supplier_item.id, .qty = q(2), .price_cents = 500});
      const auto supplier_doc_created = documents.save_and_approve(supplier_doc);
      const auto supplier_doc_detail = documents.detail(supplier_doc_created.id);
      check(supplier_doc_detail.partner_id == supplier.id, "采购单据应记录供应商 id");
      check(supplier_doc_detail.partner == "测试供应商", "单据往来单位应取供应商档案名称");

      bool missing_supplier_rejected = false;
      try {
        xiaotu::DocInput bad;
        bad.doc_type = "PUR_IN";
        bad.warehouse_id = warehouse_list.at(0).id;
        bad.doc_date = "2026-11-12";
        bad.partner_id = 999999;
        bad.lines.push_back(
            xiaotu::DocLineInput{.item_id = supplier_item.id, .qty = q(1), .price_cents = 100});
        documents.save_and_approve(bad);
      } catch (const xiaotu::AppError& error) {
        missing_supplier_rejected = error.code() == "NOT_FOUND";
      }
      check(missing_supplier_rejected, "引用不存在的供应商应被拒绝");

      // 计量单位档案：商品引用档案时回填名称快照，老数据 unit_id=0 继续按文本读取。
      check(units.list().size() == 7, "迁移应内置 7 个计量单位");
      const auto bottled =
          items.create(xiaotu::gen::ItemRow{.code = "U001", .name = "瓶装商品", .unit_id = 2});
      check(bottled.unit == "瓶", "商品应引用单位档案并回填单位名称");
      bool bad_unit_rejected = false;
      try {
        items.create(xiaotu::gen::ItemRow{.code = "U002", .name = "错误单位商品", .unit_id = 999999});
      } catch (const xiaotu::AppError& error) {
        bad_unit_rejected = error.code() == "NOT_FOUND";
      }
      check(bad_unit_rejected, "引用不存在的计量单位应被拒绝");
      const auto bottled_updated =
          items.update(bottled.id, xiaotu::gen::ItemRow{.code = "U001",
                                                       .name = "瓶装商品（改名）",
                                                       .unit_id = 2});
      check(bottled_updated.name == "瓶装商品（改名）" && bottled_updated.unit == "瓶",
            "商品更新应保留单位引用");

      // 客户档案与往来台账：销售应收、采购应付，退货红字，反审核作废。
      const auto customer = customers.create(
          xiaotu::gen::CustomerRow{.code = "KH001", .name = "测试客户档案"});
      check(customer.id > 0 && customer.name == "测试客户档案", "客户应创建成功");
      xiaotu::FinanceService finance(db);
      const auto sales_item =
          items.create(xiaotu::gen::ItemRow{.code = "F001", .name = "往来商品", .unit_id = 1});
      {
        xiaotu::DocInput opening;
        opening.doc_type = "PUR_IN";
        opening.warehouse_id = warehouse_list.at(0).id;
        opening.doc_date = "2026-11-12";
        opening.lines.push_back(
            xiaotu::DocLineInput{.item_id = sales_item.id, .qty = q(10), .price_cents = 1000});
        documents.save_and_approve(opening);
      }

      xiaotu::DocInput customer_sale;
      customer_sale.doc_type = "SAL_OUT";
      customer_sale.warehouse_id = warehouse_list.at(0).id;
      customer_sale.doc_date = "2026-11-12";
      customer_sale.partner_id = customer.id;
      customer_sale.partner = "手工名称应被档案覆盖";
      customer_sale.lines.push_back(
          xiaotu::DocLineInput{.item_id = sales_item.id, .qty = q(2), .price_cents = 2000});
      const auto customer_sale_doc = documents.save_and_approve(customer_sale);
      const std::string customer_key = "C:" + std::to_string(customer.id);
      const auto ar_rows = finance.entries(1, customer_key);
      check(ar_rows.size() == 1, "销售出库应生成一条应收记录");
      check(ar_rows.at(0).amount_cents == 4000 && ar_rows.at(0).outstanding_cents == 4000,
            "应收金额应为 4000 分且未核销");
      check(ar_rows.at(0).partner_id == customer.id && ar_rows.at(0).partner_name == "测试客户档案",
            "应收应引用客户档案");
      check(documents.detail(customer_sale_doc.id).partner == "测试客户档案",
            "销售单据应取客户档案名称");

      documents.reverse(customer_sale_doc.id);
      check(finance.entries(1, customer_key).at(0).status == "VOIDED",
            "反审核后应收记录应作废");

      xiaotu::DocInput customer_return;
      customer_return.doc_type = "SAL_RET";
      customer_return.warehouse_id = warehouse_list.at(0).id;
      customer_return.doc_date = "2026-11-12";
      customer_return.partner_id = customer.id;
      customer_return.lines.push_back(
          xiaotu::DocLineInput{.item_id = sales_item.id, .qty = q(1), .price_cents = 2000});
      documents.save_and_approve(customer_return);
      check(finance.entries(1, customer_key).at(0).amount_cents == -2000,
            "销售退货应生成负数应收（红字）");

      const auto ap_rows = finance.entries(-1, "S:" + std::to_string(supplier.id));
      check(ap_rows.size() == 1 && ap_rows.at(0).amount_cents == 1000,
            "采购入库应生成应付记录");

      // 核销：部分核销、多单合并、超额/重复拒绝、撤销用反向记录。
      xiaotu::SettlementService settlements(db);
      const auto sale_a = [&] {
        xiaotu::DocInput doc;
        doc.doc_type = "SAL_OUT";
        doc.warehouse_id = warehouse_list.at(0).id;
        doc.doc_date = "2026-11-12";
        doc.partner_id = customer.id;
        doc.lines.push_back(
            xiaotu::DocLineInput{.item_id = sales_item.id, .qty = q(3), .price_cents = 1000});
        return documents.save_and_approve(doc);
      }();
      xiaotu::DocInput sale_b_input;
      sale_b_input.doc_type = "SAL_OUT";
      sale_b_input.warehouse_id = warehouse_list.at(0).id;
      sale_b_input.doc_date = "2026-11-12";
      sale_b_input.partner_id = customer.id;
      sale_b_input.lines.push_back(
          xiaotu::DocLineInput{.item_id = sales_item.id, .qty = q(2), .price_cents = 1000});
      const auto sale_b = documents.save_and_approve(sale_b_input);

      const auto entry_of = [&](std::int64_t doc_id) {
        for (const auto& row : finance.entries(1, customer_key)) {
          if (row.doc_id == doc_id) return row;
        }
        throw xiaotu::AppError("TEST", "找不到往来记录");
      };
      const auto entry_a = entry_of(sale_a.id);
      const auto entry_b = entry_of(sale_b.id);
      check(entry_a.amount_cents == 3000 && entry_b.amount_cents == 2000, "两笔应收金额应正确");

      const auto make_settlement = [&](int direction, const std::string& key,
                                       std::int64_t amount,
                                       std::vector<xiaotu::AllocationInput> allocations,
                                       const std::string& note) {
        xiaotu::SettlementCreateInput input;
        input.direction = direction;
        input.partner_key = key;
        input.amount_cents = amount;
        input.account_id = 1;
        input.method = "CASH";
        input.settled_on = "2026-11-12";
        input.note = note;
        input.allocations = std::move(allocations);
        return settlements.create(input);
      };
      const auto first_settlement =
          make_settlement(1, customer_key, 1000, {{entry_a.id, 1000}}, "部分收款");
      check(first_settlement.settle_no.rfind("SK20261112-", 0) == 0, "收款单号应以 SK 开头");
      auto after_partial = entry_of(sale_a.id);
      check(after_partial.settled_cents == 1000 && after_partial.outstanding_cents == 2000 &&
                after_partial.status == "PARTIAL",
            "部分核销后应剩 2000 分未核销");

      bool over_settle_rejected = false;
      try {
        make_settlement(1, customer_key, 3000, {{entry_a.id, 3000}}, "");
      } catch (const xiaotu::AppError& error) {
        over_settle_rejected = error.code() == "SETTLE_OVER";
      }
      check(over_settle_rejected, "超额核销应被拒绝");

      bool duplicate_allocation_rejected = false;
      try {
        make_settlement(1, customer_key, 200,
                        {{entry_a.id, 100}, {entry_a.id, 100}}, "");
      } catch (const xiaotu::AppError& error) {
        duplicate_allocation_rejected = error.code() == "INVALID_INPUT";
      }
      check(duplicate_allocation_rejected, "同一单据重复分摊应被拒绝");

      bool cross_partner_rejected = false;
      try {
        make_settlement(1, "C:999999", 100, {{entry_a.id, 100}}, "");
      } catch (const xiaotu::AppError& error) {
        cross_partner_rejected = error.code() == "INVALID_INPUT";
      }
      check(cross_partner_rejected, "跨往来单位核销应被拒绝");

      const auto merged = make_settlement(1, customer_key, 3500,
                                          {{entry_a.id, 2000}, {entry_b.id, 1500}},
                                          "合并收款");
      check(merged.amount_cents == 3500, "一次核销多张单应成功");
      check(entry_of(sale_a.id).status == "SETTLED", "第一张单应核销完毕");
      check(entry_of(sale_b.id).settled_cents == 1500 && entry_of(sale_b.id).status == "PARTIAL",
            "第二张单应部分核销");

      settlements.reverse(first_settlement.id);
      const auto after_settle_reverse = entry_of(sale_a.id);
      check(after_settle_reverse.settled_cents == 2000 &&
                after_settle_reverse.outstanding_cents == 1000,
            "撤销核销后已核销金额应回退");

      bool double_settle_reverse_rejected = false;
      try {
        settlements.reverse(first_settlement.id);
      } catch (const xiaotu::AppError& error) {
        double_settle_reverse_rejected = error.code() == "INVALID_STATUS";
      }
      check(double_settle_reverse_rejected, "同一核销不能重复撤销");

      const auto settlement_rows = settlements.list(1, customer_key);
      check(settlement_rows.size() == 3, "应有核销、合并核销与撤销三条记录");
      check(settlement_rows.front().account_id == 1 && settlement_rows.front().method == "CASH",
            "收付款记录应带资金账户与结算方式");

      // 报表：流水明细、收发明细与账龄都从同一份流水/台账聚合，结果需自洽。
      xiaotu::ReportService reports(db);
      xiaotu::ReportFilter report_filter;
      report_filter.from = "2026-11-01";
      report_filter.to = "2026-11-12";
      report_filter.item_id = sales_item.id;
      const auto report_ledger = reports.stock_ledger(report_filter);
      check(!report_ledger.empty() && report_ledger.size() == stock.ledger(sales_item.id).size(),
            "流水明细应与库存流水条数一致");
      check(report_ledger.front().biz_time.rfind("2026-11-12", 0) == 0,
            "流水明细应按时间倒序取到当天记录");

      const auto flows = reports.goods_flow(report_filter);
      check(flows.size() == 1 && flows.front().closing_qty == balance_of(sales_item.id).qty,
            "收发明细期末数应等于库存余额");
      check(flows.front().opening_qty == q(0), "11-01 起期初应为 0");

      const auto inv_summary = reports.inventory_summary(report_filter);
      check(inv_summary.size() == 1 && inv_summary.front().item_id == sales_item.id,
            "进销存汇总应按商品返回一行");
      check(inv_summary.front().opening_qty == q(0) &&
                inv_summary.front().closing_qty == flows.front().closing_qty,
            "进销存汇总期末数量应等于收发明细期末");
      check(inv_summary.front().closing_amount_cents ==
                balance_of(sales_item.id).amount_cents,
            "进销存汇总期末金额应等于库存余额金额");

      // 销售毛利：收入取单据金额，成本取审核写入的库存流水；已反审核单据不计入。
      const auto profits = reports.sales_profit(report_filter);
      check(profits.size() == 1 && profits.front().item_id == sales_item.id,
            "销售毛利应按商品返回一行");
      // 成本取审核写入的流水：退货 +2000、两笔出库 3273 + 2182；已反审核的销售单不计入。
      check(profits.front().qty == q(4) && profits.front().revenue_cents == 3000 &&
                profits.front().cost_cents == 3455 && profits.front().profit_cents == -455,
            "销售毛利应为 4 件 / 收入 3000 / 成本 3455 / 毛利 -455 分");

      const auto aging = reports.ar_ap_aging(1, "2026-11-12");
      std::int64_t expected_ar = 0;
      for (const auto& row : finance.entries(1, customer_key)) {
        if (row.status != "VOIDED") expected_ar += row.outstanding_cents;
      }
      check(aging.size() == 1 && aging.front().total_cents == expected_ar,
            "账龄合计应等于未作废往来记录的未核销余额");
      check(aging.front().not_due_cents == expected_ar && aging.front().d90_plus_cents == 0,
            "截止日当天到期的记录应全部落在未到期桶");

      // 对账单：期初 + 单据 - 核销 = 期末，且期末与台账未核销余额一致。
      const auto statement =
          reports.partner_statement(customer_key, 1, "2026-11-01", "2026-11-30");
      check(statement.partner_name == "测试客户档案", "对账单应取客户档案名称");
      check(statement.opening_cents == 0, "11-01 起期初应为 0");
      check(statement.increase_cents == 6000, "本期增加应为两笔应收加核销撤销 6000 分");
      check(statement.decrease_cents == 6500, "本期减少应为退货与两笔核销 6500 分");
      check(statement.closing_cents == -500, "期末余额应为 -500 分");
      check(statement.lines.size() == 6, "对账单应列出 3 笔单据与 3 笔核销");
      std::int64_t statement_outstanding = 0;
      for (const auto& row : finance.entries(1, customer_key)) {
        if (row.status != "VOIDED") statement_outstanding += row.outstanding_cents;
      }
      check(statement.closing_cents == statement_outstanding,
            "对账单期末应等于台账未核销余额");
      check(statement.lines.back().balance_cents == statement.closing_cents,
            "最后一行的余额应为期末余额");

      const auto later_statement =
          reports.partner_statement(customer_key, 1, "2026-11-13", "");
      check(later_statement.opening_cents == -500 && later_statement.lines.empty() &&
                later_statement.closing_cents == -500,
            "期间之外的发生额应全部计入下一期期初");

      // 采购退货：先入库再退货，以及入库后部分退货。
      auto returned = items.create(xiaotu::gen::ItemRow{.code = "R001", .name = "退货商品"});
      make_doc("PUR_IN", returned.id, q(10), 1000);
      const auto purchase_return = make_doc("PUR_RET", returned.id, q(4), 1000);
      check(purchase_return.doc_no.rfind("CGTH", 0) == 0, "采购退货单号应以 CGTH 开头");
      auto returned_balance = balance_of(returned.id);
      check(returned_balance.qty == q(6) && returned_balance.amount_cents == 6000,
            "采购退货 4 件后应为 6 件 / 6000 分");

      const auto partial_return = make_doc("PUR_RET", returned.id, q(3), 1000);
      returned_balance = balance_of(returned.id);
      check(returned_balance.qty == q(3) && returned_balance.amount_cents == 3000,
            "部分退货再退 3 件后应为 3 件 / 3000 分");
      documents.reverse(partial_return.id);
      returned_balance = balance_of(returned.id);
      check(returned_balance.qty == q(6) && returned_balance.amount_cents == 6000,
            "反审核部分退货后应回到 6 件 / 6000 分");
      check(stock.ledger(returned.id).size() == 4,
            "采购退货链路应为入库 + 两次退货 + REVERSE 共 4 条流水");

      // 销售退货：先出库再退回，按成本单价入库。
      auto sales_returned = items.create(xiaotu::gen::ItemRow{.code = "S001", .name = "销退商品"});
      make_doc("PUR_IN", sales_returned.id, q(10), 1000);
      make_doc("SAL_OUT", sales_returned.id, q(4), 1500);
      const auto sales_return = make_doc("SAL_RET", sales_returned.id, q(2), 1000);
      check(sales_return.doc_no.rfind("XSTH", 0) == 0, "销售退货单号应以 XSTH 开头");
      auto sales_returned_balance = balance_of(sales_returned.id);
      check(sales_returned_balance.qty == q(8) && sales_returned_balance.amount_cents == 8000,
            "销售退货 2 件后应为 8 件 / 8000 分");
      documents.reverse(sales_return.id);
      sales_returned_balance = balance_of(sales_returned.id);
      check(sales_returned_balance.qty == q(6) && sales_returned_balance.amount_cents == 6000,
            "反审核销售退货后应回到 6 件 / 6000 分");

      // 草稿态：保存不写库存，审核时才在单事务内写流水。
      auto draft_item = items.create(xiaotu::gen::ItemRow{.code = "D001", .name = "草稿商品"});
      xiaotu::DocInput draft_input;
      draft_input.doc_type = "PUR_IN";
      draft_input.warehouse_id = warehouse_list.at(0).id;
      draft_input.doc_date = "2026-11-12";
      draft_input.partner = "草稿供应商";
      draft_input.lines.push_back(
          xiaotu::DocLineInput{.item_id = draft_item.id, .qty = q(5), .price_cents = 900});

      const auto draft = documents.save_draft(draft_input);
      check(draft.doc_no.rfind("CGRK", 0) == 0, "草稿也应取到正式单据号");
      check(stock.ledger(draft_item.id).empty(), "草稿不应写库存流水");
      auto draft_detail = documents.detail(draft.id);
      check(draft_detail.status == "DRAFT", "草稿状态应为 DRAFT");
      check(draft_detail.lines.size() == 1 && draft_detail.lines.at(0).qty == q(5),
            "草稿明细应保存");

      draft_input.lines.at(0).qty = q(7);
      draft_input.lines.at(0).price_cents = 1100;
      documents.update_draft(draft.id, draft_input);
      draft_detail = documents.detail(draft.id);
      check(draft_detail.lines.at(0).qty == q(7) && draft_detail.lines.at(0).price_cents == 1100,
            "草稿明细应可编辑");
      check(draft_detail.total_qty == q(7) && draft_detail.total_amount_cents == 7700,
            "草稿汇总金额应随明细更新");
      check(stock.ledger(draft_item.id).empty(), "编辑草稿仍不应写库存流水");

      documents.approve(draft.id);
      draft_detail = documents.detail(draft.id);
      check(draft_detail.status == "APPROVED", "审核后状态应为 APPROVED");
      auto draft_balance = balance_of(draft_item.id);
      check(draft_balance.qty == q(7) && draft_balance.amount_cents == 7700,
            "审核草稿后应入库 7 件 / 7700 分");
      check(stock.ledger(draft_item.id).size() == 1, "审核草稿应写一条库存流水");

      bool draft_locked = false;
      try {
        documents.update_draft(draft.id, draft_input);
      } catch (const xiaotu::AppError& error) {
        draft_locked = error.code() == "INVALID_STATUS";
      }
      check(draft_locked, "已审核单据不应再允许编辑");

      bool draft_delete_locked = false;
      try {
        documents.delete_draft(draft.id);
      } catch (const xiaotu::AppError& error) {
        draft_delete_locked = error.code() == "INVALID_STATUS";
      }
      check(draft_delete_locked, "已审核单据不应允许按草稿删除");

      const auto second_draft = documents.save_draft(draft_input);
      documents.delete_draft(second_draft.id);
      bool draft_deleted = false;
      try {
        documents.detail(second_draft.id);
      } catch (const xiaotu::AppError& error) {
        draft_deleted = error.code() == "NOT_FOUND";
      }
      check(draft_deleted, "删除草稿后应查不到该单据");

      // 预收：不填分摊也应保存，资金流水与余额要跟着变动。
      const auto prepayment = make_settlement(1, customer_key, 500, {}, "预收");
      check(prepayment.settle_no.rfind("SK", 0) == 0, "预收也应使用收款单号");
      const auto fund_ledger = settlements.ledger(1, "2026-11-01", "2026-11-12");
      check(!fund_ledger.empty() && fund_ledger.front().opening, "资金流水应包含期初行");
      check(fund_ledger.back().balance_cents == 4000, "资金流水期末余额应为 40.00 元");

      const auto fund_balances = settlements.account_balances("2026-11-12");
      check(fund_balances.size() == 1 && fund_balances.front().inflow_cents == 4000 &&
                fund_balances.front().outflow_cents == 0 &&
                fund_balances.front().balance_cents == 4000,
            "账户余额应为累计收款减累计付款");

      settlements.reverse(prepayment.id);
      check(settlements.ledger(1, "2026-11-01", "2026-11-12").back().balance_cents == 3500,
            "撤销预收后资金余额应回退");

      items.remove(draft_item.id);
      items.remove(sales_returned.id);
      items.remove(returned.id);
      items.remove(purchased.id);
      items.remove(item.id);
      items.remove(supplier_item.id);
      items.remove(bottled.id);
      items.remove(sales_item.id);
      items.remove(decimal_item.id);
      suppliers.remove(supplier.id);
      customers.remove(customer.id);
      check(items.list().empty(), "软删除后列表中不应再出现该商品");

      // 审计日志：关键写操作都留痕，且能回答谁/何时/对象/动作。
      const auto audit_rows = xiaotu::audit_recent(db, 500);
      const auto has_audit = [&audit_rows](const std::string& action, const std::string& entity) {
        for (const auto& row : audit_rows) {
          if (row.action == action && row.entity == entity && !row.actor.empty() &&
              !row.at.empty()) {
            return true;
          }
        }
        return false;
      };
      check(has_audit("create", "item"), "主数据新增应写审计日志");
      check(has_audit("update", "item"), "主数据修改应写审计日志");
      check(has_audit("delete", "item"), "主数据删除应写审计日志");
      check(has_audit("create", "supplier"), "供应商新增应写审计日志");
      check(has_audit("create", "user"), "用户变更应写审计日志");
      check(has_audit("approve", "document"), "单据审核应写审计日志");
      check(has_audit("save_draft", "document"), "单据存草稿应写审计日志");
      check(has_audit("update_draft", "document"), "草稿编辑应写审计日志");
      check(has_audit("delete_draft", "document"), "草稿删除应写审计日志");
      check(has_audit("reverse", "document"), "反审核应写审计日志");

      // 打印模板：列白名单、默认唯一、软删除回退与乐观锁。
      xiaotu::PrintTemplateService templates(db);
      check(xiaotu::print_targets().size() == 18, "打印目标应包含 10 种单据与 8 张报表");

      // 成本调整单：仓库与原因在明细行，只改结存金额；反审核不能把金额冲成负数。
      const auto adjust_item = items.create(
          xiaotu::gen::ItemRow{.code = "ADJ001", .name = "成本调整测试", .unit_id = 1});
      make_doc("PUR_IN", adjust_item.id, q(10), 1000);
      const std::int64_t adjust_wh = warehouse_list.at(0).id;

      xiaotu::DocInput adjust;
      adjust.doc_type = "CST_ADJ";
      adjust.doc_date = "2026-11-12";
      adjust.lines.push_back(xiaotu::DocLineInput{.item_id = adjust_item.id,
                                                  .qty = 0,
                                                  .price_cents = 0,
                                                  .warehouse_id = adjust_wh,
                                                  .adjust_cents = 500,
                                                  .adjust_reason = "运费分摊"});
      // 表头不传仓库（前端传 0），草稿也应取明细行仓库，不能撞 warehouse 外键。
      const auto adjust_draft_doc = documents.save_draft(adjust);
      check(documents.detail(adjust_draft_doc.id).warehouse_id == adjust_wh,
            "成本调整草稿的表头仓库应取明细行仓库");
      documents.delete_draft(adjust_draft_doc.id);
      const auto adjust_doc = documents.save_and_approve(adjust);
      check(adjust_doc.doc_no.rfind("CBTZ", 0) == 0, "成本调整单号应以 CBTZ 开头");
      auto adjust_balance = balance_of(adjust_item.id);
      check(adjust_balance.qty == q(10) && adjust_balance.amount_cents == 10500,
            "成本调整不应改变数量，只增加结存金额");
      const auto adjust_detail = documents.detail(adjust_doc.id);
      check(adjust_detail.lines.at(0).warehouse_id == adjust_wh &&
                adjust_detail.lines.at(0).adjust_reason == "运费分摊",
            "成本调整单应逐行保存仓库与原因");
      documents.reverse(adjust_doc.id);
      check(balance_of(adjust_item.id).amount_cents == 10000, "反审核成本调整应回退金额");

      bool negative_adjust_rejected = false;
      try {
        xiaotu::DocInput negative = adjust;
        negative.lines.at(0).adjust_cents = -10001;
        documents.save_and_approve(negative);
      } catch (const xiaotu::AppError& error) {
        negative_adjust_rejected = error.code() == "INVALID_INPUT";
      }
      check(negative_adjust_rejected, "调整后结存金额为负应被拒绝");

      const auto empty_item = items.create(
          xiaotu::gen::ItemRow{.code = "ADJ002", .name = "零库存商品", .unit_id = 1});
      bool zero_stock_rejected = false;
      try {
        xiaotu::DocInput zero_adjust = adjust;
        zero_adjust.lines.at(0).item_id = empty_item.id;
        zero_adjust.lines.at(0).adjust_cents = 100;
        documents.save_and_approve(zero_adjust);
      } catch (const xiaotu::AppError& error) {
        zero_stock_rejected = error.code() == "INVALID_INPUT";
      }
      check(zero_stock_rejected, "零库存商品不允许留下结存金额");

      // 调整金额已被后续出库结转后，反审核调整单会让金额变负，必须拒绝。
      xiaotu::DocInput adjust_later = adjust;
      adjust_later.lines.at(0).adjust_cents = 1000;
      const auto adjust_later_doc = documents.save_and_approve(adjust_later);
      const auto consume_doc = make_doc("OTH_OUT", adjust_item.id, q(10), 0);
      bool cost_reverse_rejected = false;
      try {
        documents.reverse(adjust_later_doc.id);
      } catch (const xiaotu::AppError& error) {
        cost_reverse_rejected = error.code() == "REVERSE_WOULD_NEGATIVE";
      }
      check(cost_reverse_rejected, "反审核会使结存金额为负时应拒绝");
      documents.reverse(consume_doc.id);
      documents.reverse(adjust_later_doc.id);
      check(balance_of(adjust_item.id).amount_cents == 10000, "处理后续业务后可正常反审核");

      items.remove(adjust_item.id);
      items.remove(empty_item.id);

      check(xiaotu::find_print_target("DOC", "OTH_IN") != nullptr,
            "其他入库应登记为打印目标");
      check(xiaotu::find_print_target("DOC", "OTH_OUT") != nullptr,
            "其他出库应登记为打印目标");
      check(xiaotu::find_print_target("DOC", "SAL_ORDER") != nullptr,
            "销售订单应登记为打印目标");
      const auto* ledger_target = xiaotu::find_print_target("REPORT", "stock-ledger");
      check(ledger_target != nullptr && !ledger_target->columns.empty(),
            "库存流水明细应登记列白名单");
      check(xiaotu::find_print_target("REPORT", "not-exist") == nullptr, "未知报表不应登记为目标");

      const auto builtin = templates.resolve("REPORT", "stock-ledger");
      check(builtin.id == 0 && builtin.name.find("内置默认") != std::string::npos,
            "未配置模板时应回退到内置默认");
      check(xiaotu::resolved_print_columns(builtin).size() == ledger_target->columns.size(),
            "内置默认应显示全部登记列");

      xiaotu::PrintTemplate tmpl;
      tmpl.code = "PT-LEDGER";
      tmpl.name = "流水打印";
      tmpl.target_type = "REPORT";
      tmpl.target_key = "stock-ledger";
      tmpl.is_default = true;
      tmpl.header_text = "测试单位";
      tmpl.columns = {
          {.column_key = "biz_time", .position = 1, .visible = true},
          {.column_key = "item_name", .position = 2, .visible = true},
      };
      const auto created_tmpl = templates.create(tmpl);
      check(created_tmpl.id > 0 && created_tmpl.version == 1 && created_tmpl.is_default, "应能创建默认模板");
      check(templates.resolve("REPORT", "stock-ledger").id == created_tmpl.id,
            "目标默认模板应优先于内置默认");

      auto second = tmpl;
      second.code = "PT-LEDGER-2";
      second.name = "流水打印二";
      const auto created_tmpl2 = templates.create(second);
      check(!templates.get(created_tmpl.id).is_default && templates.get(created_tmpl2.id).is_default,
            "同一目标只应保留一个默认模板");

      const auto switched = templates.set_default(created_tmpl.id);
      check(switched.is_default && !templates.get(created_tmpl2.id).is_default, "切换默认应取消旧默认");

      auto stale = switched;
      stale.version = 0;
      bool version_conflict = false;
      try {
        templates.update(created_tmpl.id, stale);
      } catch (const xiaotu::AppError& error) {
        version_conflict = error.code() == "VERSION_CONFLICT";
      }
      check(version_conflict, "版本不一致应拒绝更新");

      auto edited = templates.get(created_tmpl.id);
      edited.name = "流水打印（改）";
      edited.columns.at(1).visible = false;
      const auto saved = templates.update(created_tmpl.id, edited);
      check(saved.version == edited.version + 1 && saved.name == "流水打印（改）",
            "版本一致时应更新并递增版本");
      const auto rendered = xiaotu::resolved_print_columns(saved);
      check(rendered.size() == 1 && rendered.front().def.key == "biz_time",
            "隐藏列不应进入渲染列");

      auto hand_made = builtin;
      hand_made.columns.push_back(
          {.column_key = "removed-column", .position = 99, .visible = true});
      check(xiaotu::resolved_print_columns(hand_made).size() == ledger_target->columns.size(),
            "渲染时应跳过目标中已不存在的列");

      auto unknown_column = tmpl;
      unknown_column.code = "PT-BAD-COLUMN";
      unknown_column.columns = {{.column_key = "hack", .position = 1, .visible = true}};
      bool bad_column_rejected = false;
      try {
        templates.create(unknown_column);
      } catch (const xiaotu::AppError& error) {
        bad_column_rejected = error.code() == "INVALID_INPUT";
      }
      check(bad_column_rejected, "未知列应在保存时拒绝");

      auto unknown_target = tmpl;
      unknown_target.code = "PT-BAD-TARGET";
      unknown_target.target_key = "not-exist";
      bool bad_target_rejected = false;
      try {
        templates.create(unknown_target);
      } catch (const xiaotu::AppError& error) {
        bad_target_rejected = error.code() == "INVALID_INPUT";
      }
      check(bad_target_rejected, "未知目标应在保存时拒绝");

      templates.remove(created_tmpl.id);
      check(templates.resolve("REPORT", "stock-ledger").id == 0,
            "删除默认模板后应回退到内置默认");
      templates.remove(created_tmpl2.id);
      check(templates.list("REPORT", "stock-ledger").empty(), "软删除后列表应为空");

      // 显式模板失效（不存在或目标不匹配）时同样回退到目标的内置默认。
      check(templates.resolve("REPORT", "stock-ledger", 99999999).id == 0,
            "显式模板不存在时应回退内置默认");
      auto cross = tmpl;
      cross.code = "PT-CROSS";
      cross.name = "跨目标模板";
      cross.is_default = false;
      const auto cross_created = templates.create(cross);
      const auto mismatched =
          templates.resolve("REPORT", "stock-balance", cross_created.id);
      check(mismatched.id == 0 && mismatched.target_key == "stock-balance",
            "显式模板与目标不匹配时应回退该目标的内置默认");
      templates.remove(cross_created.id);

      // 字段范围校验：越界值在保存阶段就被拒绝，不依赖数据库 CHECK 报 500。
      const auto rejects = [&](const xiaotu::PrintTemplate& candidate) {
        try {
          templates.create(candidate);
        } catch (const xiaotu::AppError& error) {
          return error.code() == "INVALID_INPUT";
        }
        return false;
      };
      auto candidate = tmpl;
      candidate.code = "PT-RANGE-MARGIN";
      candidate.margin_mm = 51;
      check(rejects(candidate), "页边距超过 50mm 应拒绝");
      candidate = tmpl;
      candidate.code = "PT-RANGE-FONT";
      candidate.font_size_pt = 5;
      check(rejects(candidate), "字号小于 6pt 应拒绝");
      candidate = tmpl;
      candidate.code = "PT-RANGE-PAPER";
      candidate.page_size = "CUSTOM";
      check(rejects(candidate), "自定义纸张未填宽高应拒绝");
      candidate = tmpl;
      candidate.code = "PT-RANGE-HEADER";
      candidate.header_text = std::string(501, 'x');
      check(rejects(candidate), "页眉超过 500 字符应拒绝");
      candidate = tmpl;
      candidate.code = "PT-RANGE-WIDTH";
      candidate.columns.at(0).width_mm = 501;
      check(rejects(candidate), "列宽超过 500mm 应拒绝");
      candidate = tmpl;
      candidate.code = "PT-RANGE-DUP";
      candidate.columns.push_back(
          {.column_key = "biz_time", .position = 3, .visible = true});
      check(rejects(candidate), "同一模板重复列应拒绝");

      // 销售订单：确认不动库存/流水，转出库受累计数量约束。
      auto ordered = items.create(xiaotu::gen::ItemRow{.code = "SO001", .name = "订单商品"});
      make_doc("PUR_IN", ordered.id, q(10), 500);
      const auto ordered_before = balance_of(ordered.id);
      const auto ordered_ledger_before = stock.ledger(ordered.id).size();

      xiaotu::DocInput order_input;
      order_input.doc_type = "SAL_ORDER";
      order_input.warehouse_id = warehouse_list.at(0).id;
      order_input.doc_date = "2026-11-12";
      order_input.partner = "订单客户";
      order_input.lines.push_back(
          {.item_id = ordered.id, .qty = q(6), .price_cents = 1500});
      const auto order = documents.save_and_approve(order_input);
      check(order.doc_no.rfind("XSDD", 0) == 0, "销售订单单号应以 XSDD 开头");
      auto order_detail = documents.detail(order.id);
      check(order_detail.order_status == "CONFIRMED", "保存并确认后订单应为 CONFIRMED");
      check(balance_of(ordered.id).qty == ordered_before.qty,
            "订单确认不应改变库存数量");
      check(balance_of(ordered.id).amount_cents == ordered_before.amount_cents,
            "订单确认不应改变库存金额");
      check(stock.ledger(ordered.id).size() == ordered_ledger_before,
            "订单确认不应写库存流水");
      check(documents.list("SAL_ORDER").at(0).order_status == "CONFIRMED",
            "订单列表应返回 order_status");
      const std::int64_t order_line_id = order_detail.lines.at(0).id;
      check(order_detail.lines.at(0).qty - order_detail.lines.at(0).converted_qty == q(6),
            "订单行剩余数量应为 6");

      xiaotu::OrderConvertInput convert;
      convert.lines.push_back({.order_line_id = order_line_id, .qty = q(4)});
      const auto partial = documents.convert_order(order.id, convert);
      check(partial.doc_no.rfind("XSCK", 0) == 0, "转出库应生成销售出库单");
      order_detail = documents.detail(order.id);
      check(order_detail.order_status == "CONFIRMED" &&
                order_detail.lines.at(0).converted_qty == q(4),
            "部分转出后订单应保持 CONFIRMED 且累计 4 件");
      check(balance_of(ordered.id).qty == ordered_before.qty - q(4),
            "转出库应实际扣减库存");

      convert.lines.at(0).qty = q(3);
      bool exceeded = false;
      try {
        documents.convert_order(order.id, convert);
      } catch (const xiaotu::AppError& error) {
        exceeded = error.code() == "ORDER_QTY_EXCEEDED";
      }
      check(exceeded, "累计转出超过订单数量应拒绝");

      convert.lines.at(0).qty = q(2);
      const auto full = documents.convert_order(order.id, convert);
      check(full.order_status == "CONVERTED", "全部转出后订单应为 CONVERTED");
      bool no_more = false;
      try {
        documents.convert_order(order.id, convert);
      } catch (const xiaotu::AppError& error) {
        no_more = error.code() == "INVALID_STATUS";
      }
      check(no_more, "已全部转出的订单应拒绝继续转出");

      documents.reverse(partial.doc_id);
      order_detail = documents.detail(order.id);
      check(order_detail.order_status == "CONFIRMED" &&
                order_detail.lines.at(0).converted_qty == q(2),
            "反审核出库单应释放对应订单数量");

      documents.close_order(order.id);
      check(documents.detail(order.id).order_status == "CLOSED", "订单应可关闭");
      bool closed_rejected = false;
      convert.lines.at(0).qty = q(1);
      try {
        documents.convert_order(order.id, convert);
      } catch (const xiaotu::AppError& error) {
        closed_rejected = error.code() == "INVALID_STATUS";
      }
      check(closed_rejected, "关闭后应拒绝转出");

      // 其他入库/出库：入库按手工单价，出库按移动加权平均，且不产生往来。
      auto other = items.create(xiaotu::gen::ItemRow{.code = "O001", .name = "其他商品"});
      xiaotu::DocInput other_in;
      other_in.doc_type = "OTH_IN";
      other_in.warehouse_id = warehouse_list.at(0).id;
      other_in.doc_date = "2026-11-12";
      other_in.partner = "备注：受赠样品";
      other_in.lines.push_back(
          xiaotu::DocLineInput{.item_id = other.id, .qty = q(4), .price_cents = 250});
      const auto other_in_doc = documents.save_and_approve(other_in);
      check(other_in_doc.doc_no.rfind("QTRK", 0) == 0, "其他入库单号应以 QTRK 开头");
      auto other_balance = balance_of(other.id);
      check(other_balance.qty == q(4) && other_balance.amount_cents == 1000,
            "其他入库应按录入单价入账");

      xiaotu::DocInput other_out;
      other_out.doc_type = "OTH_OUT";
      other_out.warehouse_id = warehouse_list.at(0).id;
      other_out.doc_date = "2026-11-12";
      other_out.partner = "备注：领用";
      other_out.lines.push_back(
          xiaotu::DocLineInput{.item_id = other.id, .qty = q(1), .price_cents = 9999});
      const auto other_out_doc = documents.save_and_approve(other_out);
      check(other_out_doc.doc_no.rfind("QTCK", 0) == 0, "其他出库单号应以 QTCK 开头");
      other_balance = balance_of(other.id);
      check(other_balance.qty == q(3) && other_balance.amount_cents == 750,
            "其他出库应按移动加权平均结转成本，忽略录入单价");
      check(stock.ledger(other.id).size() == 2, "其他出入库应各写一条库存流水");

      std::int64_t other_entries = 0;
      db.read([&](sqlite3* conn) {
        xiaotu::Stmt stmt(conn,
                          "SELECT COUNT(*) FROM ar_ap_entry WHERE doc_id IN (?, ?);");
        stmt.bind(1, other_in_doc.id).bind(2, other_out_doc.id);
        if (stmt.step()) other_entries = stmt.i64(0);
      });
      check(other_entries == 0, "其他出入库不应产生应收应付");

      bool other_out_rejected = false;
      try {
        xiaotu::DocInput overdraw;
        overdraw.doc_type = "OTH_OUT";
        overdraw.warehouse_id = warehouse_list.at(0).id;
        overdraw.doc_date = "2026-11-12";
        overdraw.lines.push_back(
            xiaotu::DocLineInput{.item_id = other.id, .qty = q(4), .price_cents = 0});
        documents.save_and_approve(overdraw);
      } catch (const xiaotu::AppError& error) {
        other_out_rejected = error.code() == "STOCK_INSUFFICIENT";
      }
      check(other_out_rejected, "其他出库库存不足应整单拒绝");
      check(balance_of(other.id).qty == q(3), "被拒绝的其他出库不应改变库存");

      // 并发验收：8 张草稿同时审核、库存只够 3 张，必须恰好成功 3 张且不超卖。
      auto concurrent =
          items.create(xiaotu::gen::ItemRow{.code = "CC1", .name = "并发商品"});
      make_doc("PUR_IN", concurrent.id, q(10), 100);

      std::vector<std::int64_t> draft_ids;
      for (int index = 0; index < 8; ++index) {
        xiaotu::DocInput draft;
        draft.doc_type = "SAL_OUT";
        draft.warehouse_id = warehouse_list.at(0).id;
        draft.doc_date = "2026-11-12";
        draft.lines.push_back(
            xiaotu::DocLineInput{.item_id = concurrent.id, .qty = q(3), .price_cents = 500});
        draft_ids.push_back(documents.save_draft(draft).id);
      }

      std::latch start(1);
      std::atomic<int> approved{0};
      std::atomic<int> insufficient_count{0};
      std::atomic<int> unexpected{0};
      std::vector<std::thread> threads;
      threads.reserve(draft_ids.size());
      for (const std::int64_t doc_id : draft_ids) {
        threads.emplace_back([&, doc_id]() {
          start.wait();
          try {
            documents.approve(doc_id);
            ++approved;
          } catch (const xiaotu::AppError& error) {
            if (error.code() == "STOCK_INSUFFICIENT") {
              ++insufficient_count;
            } else {
              ++unexpected;
            }
          }
        });
      }
      start.count_down();
      for (auto& thread : threads) {
        thread.join();
      }

      check(approved.load() == 3, "并发审核应恰好成功 3 张（库存 10 / 每张 3）");
      check(insufficient_count.load() == 5, "其余 5 张应因库存不足被拒绝");
      check(unexpected.load() == 0, "并发审核不应出现其他错误");
      const auto concurrent_balance = balance_of(concurrent.id);
      check(concurrent_balance.qty == q(1), "并发审核后库存应为 1，不能为负");
      check(concurrent_balance.amount_cents == 100, "并发审核后结存金额应为 100 分");
      check(stock.ledger(concurrent.id).size() == 4,
            "并发审核后应只有 1 条入库与 3 条出库流水");

      int approved_docs = 0;
      int draft_docs = 0;
      std::int64_t draft_entries = 0;
      for (const std::int64_t doc_id : draft_ids) {
        const auto detail = documents.detail(doc_id);
        if (detail.status == "APPROVED") {
          ++approved_docs;
        } else if (detail.status == "DRAFT") {
          ++draft_docs;
          db.read([&](sqlite3* conn) {
            xiaotu::Stmt stmt(conn, "SELECT COUNT(*) FROM ar_ap_entry WHERE doc_id = ?;");
            stmt.bind(1, doc_id);
            if (stmt.step()) draft_entries += stmt.i64(0);
          });
        }
      }
      check(approved_docs == 3 && draft_docs == 5,
            "并发后应保留 3 张已审核、5 张未动的草稿");
      check(draft_entries == 0, "被拒绝的草稿不应留下应收记录");

      // 并发验收：同一张已审核出库单被 8 个线程同时反审核，只能回滚一次。
      auto reverse_item =
          items.create(xiaotu::gen::ItemRow{.code = "CC2", .name = "并发反审核商品"});
      make_doc("PUR_IN", reverse_item.id, q(10), 100);
      xiaotu::DocInput reverse_sale;
      reverse_sale.doc_type = "SAL_OUT";
      reverse_sale.warehouse_id = warehouse_list.at(0).id;
      reverse_sale.doc_date = "2026-11-12";
      reverse_sale.lines.push_back(
          xiaotu::DocLineInput{.item_id = reverse_item.id, .qty = q(4), .price_cents = 500});
      const auto reverse_doc = documents.save_and_approve(reverse_sale);
      check(balance_of(reverse_item.id).qty == q(6), "反审核前库存应为 6");

      std::latch reverse_start(1);
      std::atomic<int> reversed{0};
      std::atomic<int> reverse_rejected{0};
      std::atomic<int> reverse_unexpected{0};
      std::vector<std::thread> reverse_threads;
      reverse_threads.reserve(8);
      for (int index = 0; index < 8; ++index) {
        reverse_threads.emplace_back([&]() {
          reverse_start.wait();
          try {
            documents.reverse(reverse_doc.id);
            ++reversed;
          } catch (const xiaotu::AppError& error) {
            if (error.code() == "INVALID_STATUS") {
              ++reverse_rejected;
            } else {
              ++reverse_unexpected;
            }
          }
        });
      }
      reverse_start.count_down();
      for (auto& thread : reverse_threads) {
        thread.join();
      }

      check(reversed.load() == 1, "并发反审核应只有一次成功");
      check(reverse_rejected.load() == 7, "其余 7 次应因单据已作废被拒绝");
      check(reverse_unexpected.load() == 0, "并发反审核不应出现其他错误");
      const auto reverse_balance = balance_of(reverse_item.id);
      check(reverse_balance.qty == q(10) && reverse_balance.amount_cents == 1000,
            "并发反审核后库存应只回滚一次");
      check(stock.ledger(reverse_item.id).size() == 3,
            "并发反审核后应只有 1 条入库、1 条出库与 1 条 REVERSE");

      std::int64_t reverse_entries = 0;
      std::string reverse_entry_status;
      db.read([&](sqlite3* conn) {
        xiaotu::Stmt stmt(conn,
                          "SELECT COUNT(*), COALESCE(MAX(status), '') FROM ar_ap_entry"
                          " WHERE doc_id = ?;");
        stmt.bind(1, reverse_doc.id);
        if (stmt.step()) {
          reverse_entries = stmt.i64(0);
          reverse_entry_status = stmt.text(1);
        }
      });
      check(reverse_entries == 1 && reverse_entry_status == "VOIDED",
            "并发反审核后应收应只有一条且为 VOIDED");

      // 成本重算只读核对：健康数据应无差异，篡改账面金额后应报出差异。
      xiaotu::CostRecalcService cost_recalc(db);
      const auto healthy = cost_recalc.recalculate();
      check(healthy.ledger_rows > 0, "重算应读取到库存流水");
      check(healthy.qty_difference_count == 0, "健康数据重算数量应一致");
      check(healthy.difference_count == 0, "健康数据重算金额应无差异");

      db.write([&](sqlite3* conn) {
        xiaotu::Stmt stmt(conn,
                  "UPDATE stock_balance SET amount_cents = amount_cents + 12345"
                  " WHERE item_id = ?;");
        stmt.bind(1, ordered.id).run();
      });
      const auto tampered = cost_recalc.recalculate(ordered.id);
      check(tampered.difference_count >= 1, "篡改账面金额后应报出差异");
      check(tampered.differences.at(0).amount_diff_cents == -12345,
            "差异应为重算金额减账面金额");

      // 运行日志：级别过滤、落盘与按大小滚动。
      const auto log_dir = std::filesystem::temp_directory_path() / "xiaotu-log-smoke";
      std::filesystem::remove_all(log_dir);
      xiaotu::init_logging(xiaotu::LogOptions{.directory = log_dir,
                                              .level = xiaotu::LogLevel::Info,
                                              .max_bytes = 2048,
                                              .keep_files = 2});
      xiaotu::log_debug("debug-should-be-filtered");
      xiaotu::log_info("info-visible");
      xiaotu::log_warn("warn-visible");
      xiaotu::shutdown_logging();
      {
        std::ifstream in(log_dir / "app.log", std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>(in),
                               std::istreambuf_iterator<char>()};
        check(text.find("[INFO]") != std::string::npos &&
                  text.find("info-visible") != std::string::npos,
              "INFO 日志应落盘");
        check(text.find("warn-visible") != std::string::npos, "WARN 日志应落盘");
        check(text.find("debug-should-be-filtered") == std::string::npos,
              "默认级别应过滤 DEBUG");
      }
      xiaotu::init_logging(xiaotu::LogOptions{.directory = log_dir,
                                              .level = xiaotu::LogLevel::Debug,
                                              .max_bytes = 1024,
                                              .keep_files = 2});
      for (int index = 0; index < 200; ++index) {
        xiaotu::log_info("rolling-line-" + std::to_string(index) +
                         " ................................................");
      }
      xiaotu::shutdown_logging();
      check(std::filesystem::exists(log_dir / "app.log.1"), "日志超限应滚动出 app.log.1");
      std::filesystem::remove_all(log_dir);

      // 入库被后续出库消耗后，反审核必须拒绝，避免结存数量为负。
      // 放在全部报表/列表断言之后，避免新增单据影响前面的统计口径。
      auto claimed =
          items.create(xiaotu::gen::ItemRow{.code = "P002", .name = "已消耗商品"});
      const auto claimed_in = make_doc("PUR_IN", claimed.id, q(10), 300);
      make_doc("SAL_OUT", claimed.id, q(8), 500);
      bool reverse_blocked = false;
      std::string reverse_message;
      try {
        documents.reverse(claimed_in.id);
      } catch (const xiaotu::AppError& error) {
        reverse_blocked = error.code() == "REVERSE_WOULD_NEGATIVE";
        reverse_message = error.what();
      }
      check(reverse_blocked, "入库已被后续出库消耗时反审核应拒绝");
      check(reverse_message.find("结存数量") != std::string::npos,
            "反审核拒绝原因应说明结存数量会为负");
      check(balance_of(claimed.id).qty == q(2), "反审核被拒后库存不应变化");
      check(documents.detail(claimed_in.id).status == "APPROVED",
            "反审核被拒后单据应保持已审核");

      // 备份与恢复：快照可校验，恢复后 schema 一致，覆盖前留安全备份。
      const auto backup_root =
          std::filesystem::temp_directory_path() / "xiaotu-backup-smoke";
      std::filesystem::remove_all(backup_root);
      const auto snapshot = xiaotu::backup_database(db, backup_root / "backups", "smoke");
      check(std::filesystem::exists(snapshot.path) && snapshot.bytes > 0,
            "备份应生成非空快照");
      xiaotu::verify_database(snapshot.path);

      const auto restored_path = backup_root / "restored.db";
      xiaotu::restore_database(snapshot.path, restored_path);
      {
        xiaotu::Db restored(restored_path);
        check(restored.schema_version() == db.schema_version(),
              "恢复后 schema 版本应与备份一致");
      }
      xiaotu::restore_database(snapshot.path, restored_path);
      bool has_safety = false;
      for (const auto& entry :
           std::filesystem::directory_iterator(backup_root / "backups")) {
        if (entry.path().filename().string().rfind("pre-restore-", 0) == 0) {
          has_safety = true;
        }
      }
      check(has_safety, "覆盖恢复前应留下 pre-restore 安全备份");
      std::filesystem::remove_all(backup_root);


    }
  } catch (const std::exception& error) {
    std::cerr << "异常: " << error.what() << std::endl;
    ++g_failures;
  }

  fs::remove_all(dir);
  if (g_failures == 0) {
    std::cout << "core_smoke: OK" << std::endl;
  }
  return g_failures == 0 ? 0 : 1;
}
