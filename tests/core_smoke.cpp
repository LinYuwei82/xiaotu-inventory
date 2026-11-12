// M0 冒烟测试：迁移 → 建商品 → 销售出库 → 采购入库与加权平均 → 反审核 → 查库存。
#include <unistd.h>

#include <filesystem>
#include <iostream>
#include <string>

#include "xiaotu/db.h"
#include "xiaotu/document.h"
#include "xiaotu/error.h"
#include "xiaotu/gen/item_service.h"
#include "xiaotu/gen/warehouse_service.h"
#include "xiaotu/migrations.h"
#include "xiaotu/stock_service.h"

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
      check(db.schema_version() == 1, "迁移后 schema 版本应为 1");

      xiaotu::gen::ItemService items(db);
      xiaotu::gen::WarehouseService warehouses(db);
      xiaotu::DocumentEngine documents(db);
      xiaotu::StockService stock(db);

      const auto warehouse_list = warehouses.list();
      check(warehouse_list.size() == 1, "应内置一个默认仓库");
      check(warehouse_list.at(0).code == "MAIN", "默认仓库编码应为 MAIN");

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
      const auto purchase = [&](std::int64_t qty, std::int64_t price_cents) {
        xiaotu::DocInput doc;
        doc.doc_type = "PUR_IN";
        doc.warehouse_id = warehouse_list.at(0).id;
        doc.doc_date = "2026-11-12";
        doc.partner = "测试供应商";
        doc.lines.push_back(xiaotu::DocLineInput{.item_id = purchased.id, .qty = qty,
                                                 .price_cents = price_cents});
        return documents.save_and_approve(doc);
      };

      const auto first_purchase = purchase(10, 1000);
      check(first_purchase.doc_no.rfind("CGRK", 0) == 0, "采购入库单号应以 CGRK 开头");
      auto purchased_balance = balance_of(purchased.id);
      check(purchased_balance.qty == 10, "采购入库后数量应为 10");
      check(purchased_balance.amount_cents == 10000, "采购入库后金额应为 10000 分");

      purchase(10, 1400);
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
