// M0 冒烟测试：迁移 → 建商品 → 销售出库 → 采购入库与加权平均 → 反审核 → 查库存。
#include <unistd.h>

#include <filesystem>
#include <iostream>
#include <string>

#include "xiaotu/db.h"
#include "xiaotu/document.h"
#include "xiaotu/edition.h"
#include "xiaotu/error.h"
#include "xiaotu/gen/item_service.h"
#include "xiaotu/gen/warehouse_service.h"
#include "xiaotu/migrations.h"
#include "xiaotu/permissions.h"
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
      check(db.schema_version() == 2, "迁移后 schema 版本应为 2");

      xiaotu::gen::ItemService items(db);
      xiaotu::gen::WarehouseService warehouses(db);
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
      check(balances.at(0).qty == -5, "无期初库存时出库后余额应为 -5（M0 允许负库存）");

      const auto ledger = stock.ledger(item.id);
      check(ledger.size() == 1, "应有一条库存流水");
      check(ledger.at(0).qty == -5, "流水数量应为 -5");

      const auto listed = documents.list("SAL_OUT");
      check(listed.size() == 1 && listed.at(0).status == "APPROVED", "单据应为已审核状态");

      documents.reverse(created.id);
      const auto after_reverse = stock.balances();
      check(after_reverse.at(0).qty == 0, "反审核后库存余额应回到 0");
      check(stock.ledger(item.id).size() == 2, "反审核应追加一条反向流水，而不是删除原流水");

      bool double_reverse_rejected = false;
      try {
        documents.reverse(created.id);
      } catch (const xiaotu::AppError& error) {
        double_reverse_rejected = error.code() == "INVALID_STATUS";
      }
      check(double_reverse_rejected, "重复反审核应被拒绝");

      // 采购入库：按单据金额入账，出库成本走移动加权平均。
      auto purchased = items.create(xiaotu::gen::ItemRow{.code = "P001", .name = "采购商品"});
      const auto balance_of = [&stock](std::int64_t item_id) {
        for (const auto& row : stock.balances()) {
          if (row.item_id == item_id) return row;
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
      check(purchase_docs.size() == 2, "应有两张采购入库单");
      check(purchase_docs.at(0).status == "APPROVED" && purchase_docs.at(1).status == "VOIDED",
            "反审核的入库单应标记为 VOIDED");

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
      check(items.list().empty(), "软删除后列表中不应再出现该商品");
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
