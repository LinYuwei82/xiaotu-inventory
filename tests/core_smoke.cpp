// M0 冒烟测试：迁移 → 建商品 → 销售出库 → 采购入库与加权平均 → 反审核 → 查库存。
#include <unistd.h>

#include <filesystem>
#include <iostream>
#include <string>

#include "xiaotu/audit.h"
#include "xiaotu/db.h"
#include "xiaotu/document.h"
#include "xiaotu/edition.h"
#include "xiaotu/error.h"
#include "xiaotu/finance_service.h"
#include "xiaotu/gen/customer_service.h"
#include "xiaotu/gen/item_service.h"
#include "xiaotu/gen/supplier_service.h"
#include "xiaotu/gen/unit_service.h"
#include "xiaotu/gen/warehouse_service.h"
#include "xiaotu/migrations.h"
#include "xiaotu/permissions.h"
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

}  // namespace

int main() {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / ("xiaotu-smoke-" + std::to_string(::getpid()));

  try {
    fs::remove_all(dir);
    {
      xiaotu::Db db(dir / "app.db", 2);
      xiaotu::migrate(db);
      check(db.schema_version() == 7, "迁移后 schema 版本应为 7");

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

      xiaotu::UserService users(db);
      const auto seeded_users = users.list();
      check(seeded_users.size() == 1 && seeded_users.at(0).username == "admin",
            "迁移应内置 admin 用户");
      check(seeded_users.at(0).role == "admin", "内置用户角色应为 admin");
      check(users.count_active() == 1, "内置用户应计入活跃用户");

      const auto operator_user = users.create(
          xiaotu::UserInput{.username = "op1", .display_name = "操作员一", .role = "operator"});
      check(operator_user.id > 0 && operator_user.role == "operator", "应能创建操作员");

      bool duplicate_user_rejected = false;
      try {
        users.create(xiaotu::UserInput{.username = "op1", .display_name = "重复", .role = "viewer"});
      } catch (const xiaotu::AppError& error) {
        duplicate_user_rejected = error.code() == "DUPLICATE_CODE";
      }
      check(duplicate_user_rejected, "重复用户名应被拒绝");

      bool bad_role_rejected = false;
      try {
        users.create(xiaotu::UserInput{.username = "ghost1", .role = "ghost"});
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
          xiaotu::DocLineInput{.item_id = item.id, .qty = 5, .price_cents = 1000});
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
          xiaotu::DocLineInput{.item_id = item.id, .qty = 5, .price_cents = 1000});
      const auto opening_doc = documents.save_and_approve(opening);

      xiaotu::DocInput input;
      input.doc_type = "SAL_OUT";
      input.warehouse_id = warehouse_list.at(0).id;
      input.doc_date = "2026-11-12";
      input.partner = "测试客户";
      input.lines.push_back(xiaotu::DocLineInput{.item_id = item.id, .qty = 5, .price_cents = 1000});

      const auto created = documents.save_and_approve(input);
      check(!created.doc_no.empty(), "应生成单据号");

      const auto balances = stock.balances();
      check(balances.size() == 1, "应有一条库存余额");
      check(balances.at(0).qty == 0, "出库 5 件后余额应为 0");

      const auto ledger = stock.ledger(item.id);
      check(ledger.size() == 2, "应有一条入库流水和一条出库流水");
      check(ledger.at(0).qty == -5, "出库流水数量应为 -5");

      const auto listed = documents.list("SAL_OUT");
      check(listed.size() == 1 && listed.at(0).status == "APPROVED", "单据应为已审核状态");

      documents.reverse(created.id);
      const auto after_reverse = stock.balances();
      check(after_reverse.at(0).qty == 5, "反审核后库存余额应回到 5");
      check(stock.ledger(item.id).size() == 3, "反审核应追加一条反向流水，而不是删除原流水");

      bool double_reverse_rejected = false;
      try {
        documents.reverse(created.id);
      } catch (const xiaotu::AppError& error) {
        double_reverse_rejected = error.code() == "INVALID_STATUS";
      }
      check(double_reverse_rejected, "重复反审核应被拒绝");

      documents.reverse(opening_doc.id);
      check(stock.balances().at(0).qty == 0, "反审核期初入库后余额应回到 0");

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

      const auto first_purchase = make_doc("PUR_IN", purchased.id, 10, 1000);
      check(first_purchase.doc_no.rfind("CGRK", 0) == 0, "采购入库单号应以 CGRK 开头");
      auto purchased_balance = balance_of(purchased.id);
      check(purchased_balance.qty == 10, "采购入库后数量应为 10");
      check(purchased_balance.amount_cents == 10000, "采购入库后金额应为 10000 分");

      make_doc("PUR_IN", purchased.id, 10, 1400);
      purchased_balance = balance_of(purchased.id);
      check(purchased_balance.qty == 20, "两笔入库后数量应为 20");
      check(purchased_balance.amount_cents == 24000, "两笔入库后金额应为 24000 分");

      // 出库 5 件，成本 = 24000 * 5 / 20 = 6000 分，结存应为 15 件 / 18000 分。
      xiaotu::DocInput sale;
      sale.doc_type = "SAL_OUT";
      sale.warehouse_id = warehouse_list.at(0).id;
      sale.doc_date = "2026-11-12";
      sale.lines.push_back(xiaotu::DocLineInput{.item_id = purchased.id, .qty = 5,
                                                .price_cents = 2000});
      const auto sale_doc = documents.save_and_approve(sale);
      purchased_balance = balance_of(purchased.id);
      check(purchased_balance.qty == 15, "加权平均出库后数量应为 15");
      check(purchased_balance.amount_cents == 18000, "加权平均出库后金额应为 18000 分");

      documents.reverse(sale_doc.id);
      purchased_balance = balance_of(purchased.id);
      check(purchased_balance.qty == 20 && purchased_balance.amount_cents == 24000,
            "反审核出库后应回到 20 件 / 24000 分");

      documents.reverse(first_purchase.id);
      purchased_balance = balance_of(purchased.id);
      check(purchased_balance.qty == 10, "反审核首笔入库后数量应为 10");
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
          xiaotu::DocLineInput{.item_id = purchased.id, .qty = 4, .price_cents = 0});
      const auto transfer_doc = documents.save_and_approve(transfer);
      check(transfer_doc.doc_no.rfind("DBD", 0) == 0, "调拨单号应以 DBD 开头");
      auto source_balance = balance_in("主仓库", purchased.id);
      auto target_balance = balance_in("分仓", purchased.id);
      check(source_balance.qty == 6 && source_balance.amount_cents == 8400,
            "调拨后源仓应为 6 件 / 8400 分");
      check(target_balance.qty == 4 && target_balance.amount_cents == 5600,
            "调拨后目标仓应为 4 件 / 5600 分");

      bool over_transfer_rejected = false;
      try {
        xiaotu::DocInput too_much = transfer;
        too_much.lines.clear();
        too_much.lines.push_back(
            xiaotu::DocLineInput{.item_id = purchased.id, .qty = 999, .price_cents = 0});
        documents.save_and_approve(too_much);
      } catch (const xiaotu::AppError& error) {
        over_transfer_rejected = error.code() == "STOCK_INSUFFICIENT";
      }
      check(over_transfer_rejected, "调拨数量超过源仓结存应被拒绝");

      documents.reverse(transfer_doc.id);
      source_balance = balance_in("主仓库", purchased.id);
      target_balance = balance_in("分仓", purchased.id);
      check(source_balance.qty == 10 && source_balance.amount_cents == 14000,
            "反审核调拨后源仓应回到 10 件 / 14000 分");
      check(target_balance.qty == 0 && target_balance.amount_cents == 0,
            "反审核调拨后目标仓应清零");

      // 盘点：明细为实盘数，差额生成调整流水。
      xiaotu::DocInput stocktake;
      stocktake.doc_type = "STK_CNT";
      stocktake.warehouse_id = warehouse_list.at(0).id;
      stocktake.doc_date = "2026-11-12";
      stocktake.lines.push_back(
          xiaotu::DocLineInput{.item_id = purchased.id, .qty = 7, .price_cents = 0});
      const auto stocktake_doc = documents.save_and_approve(stocktake);
      check(stocktake_doc.doc_no.rfind("PDD", 0) == 0, "盘点单号应以 PDD 开头");
      auto counted = balance_of(purchased.id);
      check(counted.qty == 7 && counted.amount_cents == 9800,
            "盘亏 3 件后应为 7 件 / 9800 分");

      documents.reverse(stocktake_doc.id);
      counted = balance_of(purchased.id);
      check(counted.qty == 10 && counted.amount_cents == 14000, "反审核盘点后应回到 10 件 / 14000 分");

      xiaotu::DocInput surplus;
      surplus.doc_type = "STK_CNT";
      surplus.warehouse_id = warehouse_list.at(0).id;
      surplus.doc_date = "2026-11-12";
      surplus.lines.push_back(
          xiaotu::DocLineInput{.item_id = purchased.id, .qty = 12, .price_cents = 0});
      const auto surplus_doc = documents.save_and_approve(surplus);
      counted = balance_of(purchased.id);
      check(counted.qty == 12 && counted.amount_cents == 16800,
            "盘盈 2 件后应为 12 件 / 16800 分");
      documents.reverse(surplus_doc.id);
      check(balance_of(purchased.id).qty == 10, "反审核盘盈后应回到 10 件");

      bool negative_stocktake_rejected = false;
      try {
        xiaotu::DocInput negative = surplus;
        negative.lines.clear();
        negative.lines.push_back(
            xiaotu::DocLineInput{.item_id = purchased.id, .qty = -1, .price_cents = 0});
        documents.save_and_approve(negative);
      } catch (const xiaotu::AppError& error) {
        negative_stocktake_rejected = error.code() == "INVALID_INPUT";
      }
      check(negative_stocktake_rejected, "实盘数量为负数应被拒绝");

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
          xiaotu::DocLineInput{.item_id = supplier_item.id, .qty = 2, .price_cents = 500});
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
            xiaotu::DocLineInput{.item_id = supplier_item.id, .qty = 1, .price_cents = 100});
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
            xiaotu::DocLineInput{.item_id = sales_item.id, .qty = 10, .price_cents = 1000});
        documents.save_and_approve(opening);
      }

      xiaotu::DocInput customer_sale;
      customer_sale.doc_type = "SAL_OUT";
      customer_sale.warehouse_id = warehouse_list.at(0).id;
      customer_sale.doc_date = "2026-11-12";
      customer_sale.partner_id = customer.id;
      customer_sale.partner = "手工名称应被档案覆盖";
      customer_sale.lines.push_back(
          xiaotu::DocLineInput{.item_id = sales_item.id, .qty = 2, .price_cents = 2000});
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
          xiaotu::DocLineInput{.item_id = sales_item.id, .qty = 1, .price_cents = 2000});
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
            xiaotu::DocLineInput{.item_id = sales_item.id, .qty = 3, .price_cents = 1000});
        return documents.save_and_approve(doc);
      }();
      xiaotu::DocInput sale_b_input;
      sale_b_input.doc_type = "SAL_OUT";
      sale_b_input.warehouse_id = warehouse_list.at(0).id;
      sale_b_input.doc_date = "2026-11-12";
      sale_b_input.partner_id = customer.id;
      sale_b_input.lines.push_back(
          xiaotu::DocLineInput{.item_id = sales_item.id, .qty = 2, .price_cents = 1000});
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

      const auto first_settlement = settlements.create(
          1, customer_key, 1000, {{entry_a.id, 1000}}, "2026-11-12", "部分收款");
      check(first_settlement.settle_no.rfind("HX20261112-", 0) == 0, "核销单号应按日期生成");
      auto after_partial = entry_of(sale_a.id);
      check(after_partial.settled_cents == 1000 && after_partial.outstanding_cents == 2000 &&
                after_partial.status == "PARTIAL",
            "部分核销后应剩 2000 分未核销");

      bool over_settle_rejected = false;
      try {
        settlements.create(1, customer_key, 3000, {{entry_a.id, 3000}}, "2026-11-12", "");
      } catch (const xiaotu::AppError& error) {
        over_settle_rejected = error.code() == "SETTLE_OVER";
      }
      check(over_settle_rejected, "超额核销应被拒绝");

      bool duplicate_allocation_rejected = false;
      try {
        settlements.create(1, customer_key, 200,
                           {{entry_a.id, 100}, {entry_a.id, 100}}, "2026-11-12", "");
      } catch (const xiaotu::AppError& error) {
        duplicate_allocation_rejected = error.code() == "INVALID_INPUT";
      }
      check(duplicate_allocation_rejected, "同一单据重复分摊应被拒绝");

      bool cross_partner_rejected = false;
      try {
        settlements.create(1, "C:999999", 100, {{entry_a.id, 100}}, "2026-11-12", "");
      } catch (const xiaotu::AppError& error) {
        cross_partner_rejected = error.code() == "INVALID_INPUT";
      }
      check(cross_partner_rejected, "跨往来单位核销应被拒绝");

      const auto merged = settlements.create(1, customer_key, 3500,
                                             {{entry_a.id, 2000}, {entry_b.id, 1500}},
                                             "2026-11-12", "合并收款");
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
      check(flows.front().opening_qty == 0, "11-01 起期初应为 0");

      const auto inv_summary = reports.inventory_summary(report_filter);
      check(inv_summary.size() == 1 && inv_summary.front().item_id == sales_item.id,
            "进销存汇总应按商品返回一行");
      check(inv_summary.front().opening_qty == 0 &&
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
      check(profits.front().qty == 4 && profits.front().revenue_cents == 3000 &&
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
      make_doc("PUR_IN", returned.id, 10, 1000);
      const auto purchase_return = make_doc("PUR_RET", returned.id, 4, 1000);
      check(purchase_return.doc_no.rfind("CGTH", 0) == 0, "采购退货单号应以 CGTH 开头");
      auto returned_balance = balance_of(returned.id);
      check(returned_balance.qty == 6 && returned_balance.amount_cents == 6000,
            "采购退货 4 件后应为 6 件 / 6000 分");

      const auto partial_return = make_doc("PUR_RET", returned.id, 3, 1000);
      returned_balance = balance_of(returned.id);
      check(returned_balance.qty == 3 && returned_balance.amount_cents == 3000,
            "部分退货再退 3 件后应为 3 件 / 3000 分");
      documents.reverse(partial_return.id);
      returned_balance = balance_of(returned.id);
      check(returned_balance.qty == 6 && returned_balance.amount_cents == 6000,
            "反审核部分退货后应回到 6 件 / 6000 分");
      check(stock.ledger(returned.id).size() == 4,
            "采购退货链路应为入库 + 两次退货 + REVERSE 共 4 条流水");

      // 销售退货：先出库再退回，按成本单价入库。
      auto sales_returned = items.create(xiaotu::gen::ItemRow{.code = "S001", .name = "销退商品"});
      make_doc("PUR_IN", sales_returned.id, 10, 1000);
      make_doc("SAL_OUT", sales_returned.id, 4, 1500);
      const auto sales_return = make_doc("SAL_RET", sales_returned.id, 2, 1000);
      check(sales_return.doc_no.rfind("XSTH", 0) == 0, "销售退货单号应以 XSTH 开头");
      auto sales_returned_balance = balance_of(sales_returned.id);
      check(sales_returned_balance.qty == 8 && sales_returned_balance.amount_cents == 8000,
            "销售退货 2 件后应为 8 件 / 8000 分");
      documents.reverse(sales_return.id);
      sales_returned_balance = balance_of(sales_returned.id);
      check(sales_returned_balance.qty == 6 && sales_returned_balance.amount_cents == 6000,
            "反审核销售退货后应回到 6 件 / 6000 分");

      // 草稿态：保存不写库存，审核时才在单事务内写流水。
      auto draft_item = items.create(xiaotu::gen::ItemRow{.code = "D001", .name = "草稿商品"});
      xiaotu::DocInput draft_input;
      draft_input.doc_type = "PUR_IN";
      draft_input.warehouse_id = warehouse_list.at(0).id;
      draft_input.doc_date = "2026-11-12";
      draft_input.partner = "草稿供应商";
      draft_input.lines.push_back(
          xiaotu::DocLineInput{.item_id = draft_item.id, .qty = 5, .price_cents = 900});

      const auto draft = documents.save_draft(draft_input);
      check(draft.doc_no.rfind("CGRK", 0) == 0, "草稿也应取到正式单据号");
      check(stock.ledger(draft_item.id).empty(), "草稿不应写库存流水");
      auto draft_detail = documents.detail(draft.id);
      check(draft_detail.status == "DRAFT", "草稿状态应为 DRAFT");
      check(draft_detail.lines.size() == 1 && draft_detail.lines.at(0).qty == 5,
            "草稿明细应保存");

      draft_input.lines.at(0).qty = 7;
      draft_input.lines.at(0).price_cents = 1100;
      documents.update_draft(draft.id, draft_input);
      draft_detail = documents.detail(draft.id);
      check(draft_detail.lines.at(0).qty == 7 && draft_detail.lines.at(0).price_cents == 1100,
            "草稿明细应可编辑");
      check(draft_detail.total_qty == 7 && draft_detail.total_amount_cents == 7700,
            "草稿汇总金额应随明细更新");
      check(stock.ledger(draft_item.id).empty(), "编辑草稿仍不应写库存流水");

      documents.approve(draft.id);
      draft_detail = documents.detail(draft.id);
      check(draft_detail.status == "APPROVED", "审核后状态应为 APPROVED");
      auto draft_balance = balance_of(draft_item.id);
      check(draft_balance.qty == 7 && draft_balance.amount_cents == 7700,
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

      items.remove(draft_item.id);
      items.remove(sales_returned.id);
      items.remove(returned.id);
      items.remove(purchased.id);
      items.remove(item.id);
      items.remove(supplier_item.id);
      items.remove(bottled.id);
      items.remove(sales_item.id);
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
