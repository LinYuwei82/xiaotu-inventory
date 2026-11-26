// M0 冒烟测试：迁移 → 建商品 → 销售出库 → 采购入库与加权平均 → 反审核 → 查库存。
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <latch>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include "xiaotu/audit.h"
#include "xiaotu/backup.h"
#include "xiaotu/customer_service.h"
#include "xiaotu/document.h"
#include "xiaotu/edition.h"
#include "xiaotu/error.h"
#include "xiaotu/finance_service.h"
#include "xiaotu/gen/customer_service.h"
#include "xiaotu/gen/item_service.h"
#include "xiaotu/log.h"
#include "xiaotu/migrations.h"
#include "xiaotu/partner_merge.h"
#include "xiaotu/partner_preview.h"
#include "xiaotu/partner_resolve.h"
#include "xiaotu/password.h"
#include "xiaotu/setting_service.h"
#include "xiaotu/gen/supplier_service.h"
#include "xiaotu/gen/unit_service.h"
#include "xiaotu/ids.h"
#include "xiaotu/gen/customer_service.h"
#include "xiaotu/gen/employee_service.h"
#include "xiaotu/gen/warehouse_service.h"
#include "xiaotu/item_category.h"
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

// 测试日期跟随当天，避免跨零点后固定日期与 now_text() 产生跨期偏差。
const std::string kToday = xiaotu::today_text();
const std::string kMonthStart = kToday.substr(0, 8) + "01";
const std::string kFarFuture = "2099-12-31";
const std::string kTodayYmd = kToday.substr(0, 4) + kToday.substr(5, 2) + kToday.substr(8, 2);

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
      check(db.schema_version() == 40, "迁移后 schema 版本应为 40");

      // 安全令牌随机源回归：输出为足长十六进制串、连续生成不重复。
      // 弱随机（伪随机数可预测种子）的问题由实现层保证，这里只守可观测语义。
      {
        const auto token = xiaotu::random_hex(32);
        check(token.size() == 64, "random_hex(32) 应输出 64 个十六进制字符");
        bool all_hex = true;
        for (const char ch : token) {
          if (std::isxdigit(static_cast<unsigned char>(ch)) == 0) {
            all_hex = false;
          }
        }
        check(all_hex, "random_hex 输出应全部是十六进制字符");
        std::set<std::string> seen;
        for (int i = 0; i < 256; ++i) {
          seen.insert(xiaotu::random_hex(32));
        }
        check(seen.size() == 256, "连续生成的 256 个令牌应互不重复");
      }

      // 读连接被占满时不能无限等待：超时后应报 DB_BUSY，让调用方回 503。
      {
        xiaotu::Db pool_db(dir / "pool.db", 1, std::chrono::milliseconds(80));
        std::latch started(1);
        std::thread holder([&] {
          pool_db.read([&](sqlite3*) {
            started.count_down();
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
          });
        });
        started.wait();
        bool busy = false;
        try {
          pool_db.read([](sqlite3*) {});
        } catch (const xiaotu::AppError& error) {
          busy = error.code() == "DB_BUSY";
        }
        holder.join();
        check(busy, "读连接占满时应在超时后报 DB_BUSY");

        // 写锁同理：占住写连接时，其他写入不能无限排队。
        std::latch writing(1);
        std::thread writer([&] {
          pool_db.write([&](sqlite3*) {
            writing.count_down();
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
          });
        });
        writing.wait();
        bool write_busy = false;
        try {
          pool_db.write([](sqlite3*) {});
        } catch (const xiaotu::AppError& error) {
          write_busy = error.code() == "DB_BUSY";
        }
        writer.join();
        check(write_busy, "写连接占用时应在超时后报 DB_BUSY");
      }

      // v28 无数据情形：空库只建表，不产生任何 partner / 映射行。
      db.read([&](sqlite3* conn) {
        xiaotu::Stmt partner_count(conn, "SELECT COUNT(*) FROM partner;");
        check(partner_count.step() && partner_count.i64(0) == 0,
              "空库迁移后 partner 应为 0 行");
        xiaotu::Stmt map_count(conn, "SELECT COUNT(*) FROM partner_legacy_map;");
        check(map_count.step() && map_count.i64(0) == 0,
              "空库迁移后 partner_legacy_map 应为 0 行");
      });

      // 迁移链：空库直达最新；v1 起步再逐版升到最新，版本必须连续、不跳号。
      {
        const auto chain_dir = dir / "migration-chain";
        fs::create_directories(chain_dir);

        xiaotu::Db empty_db(chain_dir / "empty.db", 2);
        xiaotu::migrate(empty_db);
        check(empty_db.schema_version() == xiaotu::target_schema_version(),
              "空库迁移后应等于目标 schema 版本");
        empty_db.read([&](sqlite3* conn) {
          xiaotu::Stmt count(conn, "SELECT COUNT(*) FROM schema_migration;");
          xiaotu::Stmt max_version(conn, "SELECT MAX(version) FROM schema_migration;");
          check(count.step() && count.i64(0) == xiaotu::target_schema_version(),
                "空库迁移应逐版记录，不跳号");
          check(max_version.step() && max_version.i64(0) == xiaotu::target_schema_version(),
                "空库迁移的最后一版应等于目标版本");
        });

        xiaotu::Db chain_db(chain_dir / "chain.db", 2);
        const auto& migrations = xiaotu::all_migrations();
        check(!migrations.empty() && migrations.front().version == 1,
              "迁移清单应从 v1 开始");
        const auto checksum = [](std::string_view sql) {
          std::uint64_t hash = 1469598103934665603ULL;
          for (const unsigned char ch : sql) {
            hash ^= ch;
            hash *= 1099511628211ULL;
          }
          char buffer[17];
          std::snprintf(buffer, sizeof(buffer), "%016llx",
                        static_cast<unsigned long long>(hash));
          return std::string(buffer);
        };
        chain_db.write_raw([&](sqlite3* conn) {
          char* error = nullptr;
          sqlite3_exec(conn,
                       "CREATE TABLE schema_migration ("
                       "  version    INTEGER PRIMARY KEY,"
                       "  name       TEXT NOT NULL,"
                       "  checksum   TEXT NOT NULL,"
                       "  applied_at TEXT NOT NULL"
                       ");",
                       nullptr, nullptr, &error);
          check(error == nullptr, "应能创建 schema_migration");
          if (error) sqlite3_free(error);
          sqlite3_exec(conn, migrations.front().sql, nullptr, nullptr, &error);
          check(error == nullptr, "v1 应能单独执行");
          if (error) {
            sqlite3_free(error);
            return;
          }
          xiaotu::Stmt insert(conn,
                              "INSERT INTO schema_migration (version, name, checksum, applied_at)"
                              " VALUES (1, 'v1_initial_schema', ?, '2026-01-01 00:00:00');");
          insert.bind(1, checksum(migrations.front().sql));
          insert.step();
        });

        xiaotu::migrate(chain_db);
        check(chain_db.schema_version() == xiaotu::target_schema_version(),
              "v1 库应逐版升到目标 schema 版本");
        chain_db.read([&](sqlite3* conn) {
          xiaotu::Stmt stmt(conn, "SELECT version FROM schema_migration ORDER BY version;");
          std::vector<int> versions;
          while (stmt.step()) versions.push_back(static_cast<int>(stmt.i64(0)));
          bool contiguous =
              versions.size() == static_cast<std::size_t>(xiaotu::target_schema_version());
          for (std::size_t i = 0; i < versions.size() && contiguous; ++i) {
            contiguous = versions[i] == static_cast<int>(i) + 1;
          }
          check(contiguous, "v1 起步升级后版本应连续，不跳号、不重复");
        });
      }

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
      check(xiaotu::role_has_permission("operator", "setting.write"),
            "操作员应能维护系统参数");
      check(xiaotu::role_has_permission("viewer", "setting.read"),
            "只读角色应能查看系统参数");
      check(!xiaotu::role_has_permission("viewer", "setting.write"),
            "只读角色不应能修改系统参数");

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

      // 商品分类：三级路径、同级重名、按 path 取整棵子树、带商品时禁止删除。
      xiaotu::ItemCategoryService categories(db);
      const auto root = categories.create(0, "食品", 10, true);
      const auto child = categories.create(root.id, "饮料", 20, true);
      const auto grandchild = categories.create(child.id, "碳酸饮料", 30, true);
      check(root.path == "/" + std::to_string(root.id) + "/", "根分类 path 应以自身 id 结尾");
      check(grandchild.path == root.path + std::to_string(child.id) + "/" +
                                    std::to_string(grandchild.id) + "/",
            "子分类 path 应拼接父路径");
      check(grandchild.depth == 3, "第三层分类深度应为 3");
      bool sibling_duplicate = false;
      try {
        categories.create(root.id, "饮料", 0, true);
      } catch (const xiaotu::AppError& error) {
        sibling_duplicate = error.code() == "DUPLICATE_NAME";
      }
      check(sibling_duplicate, "同一级下重名分类应被拒绝");
      const auto subtree = categories.subtree_ids(root.id);
      check(subtree.size() == 3, "根分类子树应包含自身与两个下级");

      auto categorized = items.create(xiaotu::gen::ItemRow{.code = "C001",
                                                           .name = "可乐",
                                                           .barcode = "6901234567890",
                                                           .category_id = grandchild.id,
                                                           .tax_rate_bp = 1300});
      check(categorized.tax_rate_bp == 1300 && categorized.category_id == grandchild.id,
            "商品应保存分类与整数万分比税率");
      bool barcode_duplicate = false;
      try {
        items.create(xiaotu::gen::ItemRow{
            .code = "C002", .name = "重复条码商品", .barcode = "6901234567890"});
      } catch (const xiaotu::AppError& error) {
        barcode_duplicate = error.code() == "DUPLICATE_CODE";
      }
      check(barcode_duplicate, "重复条码应被拒绝");
      bool category_in_use = false;
      try {
        categories.remove(grandchild.id);
      } catch (const xiaotu::AppError& error) {
        category_in_use = error.code() == "CATEGORY_HAS_ITEMS";
      }
      check(category_in_use, "分类下还有商品时应拒绝删除");
      bool tax_out_of_range = false;
      try {
        items.create(xiaotu::gen::ItemRow{.code = "C003", .name = "错税率", .tax_rate_bp = 10001});
      } catch (const xiaotu::AppError& error) {
        tax_out_of_range = error.code() == "INVALID_INPUT";
      }
      check(tax_out_of_range, "税率超过 10000 个基点应被拒绝");

      // 库存不足：审核前整单校验，拒绝后不留下单据与流水。
      xiaotu::DocInput insufficient;
      insufficient.doc_type = "SAL_OUT";
      insufficient.warehouse_id = warehouse_list.at(0).id;
      insufficient.doc_date = kToday;
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
      opening.doc_date = kToday;
      opening.partner = "期初供应商";
      opening.lines.push_back(
          xiaotu::DocLineInput{.item_id = item.id, .qty = q(5), .price_cents = 1000});
      const auto opening_doc = documents.save_and_approve(opening);

      xiaotu::gen::EmployeeService employees(db);
      const auto person =
          employees.create(xiaotu::gen::EmployeeRow{.code = "E001", .name = "张三",
                                                    .phone = "13800000000"});
      check(person.id > 0, "应能创建职员档案");

      xiaotu::DocInput input;
      input.doc_type = "SAL_OUT";
      input.warehouse_id = warehouse_list.at(0).id;
      input.doc_date = kToday;
      input.partner = "测试客户";
      input.salesperson_id = person.id;
      input.lines.push_back(xiaotu::DocLineInput{.item_id = item.id, .qty = q(5), .price_cents = 1000});

      const auto created = documents.save_and_approve(input);
      check(!created.doc_no.empty(), "应生成单据号");

      const auto created_detail = documents.detail(created.id);
      check(created_detail.salesperson_id == person.id &&
                created_detail.salesperson_name == "张三",
            "销售出库应保存业务员 id 与名称快照");

      xiaotu::DocInput unknown_person = input;
      unknown_person.salesperson_id = 999999;
      bool unknown_rejected = false;
      try {
        documents.save_draft(unknown_person);
      } catch (const xiaotu::AppError& error) {
        unknown_rejected = error.code() == "NOT_FOUND";
      }
      check(unknown_rejected, "不存在的业务员应被拒绝");

      // 非销售单据即使客户端传了业务员也应清空，避免污染报表维度。
      xiaotu::DocInput other_input = input;
      other_input.doc_type = "OTH_IN";
      other_input.biz_category = "OTHER";
      const auto other_draft = documents.save_draft(other_input);
      const auto other_detail = documents.detail(other_draft.id);
      check(other_detail.salesperson_id == 0 && other_detail.salesperson_name.empty(),
            "非销售单据不应保存业务员");

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
        doc.doc_date = kToday;
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
      sale.doc_date = kToday;
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

      // v35 数据缝：批次/保质期只为将来预留。两列存在、默认空串，现有单据与流水写入后仍为空。
      db.read([&](sqlite3* conn) {
        const auto empty_default = [&](const char* table, const char* column) {
          xiaotu::Stmt stmt(conn, std::string("PRAGMA table_info(") + table + ");");
          while (stmt.step()) {
            if (stmt.text(1) == column) return stmt.text(4) == "''";
          }
          return false;
        };
        check(empty_default("doc_line", "batch_no"), "doc_line.batch_no 应存在且默认为空串");
        check(empty_default("doc_line", "expiry_date"), "doc_line.expiry_date 应存在且默认为空串");
        check(empty_default("stock_ledger", "batch_no"),
              "stock_ledger.batch_no 应存在且默认为空串");
        check(empty_default("stock_ledger", "expiry_date"),
              "stock_ledger.expiry_date 应存在且默认为空串");

        xiaotu::Stmt line_rows(conn, "SELECT COUNT(*) FROM doc_line;");
        xiaotu::Stmt line_filled(
            conn, "SELECT COUNT(*) FROM doc_line WHERE batch_no <> '' OR expiry_date <> '';");
        check(line_rows.step() && line_rows.i64(0) > 0, "单据明细应有数据用于验证数据缝");
        check(line_filled.step() && line_filled.i64(0) == 0,
              "现有单据明细的批次/保质期应保持为空串");

        xiaotu::Stmt ledger_rows(conn, "SELECT COUNT(*) FROM stock_ledger;");
        xiaotu::Stmt ledger_filled(
            conn,
            "SELECT COUNT(*) FROM stock_ledger WHERE batch_no <> '' OR expiry_date <> '';");
        check(ledger_rows.step() && ledger_rows.i64(0) > 0, "库存流水应有数据用于验证数据缝");
        check(ledger_filled.step() && ledger_filled.i64(0) == 0,
              "现有库存流水的批次/保质期应保持为空串");
      });

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
      transfer.doc_date = kToday;
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
      stocktake.doc_date = kToday;
      stocktake.lines.push_back(
          xiaotu::DocLineInput{.item_id = purchased.id, .qty = q(7), .price_cents = 0});
      const auto stocktake_doc = documents.save_and_approve(stocktake);
      check(stocktake_doc.doc_no.rfind("PDD", 0) == 0, "盘点单号应以 PDD 开头");
      auto counted = balance_of(purchased.id);
      check(counted.qty == q(7) && counted.amount_cents == 9800,
            "盘亏 3 件后应为 7 件 / 9800 分");
      check(stock.ledger(purchased.id).front().biz_type == "STK_CNT_OUT" &&
                stock.ledger(purchased.id).front().qty == q(-3),
            "盘亏应写 STK_CNT_OUT 流水且数量为负");

      documents.reverse(stocktake_doc.id);
      counted = balance_of(purchased.id);
      check(counted.qty == q(10) && counted.amount_cents == 14000, "反审核盘点后应回到 10 件 / 14000 分");
      check(stock.ledger(purchased.id).front().biz_type == "STK_CNT_OUT_REVERSE" &&
                stock.ledger(purchased.id).front().qty == q(3),
            "盘亏反审核应写 STK_CNT_OUT_REVERSE 且数量回正");

      xiaotu::DocInput surplus;
      surplus.doc_type = "STK_CNT";
      surplus.warehouse_id = warehouse_list.at(0).id;
      surplus.doc_date = kToday;
      surplus.lines.push_back(
          xiaotu::DocLineInput{.item_id = purchased.id, .qty = q(12), .price_cents = 0});
      const auto surplus_doc = documents.save_and_approve(surplus);
      counted = balance_of(purchased.id);
      check(counted.qty == q(12) && counted.amount_cents == 16800,
            "盘盈 2 件后应为 12 件 / 16800 分");
      check(stock.ledger(purchased.id).front().biz_type == "STK_CNT_IN" &&
                stock.ledger(purchased.id).front().qty == q(2),
            "盘盈应写 STK_CNT_IN 流水且数量为正");
      {
        xiaotu::CostRecalcService recalc(db);
        check(recalc.recalculate(purchased.id).difference_count == 0,
              "盘盈后成本重算应无差异");
      }
      documents.reverse(surplus_doc.id);
      check(balance_of(purchased.id).qty == q(10), "反审核盘盈后应回到 10 件");
      check(stock.ledger(purchased.id).front().biz_type == "STK_CNT_IN_REVERSE" &&
                stock.ledger(purchased.id).front().qty == q(-2),
            "盘盈反审核应写 STK_CNT_IN_REVERSE 且数量为负");

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
      empty_count.doc_date = kToday;
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
      supplier_doc.doc_date = kToday;
      supplier_doc.partner_id = supplier.id;
      supplier_doc.partner = "手工填写的名称应被档案覆盖";
      supplier_doc.lines.push_back(
          xiaotu::DocLineInput{.item_id = supplier_item.id, .qty = q(2), .price_cents = 500});
      const auto supplier_doc_created = documents.save_and_approve(supplier_doc);
      const auto supplier_doc_detail = documents.detail(supplier_doc_created.id);
      check(supplier_doc_detail.partner_id == supplier.id, "采购单据应记录供应商 id");
      check(supplier_doc_detail.partner == "测试供应商", "单据往来单位应取供应商档案名称");
      db.read([&](sqlite3* conn) {
        xiaotu::Stmt key(conn, "SELECT partner_key FROM ar_ap_entry WHERE doc_id = ?;");
        key.bind(1, supplier_doc_created.id);
        check(key.step(), "采购审核后应生成应付记录");
        const std::int64_t partner_id = xiaotu::find_partner_for_archive(
            conn, xiaotu::PartnerSide::Supplier, supplier.id);
        check(partner_id > 0, "新供应商首次写入应补建 partner 与映射");
        check(key.text(0) == "P:" + std::to_string(partner_id),
              "采购应付键应写作 P:{partner_id}");
      });

      // 同一新供应商被第二张单据引用：只复用同一条 partner 与映射，审核不失败。
      xiaotu::DocInput supplier_doc2;
      supplier_doc2.doc_type = "PUR_IN";
      supplier_doc2.warehouse_id = warehouse_list.at(0).id;
      supplier_doc2.doc_date = kToday;
      supplier_doc2.partner_id = supplier.id;
      supplier_doc2.lines.push_back(
          xiaotu::DocLineInput{.item_id = supplier_item.id, .qty = q(1), .price_cents = 500});
      const auto supplier_doc2_created = documents.save_and_approve(supplier_doc2);
      check(supplier_doc2_created.id > 0, "同一供应商的第二张单据应审核成功");
      db.read([&](sqlite3* conn) {
        xiaotu::Stmt map(conn,
                         "SELECT COUNT(*), MIN(partner_id) FROM partner_legacy_map"
                         " WHERE side = 'supplier' AND legacy_id = ?;");
        map.bind(1, supplier.id);
        check(map.step() && map.i64(0) == 1, "同一供应商只应有一条 legacy 映射");
        const std::int64_t partner_id = map.i64(1);
        xiaotu::Stmt origin(conn, "SELECT origin FROM partner WHERE id = ?;");
        origin.bind(1, partner_id);
        check(origin.step() && origin.text(0) == "AUTO_ARCHIVE",
              "自动补建的 partner 应标记 AUTO_ARCHIVE");
        xiaotu::Stmt keys(conn,
                          "SELECT DISTINCT partner_key FROM ar_ap_entry"
                          " WHERE doc_id IN (?, ?);");
        keys.bind(1, supplier_doc_created.id).bind(2, supplier_doc2_created.id);
        int rows = 0;
        while (keys.step()) {
          ++rows;
          check(keys.text(0) == "P:" + std::to_string(partner_id),
                "两张单据应复用同一 P: 键");
        }
        check(rows == 1, "两张单据的应付记录应指向同一个 partner");
      });

      bool missing_supplier_rejected = false;
      try {
        xiaotu::DocInput bad;
        bad.doc_type = "PUR_IN";
        bad.warehouse_id = warehouse_list.at(0).id;
        bad.doc_date = kToday;
        bad.partner_id = 999999;
        bad.lines.push_back(
            xiaotu::DocLineInput{.item_id = supplier_item.id, .qty = q(1), .price_cents = 100});
        documents.save_and_approve(bad);
      } catch (const xiaotu::AppError& error) {
        missing_supplier_rejected = error.code() == "NOT_FOUND";
      }
      check(missing_supplier_rejected, "引用不存在的供应商应被拒绝");

      // 采购单据必须填供应商：草稿可暂不填，审核时拒绝。
      {
        xiaotu::DocInput no_supplier_draft;
        no_supplier_draft.doc_type = "PUR_IN";
        no_supplier_draft.warehouse_id = warehouse_list.at(0).id;
        no_supplier_draft.doc_date = kToday;
        no_supplier_draft.lines.push_back(
            xiaotu::DocLineInput{.item_id = supplier_item.id, .qty = q(1), .price_cents = 100});
        const auto created_draft = documents.save_draft(no_supplier_draft);
        bool approve_rejected = false;
        try {
          documents.approve(created_draft.id);
        } catch (const xiaotu::AppError& error) {
          approve_rejected =
              error.code() == "INVALID_INPUT" && error.what() == std::string("采购单据必须选择供应商");
        }
        check(approve_rejected, "草稿未选供应商时审核应被拒绝");
        documents.delete_draft(created_draft.id);
      }

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
        opening.doc_date = kToday;
        opening.partner = "期初供应商";
        opening.lines.push_back(
            xiaotu::DocLineInput{.item_id = sales_item.id, .qty = q(10), .price_cents = 1000});
        documents.save_and_approve(opening);
      }

      xiaotu::DocInput customer_sale;
      customer_sale.doc_type = "SAL_OUT";
      customer_sale.warehouse_id = warehouse_list.at(0).id;
      customer_sale.doc_date = kToday;
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
      db.read([&](sqlite3* conn) {
        xiaotu::Stmt key(conn, "SELECT partner_key FROM ar_ap_entry WHERE doc_id = ?;");
        key.bind(1, customer_sale_doc.id);
        check(key.step() && key.text(0) == "P:" + std::to_string(customer.id),
              "销售应收键应写作 P:{partner_id}");
        xiaotu::Stmt sale_header(conn,
                                 "SELECT partner_id, customer_id FROM doc_header WHERE id = ?;");
        sale_header.bind(1, customer_sale_doc.id);
        check(sale_header.step() && sale_header.i64(0) == customer.id &&
                  sale_header.i64(1) == customer.id,
              "销售单据应同时写 partner_id 与 customer_id");
        xiaotu::Stmt origin(conn, "SELECT origin FROM partner WHERE id = ?;");
        origin.bind(1, customer.id);
        check(origin.step() && origin.text(0) == "AUTO_ARCHIVE",
              "自动补建的客户 partner 应标记 AUTO_ARCHIVE");
      });
      check(documents.detail(customer_sale_doc.id).partner == "测试客户档案",
            "销售单据应取客户档案名称");

      documents.reverse(customer_sale_doc.id);
      // 列表默认排除作废台账，与账龄口径一致；显式按状态查询仍可看到历史。
      const auto after_void = finance.entries(1, customer_key);
      check(after_void.empty(), "反审核后应收记录不应再出现在未结列表中");
      const auto voided_rows = finance.entries(1, customer_key, "VOIDED");
      check(voided_rows.size() == 1 && voided_rows.at(0).doc_id == customer_sale_doc.id,
            "显式按 VOIDED 状态查询应能看到已作废记录");

      xiaotu::DocInput customer_return;
      customer_return.doc_type = "SAL_RET";
      customer_return.warehouse_id = warehouse_list.at(0).id;
      customer_return.doc_date = kToday;
      customer_return.partner_id = customer.id;
      customer_return.lines.push_back(
          xiaotu::DocLineInput{.item_id = sales_item.id, .qty = q(1), .price_cents = 2000});
      documents.save_and_approve(customer_return);
      check(finance.entries(1, customer_key).at(0).amount_cents == -2000,
            "销售退货应生成负数应收（红字）");

      const auto ap_rows = finance.entries(-1, "S:" + std::to_string(supplier.id));
      std::int64_t ap_total = 0;
      for (const auto& row : ap_rows) ap_total += row.amount_cents;
      check(ap_rows.size() == 2 && ap_total == 1500,
            "同一供应商两张采购入库都应生成应付记录");

      // 核销：部分核销、多单合并、超额/重复拒绝、撤销用反向记录。
      xiaotu::SettlementService settlements(db);
      const auto sale_a = [&] {
        xiaotu::DocInput doc;
        doc.doc_type = "SAL_OUT";
        doc.warehouse_id = warehouse_list.at(0).id;
        doc.doc_date = kToday;
        doc.partner_id = customer.id;
        doc.lines.push_back(
            xiaotu::DocLineInput{.item_id = sales_item.id, .qty = q(3), .price_cents = 1000});
        return documents.save_and_approve(doc);
      }();
      xiaotu::DocInput sale_b_input;
      sale_b_input.doc_type = "SAL_OUT";
      sale_b_input.warehouse_id = warehouse_list.at(0).id;
      sale_b_input.doc_date = kToday;
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
        input.settled_on = kToday;
        input.note = note;
        input.allocations = std::move(allocations);
        return settlements.create(input);
      };
      const auto first_settlement =
          make_settlement(1, customer_key, 1000, {{entry_a.id, 1000}}, "部分收款");
      check(first_settlement.settle_no.rfind("SK" + kTodayYmd + "-", 0) == 0,
            "收款单号应以 SK 开头");
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
      report_filter.from = kMonthStart;
      report_filter.to = kToday;
      report_filter.item_id = sales_item.id;
      const auto report_ledger = reports.stock_ledger(report_filter);
      check(!report_ledger.empty() && report_ledger.size() == stock.ledger(sales_item.id).size(),
            "流水明细应与库存流水条数一致");
      check(report_ledger.front().biz_time.rfind(kToday, 0) == 0,
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
      // 成本取审核写入的流水：退货按移动加权成本冲回 1000、
      // 两笔出库按均价 1000 结转 3000 + 2000；已反审核的销售单不计入。
      check(profits.front().qty == q(4) && profits.front().revenue_cents == 3000 &&
                profits.front().cost_cents == 4000 && profits.front().profit_cents == -1000,
            "销售毛利应为 4 件 / 收入 3000 / 成本 4000 / 毛利 -1000 分");

      const auto aging = reports.ar_ap_aging(1, kToday);
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
          reports.partner_statement(customer_key, 1, kMonthStart, kFarFuture);
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
      // 撤销收付款：按撤销当天入账、标注原单，并给出期间核销净额。
      check(statement.reverse_count == 1 && statement.reverse_cents == 1000,
            "对账单应统计 1 笔 1000 分的撤销收付款");
      check(statement.settle_net_cents == 3500,
            "期间核销净额应为原收款 1000 + 合并收款 3500 − 撤销 1000");
      bool found_reverse_line = false;
      for (const auto& line : statement.lines) {
        if (!line.is_reverse) continue;
        found_reverse_line = line.reverses_no == first_settlement.settle_no &&
                             line.increase_cents == 1000;
      }
      check(found_reverse_line, "对账单撤销行应带原收款单号与冲回金额");

      const auto later_statement =
          reports.partner_statement(customer_key, 1, kFarFuture, "");
      check(later_statement.opening_cents == -500 && later_statement.lines.empty() &&
                later_statement.closing_cents == -500,
            "期间之外的发生额应全部计入下一期期初");

      // 采购报表：入库为正、退货为负；汇总按净额计算均价（整数四舍五入）。
      const auto report_supplier = suppliers.create(
          xiaotu::gen::SupplierRow{.code = "GYS900", .name = "报表供应商"});
      const auto report_item =
          items.create(xiaotu::gen::ItemRow{.code = "PR01", .name = "采购报表商品"});
      const auto make_purchase = [&](const std::string& doc_type, std::int64_t qty,
                                     std::int64_t price_cents) {
        xiaotu::DocInput doc;
        doc.doc_type = doc_type;
        doc.warehouse_id = warehouse_list.at(0).id;
        doc.doc_date = kToday;
        doc.partner_id = report_supplier.id;
        doc.lines.push_back(xiaotu::DocLineInput{.item_id = report_item.id, .qty = qty,
                                                 .price_cents = price_cents});
        return documents.save_and_approve(doc);
      };
      make_purchase("PUR_IN", q(10), 1000);
      make_purchase("PUR_IN", q(5), 1200);
      make_purchase("PUR_RET", q(4), 1000);

      xiaotu::ReportFilter purchase_filter;
      purchase_filter.from = kMonthStart;
      purchase_filter.to = kToday;
      purchase_filter.item_id = report_item.id;
      const auto purchase_rows = reports.purchase_detail(purchase_filter);
      check(purchase_rows.size() == 3, "采购明细应含两笔入库与一笔退货");
      bool detail_sorted = true;
      for (std::size_t i = 1; i < purchase_rows.size(); ++i) {
        if (purchase_rows[i - 1].date > purchase_rows[i].date ||
            (purchase_rows[i - 1].date == purchase_rows[i].date &&
             purchase_rows[i - 1].doc_no > purchase_rows[i].doc_no)) {
          detail_sorted = false;
        }
      }
      check(detail_sorted, "采购明细应按日期、单号升序");
      check(purchase_rows.front().kind == "采购入库" && purchase_rows.front().qty == q(10) &&
                purchase_rows.front().amount_cents == 10000,
            "采购明细入库行数量与金额应为正");
      check(purchase_rows.back().kind == "采购退货" && purchase_rows.back().qty == -q(4) &&
                purchase_rows.back().amount_cents == -4000 &&
                purchase_rows.back().price_cents == 1000,
            "采购明细退货行数量与金额应为负、单价保持正数");
      const auto purchase_totals = reports.purchase_detail_totals(purchase_filter);
      check(purchase_totals.rows == 3 && purchase_totals.qty == q(11) &&
                purchase_totals.amount_cents == 12000,
            "采购明细合计应按全量计算且不依赖行数上限");

      purchase_filter.item_id = 0;
      purchase_filter.supplier_id = report_supplier.id;
      const auto by_supplier = reports.purchase_summary_by_supplier(purchase_filter);
      check(by_supplier.size() == 1 && by_supplier.front().supplier == "报表供应商",
            "按供应商汇总应返回一行并取档案名称");
      check(by_supplier.front().qty == q(11) && by_supplier.front().amount_cents == 12000,
            "按供应商汇总应为净数量 11、净金额 12000 分");
      check(by_supplier.front().in_qty == q(15) && by_supplier.front().in_amount_cents == 16000 &&
                by_supplier.front().return_amount_cents == 4000 && by_supplier.front().has_inbound,
            "按供应商汇总应分列入库数量 15、入库金额 16000、退货额 4000");
      check(by_supplier.front().avg_price_cents == 1067,
            "入库均价只取入库行（16000 ÷ 15 = 10.67 元），退货不参与");
      const auto supplier_totals = reports.purchase_summary_totals(purchase_filter);
      check(supplier_totals.qty == q(11) && supplier_totals.amount_cents == 12000 &&
                supplier_totals.in_qty == q(15) &&
                supplier_totals.in_amount_cents == 16000 &&
                supplier_totals.return_amount_cents == 4000 &&
                supplier_totals.avg_price_cents == 1067,
            "采购汇总合计应按全量入库行计算均价，不能对分组均价再平均");

      purchase_filter.item_id = report_item.id;
      const auto by_item = reports.purchase_summary_by_item(purchase_filter);
      check(by_item.size() == 1 && by_item.front().item_code == "PR01" &&
                by_item.front().qty == q(11) && by_item.front().amount_cents == 12000 &&
                by_item.front().avg_price_cents == 1067,
            "按商品汇总应与按供应商汇总的入库均价一致");

      // 三种边界：只有退货、退货价高于进货价、入库与退货数量相等。
      const auto edge_supplier = suppliers.create(
          xiaotu::gen::SupplierRow{.code = "GYS901", .name = "边界供应商"});
      // 只退货组的库存由另一家供应商入库提供，保证筛选 edge_supplier 时只有退货行。
      const auto edge_stock_supplier = suppliers.create(
          xiaotu::gen::SupplierRow{.code = "GYS902", .name = "边界备货供应商"});
      const auto make_edge = [&](const std::string& doc_type, std::int64_t item_id,
                                 std::int64_t qty, std::int64_t price_cents,
                                 std::int64_t supplier_id) {
        xiaotu::DocInput doc;
        doc.doc_type = doc_type;
        doc.warehouse_id = warehouse_list.at(0).id;
        doc.doc_date = kToday;
        doc.partner_id = supplier_id;
        doc.lines.push_back(
            xiaotu::DocLineInput{.item_id = item_id, .qty = qty, .price_cents = price_cents});
        return documents.save_and_approve(doc);
      };
      auto only_return = items.create(xiaotu::gen::ItemRow{.code = "PR02", .name = "只退货商品"});
      make_edge("PUR_IN", only_return.id, q(10), 1000, edge_stock_supplier.id);
      make_edge("PUR_RET", only_return.id, q(3), 2000, edge_supplier.id);
      const auto higher_return = items.create(
          xiaotu::gen::ItemRow{.code = "PR03", .name = "退价高于进价商品"});
      make_edge("PUR_IN", higher_return.id, q(10), 1000, edge_supplier.id);
      make_edge("PUR_RET", higher_return.id, q(2), 1500, edge_supplier.id);
      auto equal_qty =
          items.create(xiaotu::gen::ItemRow{.code = "PR04", .name = "入库退货相等商品"});
      make_edge("PUR_IN", equal_qty.id, q(5), 800, edge_supplier.id);
      make_edge("PUR_RET", equal_qty.id, q(5), 800, edge_supplier.id);

      xiaotu::ReportFilter edge_filter;
      edge_filter.from = kMonthStart;
      edge_filter.to = kToday;
      edge_filter.supplier_id = edge_supplier.id;
      const auto edge_rows = reports.purchase_summary_by_item(edge_filter);
      const auto find_edge = [&](const std::string& code) {
        for (const auto& row : edge_rows) {
          if (row.item_code == code) return row;
        }
        return xiaotu::PurchaseItemSummaryRow{};
      };
      const auto only_return_row = find_edge("PR02");
      check(!only_return_row.has_inbound && only_return_row.in_qty == 0 &&
                only_return_row.qty == -q(3) && only_return_row.return_amount_cents == 6000,
            "只有退货的分组应无入库、净数量为负、退货额单列");
      const auto higher_return_row = find_edge("PR03");
      check(higher_return_row.has_inbound && higher_return_row.avg_price_cents == 1000 &&
                higher_return_row.return_amount_cents == 3000 &&
                higher_return_row.amount_cents == 7000,
            "退价高于进价时均价仍取入库价，退货额按实际退货金额单列");
      const auto equal_row = find_edge("PR04");
      check(equal_row.has_inbound && equal_row.qty == 0 && equal_row.amount_cents == 0 &&
                equal_row.avg_price_cents == 800,
            "入库与退货数量相等时净数量为 0，均价仍为入库均价");
      edge_filter.supplier_id = 0;
      edge_filter.item_id = only_return.id;
      edge_filter.supplier_id = edge_supplier.id;
      const auto only_return_totals = reports.purchase_summary_totals(edge_filter);
      check(only_return_totals.qty == -q(3) && only_return_totals.in_qty == 0 &&
                !only_return_totals.has_inbound,
            "合计口径下无入库行时应标记无入库，由前端显示「—」");

      purchase_filter.supplier_id = 999999;
      check(reports.purchase_summary_by_supplier(purchase_filter).empty() &&
                reports.purchase_detail(purchase_filter).empty(),
            "按不存在的供应商筛选应返回空");
      purchase_filter.supplier_id = report_supplier.id;
      purchase_filter.warehouse_id = 999999;
      check(reports.purchase_detail(purchase_filter).empty(), "按不存在的仓库筛选应返回空");

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
      draft_input.doc_date = kToday;
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

      // 表头备注：往返、审核后保留、超长拒绝（按 UTF-8 码点而不是字节）、空值可选。
      {
        auto remark_item =
            items.create(xiaotu::gen::ItemRow{.code = "RMK1", .name = "备注商品"});
        xiaotu::DocInput remark_input;
        remark_input.doc_type = "PUR_IN";
        remark_input.warehouse_id = warehouse_list.at(0).id;
        remark_input.doc_date = kToday;
        remark_input.partner = "备注供应商";
        remark_input.lines.push_back(
            xiaotu::DocLineInput{.item_id = remark_item.id, .qty = q(1), .price_cents = 100});
        remark_input.remark = "首次备注";
        const auto remark_draft = documents.save_draft(remark_input);
        check(documents.detail(remark_draft.id).remark == "首次备注", "草稿应保存表头备注");
        remark_input.remark = "改后备注";
        documents.update_draft(remark_draft.id, remark_input);
        check(documents.detail(remark_draft.id).remark == "改后备注", "草稿应可修改表头备注");
        documents.approve(remark_draft.id);
        check(documents.detail(remark_draft.id).remark == "改后备注", "审核不应丢失表头备注");

        // 200 个汉字（UTF-8 下 600 字节）应通过：上限按码点计，不按字节计。
        std::string max_remark;
        for (int i = 0; i < 200; ++i) max_remark += "备";
        remark_input.remark = max_remark;
        const auto max_draft = documents.save_draft(remark_input);
        check(documents.detail(max_draft.id).remark == max_remark &&
                  documents.detail(max_draft.id).remark.size() == 600,
              "200 个汉字备注应完整保存");

        remark_input.remark = max_remark + "备";
        bool remark_rejected = false;
        try {
          documents.save_draft(remark_input);
        } catch (const xiaotu::AppError& error) {
          remark_rejected = error.code() == "INVALID_INPUT" &&
                            std::string(error.what()).find("200") != std::string::npos;
        }
        check(remark_rejected, "超过 200 个字符的备注应明确拒绝");

        remark_input.remark = "";
        const auto empty_draft = documents.save_draft(remark_input);
        check(documents.detail(empty_draft.id).remark.empty(), "备注为空应可正常保存");
        items.remove(remark_item.id);
      }

      // 预收：不填分摊也应保存，资金流水与余额要跟着变动。
      const auto prepayment = make_settlement(1, customer_key, 500, {}, "预收");
      check(prepayment.settle_no.rfind("SK", 0) == 0, "预收也应使用收款单号");
      const auto fund_ledger = settlements.ledger(1, kMonthStart, kToday);
      check(!fund_ledger.empty() && fund_ledger.front().opening, "资金流水应包含期初行");
      check(fund_ledger.back().balance_cents == 4000, "资金流水期末余额应为 40.00 元");

      const auto fund_balances = settlements.account_balances(kToday);
      check(fund_balances.size() == 1 && fund_balances.front().inflow_cents == 4000 &&
                fund_balances.front().outflow_cents == 0 &&
                fund_balances.front().balance_cents == 4000,
            "账户余额应为累计收款减累计付款");

      settlements.reverse(prepayment.id);
      check(settlements.ledger(1, kMonthStart, kToday).back().balance_cents == 3500,
            "撤销预收后资金余额应回退");

      // 资金余额符号：期初 + 收款 − 付款，支出列不重复负号。
      const auto payment = make_settlement(-1, customer_key, 500, {}, "付款");
      const auto after_payment = settlements.account_balances(kToday);
      check(after_payment.size() == 1 && after_payment.front().outflow_cents == 500 &&
                after_payment.front().balance_cents == 3000,
            "期初 3500 付款 500 后余额应为 3000（不是 4000）");
      const auto payment_row = settlements.ledger(1, kMonthStart, kToday).back();
      check(payment_row.expense_cents == 500 && payment_row.balance_cents == 3000,
            "资金流水支出列应显示正数 500，余额 3000");
      settlements.reverse(payment.id);
      const auto after_payment_reverse = settlements.account_balances(kToday);
      check(after_payment_reverse.front().balance_cents == 3500,
            "撤销付款后余额应回到 3500");
      const auto reversed_payment_row = settlements.ledger(1, kMonthStart, kToday).back();
      check(reversed_payment_row.expense_cents == 500 &&
                reversed_payment_row.balance_cents == 3500,
            "撤销付款的支出列不应出现负号，余额回到 3500");

      // 账龄按截止日时点还原：业务日期晚于截止日不计入，核销按核销日期还原。
      // 每月 1 日跑测试时月初与今天相同，跳过该场景避免日期重合。
      if (kMonthStart != kToday) {
        const auto point_customer = customers.create(
            xiaotu::gen::CustomerRow{.code = "KH900", .name = "时点客户"});
        const std::string point_key = "P:" + std::to_string(point_customer.id);
        xiaotu::DocInput month_sale;
        month_sale.doc_type = "SAL_OUT";
        month_sale.warehouse_id = warehouse_list.at(0).id;
        month_sale.doc_date = kMonthStart;
        month_sale.partner_id = point_customer.id;
        month_sale.lines.push_back(
            xiaotu::DocLineInput{.item_id = sales_item.id, .qty = q(1), .price_cents = 1000});
        const auto month_doc = documents.save_and_approve(month_sale);
        xiaotu::DocInput today_sale = month_sale;
        today_sale.doc_date = kToday;
        documents.save_and_approve(today_sale);

        // 截止到月初：只看到月初那张，且未被今天才发生的核销冲减。
        std::int64_t before_settle = -1;
        for (const auto& row : reports.ar_ap_aging(1, kMonthStart)) {
          if (row.partner_key == point_key) before_settle = row.total_cents;
        }
        check(before_settle == 1000, "截止到月初只应包含月初单据 1000 分，今天单据不计入");

        std::int64_t month_entry_id = 0;
        for (const auto& row : finance.entries(1, point_key)) {
          if (row.doc_id == month_doc.id) month_entry_id = row.id;
        }
        xiaotu::SettlementCreateInput point_settle;
        point_settle.direction = 1;
        point_settle.partner_key = point_key;
        point_settle.amount_cents = 1000;
        point_settle.account_id = 1;
        point_settle.method = "CASH";
        point_settle.settled_on = kToday;
        point_settle.allocations.push_back(
            xiaotu::AllocationInput{.entry_id = month_entry_id, .amount_cents = 1000});
        settlements.create(point_settle);

        std::int64_t after_at_month = -1;
        std::int64_t after_at_today = -1;
        for (const auto& row : reports.ar_ap_aging(1, kMonthStart)) {
          if (row.partner_key == point_key) after_at_month = row.total_cents;
        }
        for (const auto& row : reports.ar_ap_aging(1, kToday)) {
          if (row.partner_key == point_key) after_at_today = row.total_cents;
        }
        check(after_at_month == 1000, "截止到月初时，今天的核销不应提前冲减月初余额");
        check(after_at_today == 1000,
              "截止到今天时，月初单据已核销、只剩今天单据 1000 分");
        customers.remove(point_customer.id);
      }

      {
        // 归并预览（测试数据，不是真实业务数据）：同税号应命中高置信组，
        // 同名的 C:/S:/T: 三个 key 应单独列出。
        const auto preview_customer = customers.create(xiaotu::gen::CustomerRow{
            .code = "PREV-C1",
            .name = "预览测试甲",
            .phone = "138-0000-1111",
            .tax_no = "91330000TEST001"});
        const auto preview_supplier = suppliers.create(xiaotu::gen::SupplierRow{
            .code = "PREV-S1",
            .name = "预览测试甲",
            .phone = "13800001111",
            .tax_no = "9133-0000-TEST001"});
        const auto name_customer = customers.create(xiaotu::gen::CustomerRow{
            .code = "PREV-C2", .name = "预览同名乙", .phone = "13900000001"});
        const auto name_supplier = suppliers.create(xiaotu::gen::SupplierRow{
            .code = "PREV-S2", .name = "预览同名乙", .phone = "13900000002"});
        const std::string customer_key = "C:" + std::to_string(preview_customer.id);
        const std::string supplier_key = "S:" + std::to_string(preview_supplier.id);
        db.write([&](sqlite3* conn) {
          for (const auto& [key, direction, amount] :
               std::vector<std::tuple<std::string, int, std::int64_t>>{
                   {customer_key, 1, 1200}, {supplier_key, -1, 800}, {"T:预览测试甲", 1, 300}}) {
            xiaotu::Stmt stmt(conn,
                              "INSERT INTO ar_ap_entry (id, doc_id, doc_no, doc_type, direction,"
                              " partner_id, partner_name, partner_key, biz_date, due_date,"
                              " amount_cents, settled_cents, status, created_at, created_by)"
                              " VALUES (?, 0, '预览', 'OPENING_AR', ?, 0, '预览测试甲', ?, ?, '',"
                              " ?, 0, 'OPEN', ?, 'test');");
            stmt.bind(1, xiaotu::next_id())
                .bind(2, static_cast<std::int64_t>(direction))
                .bind(3, key)
                .bind(4, kToday)
                .bind(5, amount)
                .bind(6, kToday + " 00:00:00")
                .run();
          }
        });
        const auto preview = xiaotu::partner_merge_preview(db);
        check(preview.high_count == 1, "同税号客户/供应商应命中 1 组高置信");
        std::int64_t name_only_preview = 0;
        for (const auto& group : preview.groups) {
          if (group.match == xiaotu::PreviewMatch::NameOnly &&
              group.match_value == "预览同名乙") {
            ++name_only_preview;
          }
        }
        check(name_only_preview == 1, "仅同名一对应命中 1 组需人工确认");
        check(!preview.groups.empty() && preview.groups.front().keys_conflict,
              "跨客户/供应商的候选组应标记多个 partner_key");
        bool found_three_keys = false;
        for (const auto& unit : preview.multi_key_units) {
          if (unit.name == "预览测试甲" && unit.keys.size() == 3) found_three_keys = true;
        }
        check(found_three_keys, "同名 C:/S:/T: 三个 key 应列入多 key 清单");
        db.write([&](sqlite3* conn) {
          xiaotu::Stmt stmt(conn,
                            "DELETE FROM ar_ap_entry WHERE partner_key IN (?, ?, 'T:预览测试甲');");
          stmt.bind(1, customer_key).bind(2, supplier_key).run();
        });
        customers.remove(preview_customer.id);
        suppliers.remove(preview_supplier.id);
        customers.remove(name_customer.id);
        suppliers.remove(name_supplier.id);
      }

      items.remove(draft_item.id);
      items.remove(sales_returned.id);
      items.remove(returned.id);
      items.remove(purchased.id);
      items.remove(item.id);
      items.remove(supplier_item.id);
      items.remove(bottled.id);
      items.remove(sales_item.id);
      items.remove(report_item.id);
      items.remove(only_return.id);
      items.remove(higher_return.id);
      items.remove(equal_qty.id);
      items.remove(decimal_item.id);
      suppliers.remove(supplier.id);
      suppliers.remove(edge_supplier.id);
      suppliers.remove(edge_stock_supplier.id);
      customers.remove(customer.id);
      items.remove(categorized.id);
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
      check(xiaotu::print_targets().size() == 25, "打印目标应包含 11 种单据与 14 张报表");

      // 成本调整单：仓库与原因在明细行，只改结存金额；反审核不能把金额冲成负数。
      const auto adjust_item = items.create(
          xiaotu::gen::ItemRow{.code = "ADJ001", .name = "成本调整测试", .unit_id = 1});
      make_doc("PUR_IN", adjust_item.id, q(10), 1000);
      const std::int64_t adjust_wh = warehouse_list.at(0).id;

      xiaotu::DocInput adjust;
      adjust.doc_type = "CST_ADJ";
      adjust.doc_date = kToday;
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
      {
        // 数量为 0、只动金额的流水在收发明细与进销存汇总里单列「其他调整」。
        xiaotu::ReportFilter adjust_filter;
        adjust_filter.from = kMonthStart;
        adjust_filter.to = kToday;
        adjust_filter.item_id = adjust_item.id;
        const auto adjust_flows = reports.goods_flow(adjust_filter);
        check(adjust_flows.size() == 1, "成本调整商品在收发明细中应有一行");
        check(adjust_flows.front().in_amount_cents == 10000 &&
                  adjust_flows.front().out_amount_cents == 0 &&
                  adjust_flows.front().adjust_amount_cents == 500,
              "成本调整 500 分应只落在「其他调整」，不并入收入 / 发出");
        const auto adjust_summary = reports.inventory_summary(adjust_filter);
        check(adjust_summary.size() == 1 &&
                  adjust_summary.front().adjust_amount_cents == 500 &&
                  adjust_summary.front().closing_amount_cents == 10500,
              "进销存汇总期末金额应包含其他调整 500 分");
      }
      const auto adjust_detail = documents.detail(adjust_doc.id);
      check(adjust_detail.lines.at(0).warehouse_id == adjust_wh &&
                adjust_detail.lines.at(0).adjust_reason == "运费分摊",
            "成本调整单应逐行保存仓库与原因");
      {
        // 含税价：逐行算税再汇总，净额 + 税额 = 价税合计，整数分不差一。
        const auto tax_item = items.create(
            xiaotu::gen::ItemRow{.code = "TAX001", .name = "含税测试", .unit_id = 1});
        xiaotu::DocInput tax_doc;
        tax_doc.doc_type = "OTH_IN";
        tax_doc.warehouse_id = adjust_wh;
        tax_doc.doc_date = kToday;
        tax_doc.biz_category = "OTHER";
        tax_doc.lines.push_back(xiaotu::DocLineInput{.item_id = tax_item.id,
                                                     .qty = q(3),
                                                     .price_cents = 100,
                                                     .tax_rate_bp = 1300});
        tax_doc.lines.push_back(xiaotu::DocLineInput{.item_id = tax_item.id,
                                                     .qty = q(1),
                                                     .price_cents = 113,
                                                     .tax_rate_bp = 1300,
                                                     .price_includes_tax = true});
        const auto tax_created = documents.save_and_approve(tax_doc);
        const auto tax_detail = documents.detail(tax_created.id);
        check(tax_detail.lines.at(0).net_cents == 300 && tax_detail.lines.at(0).tax_cents == 39,
              "不含税单价 3.00 × 3 应按 13% 算出税额 0.39");
        check(tax_detail.lines.at(1).net_cents == 100 && tax_detail.lines.at(1).tax_cents == 13,
              "含税单价 1.13 应按 13% 反算出净额 1.00 / 税额 0.13");
        check(tax_detail.total_amount_cents == 413 && tax_detail.total_net_cents == 400 &&
                  tax_detail.total_tax_cents == 52 && tax_detail.total_with_tax_cents == 452,
              "单据合计应为金额 4.13 / 净额 4.00 / 税额 0.52 / 价税合计 4.52");
        check(tax_detail.lines.at(0).net_cents + tax_detail.lines.at(0).tax_cents == 339 &&
                  tax_detail.lines.at(1).net_cents + tax_detail.lines.at(1).tax_cents == 113,
              "每行净额 + 税额都应等于价税合计");
        documents.reverse(tax_created.id);
        items.remove(tax_item.id);
      }
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

      // 社区版打印模板上限为 1，多模板行为只在额度足够的版本里验证。
      if (xiaotu::EditionPolicy::instance().limit("max_print_templates") > 2) {
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
      }  // 打印模板额度足够时才验证多模板行为

      // 打印布局计划：纸型、可打印宽度、缩放与告警在服务端一次算清，页面 / 打印 / PDF 共用。
      // 预设：A4 纵向、页边距 12mm → 可打印宽 186mm；241×140 连续纸 → 可打印宽 217mm。
      const auto plan_for = [](xiaotu::PrintTemplate tmpl) {
        return xiaotu::plan_print_layout(tmpl, xiaotu::resolved_print_columns(tmpl));
      };
      const auto sheet_a4 = [&](const char* key) {
        auto tmpl = xiaotu::builtin_print_template("REPORT", key);
        tmpl.page_size = "A4";
        tmpl.orientation = "PORTRAIT";
        tmpl.paper_kind = "SHEET";
        tmpl.scale_mode = "FIT_WIDTH";
        tmpl.margin_mm = 12;
        tmpl.font_size_pt = 10;
        return plan_for(tmpl);
      };

      const auto purchase_a4 = sheet_a4("purchase-detail");
      check(purchase_a4.table_width_mm == 252 && purchase_a4.printable_width_mm == 186,
            "采购明细表宽应为 252mm，A4 纵向可打印 186mm");
      check(purchase_a4.scale_percent == 73 && purchase_a4.warn == "none",
            "采购明细按纸宽自适应应缩到 73%");
      check(static_cast<int>(purchase_a4.effective_font_pt * 10) == 73,
            "采购明细缩放后字号应约 7.3pt");

      const auto fund_a4 = sheet_a4("fund-ledger");
      check(fund_a4.table_width_mm == 214 && fund_a4.scale_percent == 86,
            "资金流水 214mm 应缩到 86%");
      check(static_cast<int>(fund_a4.effective_font_pt * 10) == 86,
            "资金流水缩放后字号应约 8.6pt");

      const auto flow_a4 = sheet_a4("goods-flow");
      check(flow_a4.table_width_mm == 200 && flow_a4.scale_percent == 93,
            "收发明细 200mm 应缩到 93%");

      auto doc_tmpl = xiaotu::builtin_print_template("DOC", "SAL_OUT");
      check(xiaotu::plan_print_layout(doc_tmpl, xiaotu::resolved_print_columns(doc_tmpl))
                    .table_width_mm == 210,
            "单据明细列宽合计应为 210mm（金额列改名不改宽度）");

      auto continuous = xiaotu::builtin_print_template("REPORT", "purchase-detail");
      continuous.page_size = "CUSTOM";
      continuous.paper_width_mm = 241;
      continuous.paper_height_mm = 140;
      continuous.paper_kind = "CONTINUOUS";
      continuous.scale_mode = "FIT_WIDTH";
      continuous.margin_mm = 12;
      const auto continuous_plan = plan_for(continuous);
      check(continuous_plan.printable_width_mm == 217 && continuous_plan.scale_percent == 100 &&
                continuous_plan.overflow_mm == 35 && continuous_plan.warn == "continuous_overflow",
            "连续纸不参与缩放，超宽 35mm 应明确告警");

      auto original = xiaotu::builtin_print_template("REPORT", "purchase-detail");
      original.scale_mode = "ORIGINAL";
      original.margin_mm = 12;
      const auto original_plan = plan_for(original);
      check(original_plan.scale_percent == 100 && original_plan.overflow_mm == 66 &&
                original_plan.warn == "original_overflow" && original_plan.suggest_landscape,
            "原始尺寸超宽应告警并建议改横向");

      auto landscape = xiaotu::builtin_print_template("REPORT", "purchase-detail");
      landscape.orientation = "LANDSCAPE";
      landscape.margin_mm = 12;
      const auto landscape_plan = plan_for(landscape);
      check(landscape_plan.printable_width_mm == 273 && landscape_plan.scale_percent == 100 &&
                landscape_plan.warn == "none",
            "A4 横向 273mm 应容下采购明细，无需缩放");

      auto tiny = xiaotu::builtin_print_template("REPORT", "purchase-detail");
      tiny.font_size_pt = 9;
      tiny.margin_mm = 12;
      check(plan_for(tiny).warn == "font_too_small", "缩放后字号低于 7pt 应提示精简列");

      // 销售订单：确认不动库存/流水，转出库受累计数量约束。
      auto ordered = items.create(xiaotu::gen::ItemRow{.code = "SO001", .name = "订单商品"});
      make_doc("PUR_IN", ordered.id, q(10), 500);
      const auto ordered_before = balance_of(ordered.id);
      const auto ordered_ledger_before = stock.ledger(ordered.id).size();

      xiaotu::DocInput order_input;
      order_input.doc_type = "SAL_ORDER";
      order_input.warehouse_id = warehouse_list.at(0).id;
      order_input.doc_date = kToday;
      order_input.partner = "订单客户";
      order_input.remark = "订单备注A";
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
      check(documents.detail(partial.doc_id).remark == "订单备注A",
            "转出应原样继承订单备注，不拼来源单号");
      // 订单确认后没有界面改备注的入口；这里直接改库，模拟「之后订单备注变了」。
      db.write([&](sqlite3* conn) {
        xiaotu::Stmt changed(conn, "UPDATE doc_header SET remark = '订单备注B' WHERE id = ?;");
        changed.bind(1, order.id).run();
      });
      check(documents.detail(partial.doc_id).remark == "订单备注A",
            "订单备注后改不应影响已生成的下游单据");
      bool downstream_rejected = false;
      try {
        documents.convert_order(partial.doc_id, convert);
      } catch (const xiaotu::AppError& error) {
        downstream_rejected = error.code() == "INVALID_INPUT" &&
                              std::string(error.what()).find("不是订单") != std::string::npos;
      }
      check(downstream_rejected, "下游单据不能再往下派生，备注只传一层");
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
      check(documents.detail(full.doc_id).remark == "订单备注B",
            "后续转出应取生成时刻的订单备注快照");
      bool no_more = false;
      try {
        documents.convert_order(order.id, convert);
      } catch (const xiaotu::AppError& error) {
        no_more = error.code() == "INVALID_STATUS";
      }
      check(no_more, "已全部转出的订单应拒绝继续转出");

      // 模拟用户把下游单据的备注手工改过：反审核 + 重转不应覆盖它。
      db.write([&](sqlite3* conn) {
        xiaotu::Stmt edited(
            conn, "UPDATE doc_header SET remark = '手工改备注' WHERE id = ?;");
        edited.bind(1, partial.doc_id).run();
      });
      documents.reverse(partial.doc_id);
      check(documents.detail(partial.doc_id).remark == "手工改备注",
            "反审核不应改动下游单据备注");
      order_detail = documents.detail(order.id);
      check(order_detail.order_status == "CONFIRMED" &&
                order_detail.lines.at(0).converted_qty == q(2),
            "反审核出库单应释放对应订单数量");
      convert.lines.at(0).qty = q(2);
      const auto reconverted = documents.convert_order(order.id, convert);
      check(documents.detail(partial.doc_id).remark == "手工改备注",
            "重转不应覆盖旧单被人手工改过的备注");
      check(documents.detail(reconverted.doc_id).remark == "订单备注B",
            "重转生成的新单取当时的订单备注");

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

      // 空备注：转出后目标单仍为空串，不产生「由订单…转入」之类的文本。
      xiaotu::DocInput empty_order_input;
      empty_order_input.doc_type = "SAL_ORDER";
      empty_order_input.warehouse_id = warehouse_list.at(0).id;
      empty_order_input.doc_date = kToday;
      empty_order_input.partner = "空备注客户";
      empty_order_input.lines.push_back(
          {.item_id = ordered.id, .qty = q(1), .price_cents = 1500});
      const auto empty_order = documents.save_and_approve(empty_order_input);
      const std::int64_t empty_order_line = documents.detail(empty_order.id).lines.at(0).id;
      xiaotu::OrderConvertInput empty_convert;
      empty_convert.lines.push_back({.order_line_id = empty_order_line, .qty = q(1)});
      const auto empty_converted = documents.convert_order(empty_order.id, empty_convert);
      check(documents.detail(empty_converted.doc_id).remark.empty(),
            "空备注订单转出后目标单备注仍为空");

      // 采购订单：确认不动库存/往来，下推生成采购入库，支持分批与改价。
      auto purchase_item =
          items.create(xiaotu::gen::ItemRow{.code = "PO001", .name = "采购订单商品"});
      auto ap_entries = [&](std::int64_t doc_id) {
        std::int64_t count = 0;
        db.read([&](sqlite3* conn) {
          xiaotu::Stmt stmt(conn, "SELECT COUNT(*) FROM ar_ap_entry WHERE doc_id = ?;");
          stmt.bind(1, doc_id);
          if (stmt.step()) count = stmt.i64(0);
        });
        return count;
      };

      xiaotu::DocInput purchase_order_input;
      purchase_order_input.doc_type = "PUR_ORDER";
      purchase_order_input.warehouse_id = warehouse_list.at(0).id;
      purchase_order_input.doc_date = kToday;
      purchase_order_input.partner = "订单供应商";
      purchase_order_input.remark = "采购订单备注";
      purchase_order_input.lines.push_back(
          {.item_id = purchase_item.id, .qty = q(10), .price_cents = 800});
      const auto purchase_order = documents.save_and_approve(purchase_order_input);
      check(purchase_order.doc_no.rfind("CGDD", 0) == 0, "采购订单单号应以 CGDD 开头");
      auto purchase_detail = documents.detail(purchase_order.id);
      check(purchase_detail.order_status == "CONFIRMED", "采购订单确认后应为 CONFIRMED");
      check(purchase_detail.partner == "订单供应商", "采购订单应保存供应商名称快照");
      const auto* sales_order_print = xiaotu::find_print_target("DOC", "SAL_ORDER");
      const auto* purchase_order_print = xiaotu::find_print_target("DOC", "PUR_ORDER");
      bool same_print_columns = sales_order_print != nullptr && purchase_order_print != nullptr &&
                                sales_order_print->columns.size() ==
                                    purchase_order_print->columns.size();
      if (same_print_columns) {
        for (std::size_t i = 0; i < sales_order_print->columns.size(); ++i) {
          if (sales_order_print->columns[i].label != purchase_order_print->columns[i].label) {
            same_print_columns = false;
            break;
          }
        }
      }
      check(purchase_order_print != nullptr && purchase_order_print->name == "采购订单" &&
                same_print_columns,
            "采购订单打印目标与销售订单列应一致");
      check(balance_of(purchase_item.id).qty == 0, "采购订单确认不应改变库存");
      check(stock.ledger(purchase_item.id).empty(), "采购订单确认不应写库存流水");
      check(ap_entries(purchase_order.id) == 0, "采购订单确认不应产生应付");
      const std::int64_t purchase_line_id = purchase_detail.lines.at(0).id;

      xiaotu::OrderConvertInput purchase_convert;
      purchase_convert.lines.push_back(
          {.order_line_id = purchase_line_id, .qty = q(4), .price_cents = 900});
      const auto first_receipt = documents.convert_order(purchase_order.id, purchase_convert);
      check(first_receipt.doc_no.rfind("CGRK", 0) == 0, "下推应生成采购入库单");
      check(documents.detail(first_receipt.doc_id).remark == "采购订单备注",
            "采购订单转入库同样继承备注");
      purchase_detail = documents.detail(purchase_order.id);
      check(purchase_detail.order_status == "CONFIRMED" &&
                purchase_detail.lines.at(0).converted_qty == q(4),
            "部分入库后订单应保持 CONFIRMED 且累计 4 件");
      check(balance_of(purchase_item.id).qty == q(4), "采购入库应增加库存");
      check(balance_of(purchase_item.id).amount_cents == 3600,
            "下推单价覆盖应只影响入库单金额（4 × 900）");
      check(ap_entries(first_receipt.doc_id) == 1, "采购入库应生成应付");
      check(documents.detail(purchase_order.id).total_amount_cents == 8000,
            "下推改价不应回写订单金额");
      const auto purchase_list = documents.list("PUR_ORDER");
      check(purchase_list.size() == 1 && purchase_list.front().converted_qty == q(4),
            "采购订单列表应返回累计已下推数量");

      purchase_convert.lines.at(0).qty = q(7);
      purchase_convert.lines.at(0).price_cents = -1;
      bool purchase_exceeded = false;
      try {
        documents.convert_order(purchase_order.id, purchase_convert);
      } catch (const xiaotu::AppError& error) {
        purchase_exceeded = error.code() == "ORDER_QTY_EXCEEDED";
      }
      check(purchase_exceeded, "采购累计入库超过订单数量应拒绝");
      check(balance_of(purchase_item.id).qty == q(4), "超量拒绝后库存不应变化");

      purchase_convert.lines.at(0).qty = q(6);
      const auto second_receipt = documents.convert_order(purchase_order.id, purchase_convert);
      check(second_receipt.order_status == "CONVERTED", "全部入库后订单应为 CONVERTED");
      check(balance_of(purchase_item.id).qty == q(10), "两次入库应累计到订单数量");
      check(balance_of(purchase_item.id).amount_cents == 8400,
            "第二次应按订单原价 800 入库");
      bool purchase_no_more = false;
      try {
        documents.convert_order(purchase_order.id, purchase_convert);
      } catch (const xiaotu::AppError& error) {
        purchase_no_more = error.code() == "INVALID_STATUS";
      }
      check(purchase_no_more, "已全部入库的订单应拒绝继续下推");

      documents.reverse(first_receipt.doc_id);
      purchase_detail = documents.detail(purchase_order.id);
      check(purchase_detail.order_status == "CONFIRMED" &&
                purchase_detail.lines.at(0).converted_qty == q(6),
            "反审核入库单应释放对应订单数量");
      check(balance_of(purchase_item.id).qty == q(6), "反审核后库存应回到 6 件");

      documents.close_order(purchase_order.id);
      check(documents.detail(purchase_order.id).order_status == "CLOSED",
            "部分入库的采购订单应可手工关闭");
      purchase_convert.lines.at(0).qty = q(1);
      bool purchase_closed_rejected = false;
      try {
        documents.convert_order(purchase_order.id, purchase_convert);
      } catch (const xiaotu::AppError& error) {
        purchase_closed_rejected = error.code() == "INVALID_STATUS";
      }
      check(purchase_closed_rejected, "关闭后的采购订单应拒绝下推");

      // 其他入库/出库：入库按手工单价，出库按移动加权平均，且不产生往来。
      auto other = items.create(xiaotu::gen::ItemRow{.code = "O001", .name = "其他商品"});
      xiaotu::DocInput other_in;
      other_in.doc_type = "OTH_IN";
      other_in.warehouse_id = warehouse_list.at(0).id;
      other_in.doc_date = kToday;
      other_in.partner = "备注：受赠样品";
      other_in.biz_category = "GIFT";
      other_in.lines.push_back(
          xiaotu::DocLineInput{.item_id = other.id, .qty = q(4), .price_cents = 250});
      const auto other_in_doc = documents.save_and_approve(other_in);
      check(other_in_doc.doc_no.rfind("QTRK", 0) == 0, "其他入库单号应以 QTRK 开头");
      check(documents.detail(other_in_doc.id).biz_category == "GIFT",
            "其他入库应保存业务类型");
      auto other_balance = balance_of(other.id);
      check(other_balance.qty == q(4) && other_balance.amount_cents == 1000,
            "其他入库应按录入单价入账");

      xiaotu::DocInput other_out;
      other_out.doc_type = "OTH_OUT";
      other_out.warehouse_id = warehouse_list.at(0).id;
      other_out.doc_date = kToday;
      other_out.partner = "备注：领用";
      other_out.biz_category = "USAGE";
      other_out.lines.push_back(
          xiaotu::DocLineInput{.item_id = other.id, .qty = q(1), .price_cents = 9999});
      const auto other_out_doc = documents.save_and_approve(other_out);
      check(other_out_doc.doc_no.rfind("QTCK", 0) == 0, "其他出库单号应以 QTCK 开头");
      check(documents.detail(other_out_doc.id).biz_category == "USAGE",
            "其他出库应保存业务类型");
      check(documents.list("OTH_IN").front().biz_category == "GIFT",
            "其他入库列表应返回业务类型");

      xiaotu::ReportFilter category_filter;
      category_filter.biz_category = "USAGE";
      const auto usage_ledger = reports.stock_ledger(category_filter);
      check(usage_ledger.size() == 1 && usage_ledger.front().biz_category == "USAGE" &&
                usage_ledger.front().biz_type == "OTH_OUT",
            "库存流水应能按业务类型筛选");
      const auto usage_totals = reports.stock_ledger_totals(category_filter);
      check(usage_totals.rows == 1 && usage_totals.qty == q(-1) &&
                usage_totals.amount_cents < 0,
            "库存流水合计应按全量计算且数量方向保持原样");

      xiaotu::ReportFilter other_filter;
      other_filter.from = kMonthStart;
      other_filter.to = kToday;
      other_filter.biz_category = "GIFT";
      const auto gift_summary = reports.other_flow_summary(other_filter);
      check(gift_summary.size() == 1 && gift_summary[0].in_qty == q(4) &&
                gift_summary[0].in_amount_cents == 1000 && gift_summary[0].out_qty == 0,
            "其他出入库汇总应按业务类型归集赠品入库");
      other_filter.biz_category = "USAGE";
      const auto usage_summary = reports.other_flow_summary(other_filter);
      check(usage_summary.size() == 1 && usage_summary[0].out_qty == q(1) &&
                usage_summary[0].out_amount_cents > 0,
            "其他出入库汇总应归集领用出库且出库金额取绝对值");
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
        overdraw.doc_date = kToday;
        overdraw.lines.push_back(
            xiaotu::DocLineInput{.item_id = other.id, .qty = q(4), .price_cents = 0});
        documents.save_and_approve(overdraw);
      } catch (const xiaotu::AppError& error) {
        other_out_rejected = error.code() == "STOCK_INSUFFICIENT";
      }
      check(other_out_rejected, "其他出库库存不足应整单拒绝");
      check(balance_of(other.id).qty == q(3), "被拒绝的其他出库不应改变库存");

      // 业务类型方向限制：期初只入库、领用只出库；未选默认归入其他；非其他单据忽略。
      bool opening_out_rejected = false;
      try {
        xiaotu::DocInput invalid = other_out;
        invalid.biz_category = "OPENING";
        documents.save_and_approve(invalid);
      } catch (const xiaotu::AppError& error) {
        opening_out_rejected = error.code() == "INVALID_INPUT";
      }
      check(opening_out_rejected, "其他出库不允许期初业务类型");

      bool usage_in_rejected = false;
      try {
        xiaotu::DocInput invalid = other_in;
        invalid.biz_category = "USAGE";
        documents.save_and_approve(invalid);
      } catch (const xiaotu::AppError& error) {
        usage_in_rejected = error.code() == "INVALID_INPUT";
      }
      check(usage_in_rejected, "其他入库不允许领用业务类型");

      bool loss_out_allowed = false;
      try {
        xiaotu::DocInput invalid = other_out;
        invalid.biz_category = "LOSS";
        const auto loss_doc = documents.save_and_approve(invalid);
        loss_out_allowed = documents.detail(loss_doc.id).biz_category == "LOSS";
        documents.reverse(loss_doc.id);
      } catch (const xiaotu::AppError&) {
        loss_out_allowed = false;
      }
      check(loss_out_allowed, "其他出库应允许报损业务类型");
      check(balance_of(other.id).qty == q(3), "反审核报损后库存应回到 3 件");

      xiaotu::DocInput default_category;
      default_category.doc_type = "OTH_IN";
      default_category.warehouse_id = warehouse_list.at(0).id;
      default_category.doc_date = kToday;
      default_category.lines.push_back(
          xiaotu::DocLineInput{.item_id = other.id, .qty = q(1), .price_cents = 100});
      const auto default_draft = documents.save_draft(default_category);
      check(documents.detail(default_draft.id).biz_category == "OTHER",
            "其他入库未选业务类型时应归入其他");
      documents.delete_draft(default_draft.id);

      xiaotu::DocInput non_other_draft;
      non_other_draft.doc_type = "PUR_IN";
      non_other_draft.warehouse_id = warehouse_list.at(0).id;
      non_other_draft.doc_date = kToday;
      non_other_draft.biz_category = "LOSS";
      non_other_draft.lines.push_back(
          xiaotu::DocLineInput{.item_id = other.id, .qty = q(1), .price_cents = 100});
      const auto non_other_created = documents.save_draft(non_other_draft);
      check(documents.detail(non_other_created.id).biz_category.empty(),
            "非其他出入库单据应忽略业务类型");
      documents.delete_draft(non_other_created.id);

      // 并发验收：8 张草稿同时审核、库存只够 3 张，必须恰好成功 3 张且不超卖。
      auto concurrent =
          items.create(xiaotu::gen::ItemRow{.code = "CC1", .name = "并发商品"});
      make_doc("PUR_IN", concurrent.id, q(10), 100);

      std::vector<std::int64_t> draft_ids;
      for (int index = 0; index < 8; ++index) {
        xiaotu::DocInput draft;
        draft.doc_type = "SAL_OUT";
        draft.warehouse_id = warehouse_list.at(0).id;
        draft.doc_date = kToday;
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
      reverse_sale.doc_date = kToday;
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

      // 数据库触发器是最后兜底：绕过业务层直接写负结存也必须被拒绝。
      bool negative_rejected = false;
      std::string negative_message;
      try {
        db.write([&](sqlite3* conn) {
          xiaotu::Stmt update(conn, "UPDATE stock_balance SET qty = -1 WHERE item_id = ?;");
          update.bind(1, claimed.id).run();
        });
      } catch (const xiaotu::AppError& error) {
        negative_rejected = true;
        negative_message = error.what();
      }
      check(negative_rejected, "数据库应拒绝负结存");
      check(negative_message.find("不能为负") != std::string::npos,
            "负结存错误应说明数量/金额不能为负");
      check(balance_of(claimed.id).qty == q(2), "负结存写入被拒后库存不应变化");

      // 负库存开关：默认关时出库超库存被拒；显式开启后可形成负结存，关回后恢复拦截。
      xiaotu::SettingService settings(db);
      check(!settings.load().allow_negative_stock, "负库存开关默认应为关闭");
      const auto oversell_item =
          items.create(xiaotu::gen::ItemRow{.code = "P003", .name = "负库存商品"});
      make_doc("PUR_IN", oversell_item.id, q(1), 100);
      bool oversell_rejected = false;
      try {
        make_doc("SAL_OUT", oversell_item.id, q(2), 200);
      } catch (const xiaotu::AppError& error) {
        oversell_rejected = error.code() == "STOCK_INSUFFICIENT";
      }
      check(oversell_rejected, "负库存关闭时超库存出库应被拒绝");
      auto setting = settings.load();
      setting.allow_negative_stock = true;
      settings.update(setting);
      make_doc("SAL_OUT", oversell_item.id, q(2), 200);
      check(balance_of(oversell_item.id).qty == -q(1), "开启负库存后结存应为 -1");
      setting.allow_negative_stock = false;
      settings.update(setting);
      bool negative_blocked_again = false;
      try {
        db.write([&](sqlite3* conn) {
          xiaotu::Stmt update(conn, "UPDATE stock_balance SET qty = -1 WHERE item_id = ?;");
          update.bind(1, oversell_item.id).run();
        });
      } catch (const xiaotu::AppError&) {
        negative_blocked_again = true;
      }
      check(negative_blocked_again, "负库存关回后触发器应继续拒绝负结存");

      // 成本口径：默认含税；切到不含税后只影响新审核单据，历史结存与流水不变。
      const auto basis_item =
          items.create(xiaotu::gen::ItemRow{.code = "BASIS1", .name = "口径商品", .unit_id = 1});
      check(settings.load().cost_basis == "INCLUSIVE", "成本口径默认应为含税价");
      check(settings.load().cost_basis_changed_at.empty(), "未切换过口径时切换日应为空");
      const auto basis_doc = [&](const std::string& doc_type, std::int64_t price_cents) {
        xiaotu::DocInput doc;
        doc.doc_type = doc_type;
        doc.warehouse_id = warehouse_list.at(0).id;
        doc.doc_date = kToday;
        doc.partner = "口径往来单位";
        if (doc_type == "OTH_IN") doc.biz_category = "OTHER";
        doc.lines.push_back(xiaotu::DocLineInput{.item_id = basis_item.id,
                                                 .qty = q(1),
                                                 .price_cents = price_cents,
                                                 .tax_rate_bp = 1300,
                                                 .price_includes_tax = true});
        return documents.save_and_approve(doc);
      };
      const auto inclusive_doc = basis_doc("PUR_IN", 11300);
      check(balance_of(basis_item.id).amount_cents == 11300,
            "含税口径新单成本应等于价税合计 11300 分");
      const auto ledger_before = stock.ledger(basis_item.id);
      check(ledger_before.size() == 1 && ledger_before.front().amount_cents == 11300,
            "含税口径入库流水金额应为价税合计");

      auto basis_setting = settings.load();
      basis_setting.cost_basis = "NET";
      settings.update(basis_setting);
      const auto switched_setting = settings.load();
      check(switched_setting.cost_basis == "NET" && !switched_setting.cost_basis_changed_at.empty(),
            "切换成本口径应记录生效时间");
      check(balance_of(basis_item.id).amount_cents == 11300, "切换口径不应改动历史结存");
      const auto ledger_after_switch = stock.ledger(basis_item.id);
      check(ledger_after_switch.size() == 1 &&
                ledger_after_switch.front().amount_cents == 11300,
            "切换口径不应改动历史流水金额");

      const auto net_doc = basis_doc("PUR_IN", 11300);
      check(balance_of(basis_item.id).amount_cents == 21300,
            "不含税口径新单成本应等于净额 10000 分");
      check(11300 - 10000 == 1300, "同一含税单两种口径的成本差额应等于税额");
      const auto basis_other_in_doc = basis_doc("OTH_IN", 11300);
      check(balance_of(basis_item.id).amount_cents == 31300,
            "其他入库应与采购入库共用同一取值路径");
      const auto return_doc = basis_doc("PUR_RET", 11300);
      check(balance_of(basis_item.id).amount_cents == 21300,
            "采购退货应按净额冲减库存金额");

      // 切回默认，避免影响后续用例；单据与商品一并清理。
      basis_setting.cost_basis = "INCLUSIVE";
      settings.update(basis_setting);
      check(settings.load().cost_basis == "INCLUSIVE", "成本口径应能切回含税价");
      documents.reverse(return_doc.id);
      documents.reverse(basis_other_in_doc.id);
      documents.reverse(net_doc.id);
      documents.reverse(inclusive_doc.id);
      check(balance_of(basis_item.id).amount_cents == 0, "反审核后口径商品结存金额应清零");
      items.remove(basis_item.id);

      // 信用额度：卡在销售出库审核，默认提示不拦、确认后放行并写审计；reject 模式硬拦。
      xiaotu::CustomerService credit_customers(db);
      const auto credit_customer =
          credit_customers.create(xiaotu::gen::CustomerRow{.code = "C-CREDIT",
                                                           .name = "额度客户",
                                                           .credit_limit_cents = 100000});
      check(credit_customers.credit_limit_cents(credit_customer.id) == 100000,
            "客户建档后额度应从 partner 读回");
      auto credit_item = items.create(xiaotu::gen::ItemRow{.code = "CR001", .name = "额度商品"});
      {
        xiaotu::DocInput stock_in;
        stock_in.doc_type = "OTH_IN";
        stock_in.warehouse_id = warehouse_list.at(0).id;
        stock_in.doc_date = kToday;
        stock_in.partner = "期初";
        stock_in.lines.push_back(
            xiaotu::DocLineInput{.item_id = credit_item.id, .qty = q(100), .price_cents = 10000});
        documents.save_and_approve(stock_in);
      }
      const auto credit_sale = [&](std::int64_t qty) {
        xiaotu::DocInput sale;
        sale.doc_type = "SAL_OUT";
        sale.warehouse_id = warehouse_list.at(0).id;
        sale.doc_date = kToday;
        sale.partner_id = credit_customer.id;
        sale.partner = credit_customer.name;
        sale.lines.push_back(
            xiaotu::DocLineInput{.item_id = credit_item.id, .qty = qty, .price_cents = 10000});
        return sale;
      };
      documents.save_and_approve(credit_sale(q(8)));
      bool credit_warning = false;
      try {
        documents.save_and_approve(credit_sale(q(3)));
      } catch (const xiaotu::AppError& error) {
        credit_warning = error.code() == "CREDIT_LIMIT_WARNING";
        check(std::string(error.what()).find("超出 100.00") != std::string::npos,
              "超额提示应带出超出金额");
      }
      check(credit_warning, "超过信用额度时应返回可确认的提示");
      const auto confirmed = documents.save_and_approve(credit_sale(q(3)), true);
      check(confirmed.id > 0, "二次确认后应允许继续出库");
      {
        std::int64_t audited = 0;
        db.read([&](sqlite3* conn) {
          xiaotu::Stmt stmt(conn,
                            "SELECT COUNT(*) FROM audit_log"
                            " WHERE action = 'credit_over_limit' AND entity_id = ?;");
          stmt.bind(1, confirmed.id);
          if (stmt.step()) audited = stmt.i64(0);
        });
        check(audited == 1, "超额度放行应写审计日志");
      }
      documents.set_credit_limit_mode("reject");
      bool credit_rejected = false;
      try {
        documents.save_and_approve(credit_sale(q(1)));
      } catch (const xiaotu::AppError& error) {
        credit_rejected = error.code() == "CREDIT_LIMIT_EXCEEDED";
      }
      check(credit_rejected, "reject 模式下超额应直接拒绝");
      documents.set_credit_limit_mode("warn");

      // 额度单一来源：页面改额度后 partner 与 customer 镜像一致。
      const auto credit_edit = credit_customers.update(
          credit_customer.id,
          xiaotu::gen::CustomerRow{.code = "C-CREDIT", .name = "额度客户",
                                   .credit_limit_cents = 123400});
      check(credit_edit.credit_limit_cents == 123400, "客户页改额度应返回新值");
      {
        std::int64_t partner_limit = 0;
        std::int64_t mirror_limit = 0;
        db.read([&](sqlite3* conn) {
          xiaotu::Stmt partner(conn, "SELECT credit_limit_cents FROM partner WHERE id = ?;");
          partner.bind(1, credit_customer.id);
          if (partner.step()) partner_limit = partner.i64(0);
          xiaotu::Stmt mirror(conn, "SELECT credit_limit_cents FROM customer WHERE id = ?;");
          mirror.bind(1, credit_customer.id);
          if (mirror.step()) mirror_limit = mirror.i64(0);
        });
        check(partner_limit == 123400 && mirror_limit == 123400,
              "客户页改额度后 partner 与 customer 两处应一致");
      }
      // 超额判断读 partner：把两边人为改成不同值，partner 更小就必须提示。
      credit_customers.update(credit_customer.id,
                              xiaotu::gen::CustomerRow{.code = "C-CREDIT",
                                                       .name = "额度客户",
                                                       .credit_limit_cents = 100000});
      db.write([&](sqlite3* conn) {
        xiaotu::Stmt partner(conn, "UPDATE partner SET credit_limit_cents = 10000 WHERE id = ?;");
        partner.bind(1, credit_customer.id).run();
        xiaotu::Stmt mirror(conn,
                            "UPDATE customer SET credit_limit_cents = 1000000 WHERE id = ?;");
        mirror.bind(1, credit_customer.id).run();
      });
      check(credit_customers.credit_limit_cents(credit_customer.id) == 10000,
            "读取额度应取 partner，而不是更大的 customer 镜像");
      {
        bool listed_from_partner = false;
        for (const auto& row : credit_customers.list()) {
          if (row.id == credit_customer.id) listed_from_partner = row.credit_limit_cents == 10000;
        }
        check(listed_from_partner, "客户列表展示的额度应来自 partner");
      }
      {
        bool partner_based_warning = false;
        try {
          documents.save_and_approve(credit_sale(q(2)));
        } catch (const xiaotu::AppError& error) {
          partner_based_warning = error.code() == "CREDIT_LIMIT_WARNING" &&
                                  std::string(error.what()).find("额度 100.00") !=
                                      std::string::npos;
        }
        check(partner_based_warning, "超额判断应以 partner 额度为准（镜像更大也不放行）");
      }

      // 历史分裂修补：partner 为 0、镜像非 0 时并回 partner，且可重复执行。
      db.write([&](sqlite3* conn) {
        xiaotu::Stmt partner(conn, "UPDATE partner SET credit_limit_cents = 0 WHERE id = ?;");
        partner.bind(1, credit_customer.id).run();
        xiaotu::Stmt mirror(conn,
                            "UPDATE customer SET credit_limit_cents = 666600 WHERE id = ?;");
        mirror.bind(1, credit_customer.id).run();
      });
      std::size_t repaired = 0;
      db.write([&](sqlite3* conn) {
        repaired = xiaotu::repair_customer_credit_mirror(conn);
      });
      check(repaired == 1, "partner 额度为 0 而镜像非 0 时应并回 partner");
      {
        std::int64_t partner_limit = 0;
        db.read([&](sqlite3* conn) {
          xiaotu::Stmt stmt(conn, "SELECT credit_limit_cents FROM partner WHERE id = ?;");
          stmt.bind(1, credit_customer.id);
          if (stmt.step()) partner_limit = stmt.i64(0);
        });
        check(partner_limit == 666600, "修补后 partner 应等于镜像值");
      }
      repaired = 0;
      db.write([&](sqlite3* conn) {
        repaired = xiaotu::repair_customer_credit_mirror(conn);
      });
      check(repaired == 0, "重复修补应幂等");
      credit_customers.update(credit_customer.id,
                              xiaotu::gen::CustomerRow{.code = "C-CREDIT",
                                                       .name = "额度客户",
                                                       .credit_limit_cents = 100000});

      // ---- 单据幂等与乐观锁 ----
      {
        const auto doc_customer = credit_customers.create(xiaotu::gen::CustomerRow{
            .code = "C-DOC", .name = "幂等客户", .credit_limit_cents = 0});
        const auto idem_item =
            items.create(xiaotu::gen::ItemRow{.code = "IDEM01", .name = "幂等商品"});
        xiaotu::DocInput opening;
        opening.doc_type = "OTH_IN";
        opening.warehouse_id = warehouse_list.at(0).id;
        opening.doc_date = kToday;
        opening.partner = "期初";
        opening.lines.push_back(
            xiaotu::DocLineInput{.item_id = idem_item.id, .qty = q(500), .price_cents = 1000});
        documents.save_and_approve(opening);
        const auto idem_sale = [&](const std::string& request_id) {
          xiaotu::DocInput sale;
          sale.doc_type = "SAL_OUT";
          sale.warehouse_id = warehouse_list.at(0).id;
          sale.doc_date = kToday;
          sale.partner_id = doc_customer.id;
          sale.partner = doc_customer.name;
          sale.request_id = request_id;
          sale.lines.push_back(
              xiaotu::DocLineInput{.item_id = idem_item.id, .qty = q(1), .price_cents = 1000});
          return sale;
        };
        const auto first = documents.save_and_approve(idem_sale("req-idem-1"));
        const auto replay = documents.save_and_approve(idem_sale("req-idem-1"));
        check(first.id == replay.id && first.doc_no == replay.doc_no,
              "同一 request_id 重复提交应返回首次创建的单据");
        check(first.version == 1 && replay.version == 1, "新建即审核的单据版本应为 1");
        std::int64_t headers = 0;
        db.read([&](sqlite3* conn) {
          xiaotu::Stmt stmt(conn, "SELECT COUNT(*) FROM doc_header WHERE request_id = ?;");
          stmt.bind(1, "req-idem-1");
          if (stmt.step()) headers = stmt.i64(0);
        });
        check(headers == 1, "同一 request_id 只应产生一张单据");

        const auto draft = documents.save_draft(idem_sale("req-idem-2"));
        check(draft.version == 0, "草稿版本应为 0");
        documents.approve(draft.id, false, 0);
        check(documents.detail(draft.id).version == 1, "审核成功后版本应递增");
        bool concurrent_conflict = false;
        try {
          documents.approve(draft.id, false, 0);
        } catch (const xiaotu::AppError& error) {
          concurrent_conflict = error.code() == "VERSION_CONFLICT";
        }
        check(concurrent_conflict, "两人拿同一版本审核，第二个应返回冲突");

        // 老客户端：不带 request_id / version 仍能保存、审核、反审核。
        const auto legacy = documents.save_and_approve(idem_sale(""));
        check(legacy.id > 0, "不带 request_id 的老请求仍能保存审核");
        documents.reverse(legacy.id);
        check(documents.detail(legacy.id).status == "VOIDED", "不带 version 的老请求仍能反审核");
      }

      // 备份与恢复：快照可校验，恢复后 schema 一致，覆盖前留安全备份。
      const auto backup_root =
          std::filesystem::temp_directory_path() / "xiaotu-backup-smoke";
      std::filesystem::remove_all(backup_root);
      const auto snapshot = xiaotu::backup_database(db, backup_root / "backups", "smoke");
      check(std::filesystem::exists(snapshot.path) && snapshot.bytes > 0,
            "备份应生成非空快照");
      xiaotu::verify_database(snapshot.path);

      const auto restored_path = backup_root / "restored.db";
      // 恢复审计：命令行约定 actor=cli，成功/失败都要留痕。
      auto last_restore_audit = [](const std::filesystem::path& path) {
        xiaotu::AuditRow row;
        xiaotu::Db db(path);
        db.read([&](sqlite3* conn) {
          xiaotu::Stmt stmt(
              conn,
              "SELECT id, at, actor, action, entity, entity_id, entity_no, detail FROM audit_log"
              " WHERE action = 'restore' ORDER BY id DESC LIMIT 1;");
          if (stmt.step()) {
            row = xiaotu::AuditRow{.id = stmt.i64(0),
                                   .at = stmt.text(1),
                                   .actor = stmt.text(2),
                                   .action = stmt.text(3),
                                   .entity = stmt.text(4),
                                   .entity_id = stmt.i64(5),
                                   .entity_no = stmt.text(6),
                                   .detail = stmt.text(7)};
          }
        });
        return row;
      };
      const auto first_restore =
          xiaotu::restore_database(snapshot.path, restored_path, "cli", "命令行");
      check(first_restore.safety_backup.empty(), "目标库不存在时不应产生安全备份");
      {
        xiaotu::Db restored(restored_path);
        check(restored.schema_version() == db.schema_version(),
              "恢复后 schema 版本应与备份一致");
      }
      {
        const auto audit = last_restore_audit(restored_path);
        check(audit.actor == "cli" && audit.action == "restore" && audit.entity == "database",
              "恢复成功应写 restore 审计，actor 固定为 cli");
        check(audit.entity_no == snapshot.path.filename().string(),
              "恢复审计 entity_no 应为备份文件名");
        check(audit.detail.find("命令行") != std::string::npos &&
                  audit.detail.find("schema: v") != std::string::npos,
              "恢复审计 detail 应写明来源与 schema 版本");
      }

      const auto second_restore =
          xiaotu::restore_database(snapshot.path, restored_path, "cli", "命令行");
      check(!second_restore.safety_backup.empty(), "覆盖恢复应留下 pre-restore 安全备份");
      {
        const auto audit = last_restore_audit(restored_path);
        check(audit.detail.find(second_restore.safety_backup.filename().string()) !=
                  std::string::npos,
              "恢复审计 detail 应带恢复前安全备份文件名");
      }

      // 失败也要留痕：损坏的备份在复制前就校验失败，目标库仍可写。
      const auto corrupt = backup_root / "corrupt.db";
      {
        std::ofstream out(corrupt, std::ios::binary);
        out << "not a sqlite database";
      }
      bool restore_failed = false;
      try {
        xiaotu::restore_database(corrupt, restored_path, "cli", "命令行");
      } catch (const xiaotu::AppError&) {
        restore_failed = true;
      }
      check(restore_failed, "损坏的备份应恢复失败");
      {
        const auto audit = last_restore_audit(restored_path);
        check(audit.action == "restore" && audit.detail.find("失败原因") != std::string::npos,
              "恢复失败也应写审计并说明失败原因");
      }
      std::filesystem::remove_all(backup_root);

      // v28 有数据情形：独立库先迁到 28，再回退成 v27，写入客户/供应商后走生产迁移路径。
      {
        xiaotu::Db mig(dir / "v28-migrate.db", 2);
        xiaotu::migrate(mig);
        mig.write_raw([&](sqlite3* conn) {
          const int rc =
              sqlite3_exec(conn,
                           "DROP TABLE partner_merge_log;"
                           "DROP TABLE partner_legacy_map;"
                           "DROP TABLE partner;"
                           "DROP TABLE local_remember_token;"
                           "DROP INDEX IF EXISTS uk_doc_header_request;"
                           "ALTER TABLE doc_header DROP COLUMN request_id;"
                           "ALTER TABLE doc_header DROP COLUMN version;"
                           "ALTER TABLE doc_header DROP COLUMN remark;"
                           "ALTER TABLE app_user DROP COLUMN must_change_password;"
                           "ALTER TABLE app_user DROP COLUMN failed_count;"
                           "ALTER TABLE app_user DROP COLUMN locked_until;"
                           "ALTER TABLE doc_line DROP COLUMN batch_no;"
                           "ALTER TABLE doc_line DROP COLUMN expiry_date;"
                           "ALTER TABLE stock_ledger DROP COLUMN batch_no;"
                           "ALTER TABLE stock_ledger DROP COLUMN expiry_date;"
                           "DELETE FROM app_setting WHERE key IN"
                           " ('cost_basis', 'cost_basis_changed_at');"
                           "DELETE FROM schema_migration WHERE version >= 28;"
                           "PRAGMA user_version = 27;",
                           nullptr, nullptr, nullptr);
          check(rc == SQLITE_OK, "模拟库应能回退到 v27 结构");
        });
        check(mig.schema_version() == 27, "回退后模拟库应为 v27");

        xiaotu::gen::CustomerService mig_customers(mig);
        xiaotu::gen::SupplierService mig_suppliers(mig);
        const auto cust_a = mig_customers.create(xiaotu::gen::CustomerRow{
            .code = "C001", .name = "迁移客户甲", .contact = "张三",
            .phone = "13800000001", .tax_no = "TAX-C001",
            .credit_limit_cents = 500000, .address = "甲地址"});
        const auto cust_b = mig_customers.create(
            xiaotu::gen::CustomerRow{.code = "C002", .name = "迁移客户乙"});
        const auto sup_a = mig_suppliers.create(xiaotu::gen::SupplierRow{
            .code = "S001", .name = "迁移供应商甲", .phone = "13900000001",
            .tax_no = "TAX-S001"});
        // 与客户编码冲突，验证追加 -S{legacy_id} 后缀。
        const auto sup_b = mig_suppliers.create(
            xiaotu::gen::SupplierRow{.code = "C001", .name = "迁移供应商乙"});

        mig.write([&](sqlite3* conn) {
          xiaotu::Stmt stmt(conn,
                            "INSERT INTO ar_ap_entry (id, doc_id, doc_no, doc_type, direction,"
                            " partner_id, partner_name, partner_key, biz_date, due_date,"
                            " amount_cents, settled_cents, status, created_at, created_by)"
                            " VALUES (?, 0, '迁移', 'OPENING_AR', 1, ?, '迁移客户甲', ?, ?, '',"
                            " 100, 0, 'OPEN', ?, 'test');");
          stmt.bind(1, xiaotu::next_id())
              .bind(2, cust_a.id)
              .bind(3, "C:" + std::to_string(cust_a.id))
              .bind(4, kToday)
              .bind(5, kToday + " 00:00:00")
              .run();
        });

        // 造数据阶段需要写审计，先用最新结构写完；随后回退审计列与 v35 预留列，模拟 v27 老库，
        // 这样迁移 v34 时历史审计行的来源 IP / 请求号会由默认值补成空串。
        mig.write_raw([&](sqlite3* conn) {
          const int rc =
              sqlite3_exec(conn,
                           "ALTER TABLE audit_log DROP COLUMN client_ip;"
                           "ALTER TABLE audit_log DROP COLUMN request_id;",
                           nullptr, nullptr, nullptr);
          check(rc == SQLITE_OK, "模拟库应能回退审计来源两列");
        });

        xiaotu::migrate(mig);
        check(mig.schema_version() == 40, "有数据的模拟库应迁到 v40");

        mig.read([&](sqlite3* conn) {
          xiaotu::Stmt count(conn, "SELECT COUNT(*) FROM partner;");
          check(count.step() && count.i64(0) == 4, "4 条客户/供应商应 1:1 搬入 partner");

          // 迁移前的历史审计行不应被伪造来源，两列一律为空串。
          xiaotu::Stmt legacy(conn,
                              "SELECT COUNT(*) FROM audit_log"
                              " WHERE client_ip <> '' OR request_id <> '';");
          check(legacy.step() && legacy.i64(0) == 0,
                "历史审计行的来源 IP 与请求号应为空串");

          xiaotu::Stmt cust(conn,
                            "SELECT code, name, contact, phone, address, tax_no,"
                            " is_customer, is_supplier, credit_limit_cents, credit_days,"
                            " merged_into FROM partner WHERE id = ?;");
          cust.bind(1, cust_a.id);
          check(cust.step(), "客户应保留原 id 作为 partner.id");
          check(cust.text(0) == "C001" && cust.text(1) == "迁移客户甲" &&
                    cust.text(2) == "张三" && cust.text(3) == "13800000001" &&
                    cust.text(4) == "甲地址" && cust.text(5) == "TAX-C001",
                "客户字段应原样搬迁");
          check(cust.i64(6) == 1 && cust.i64(7) == 0 && cust.i64(8) == 500000 &&
                    cust.i64(9) == 0 && cust.i64(10) == 0,
                "客户角色位、信用额度应正确，credit_days 与 merged_into 为 0");

          const std::int64_t base = cust_a.id > cust_b.id ? cust_a.id : cust_b.id;
          xiaotu::Stmt sup(conn,
                           "SELECT id, code, is_customer, is_supplier, tax_no"
                           " FROM partner WHERE name = '迁移供应商甲';");
          check(sup.step(), "供应商应搬入 partner");
          check(sup.i64(0) == base + 1,
                "供应商新 id 应接在最大客户 id 之后按 supplier.id 递增");
          check(sup.text(1) == "S001" && sup.i64(2) == 0 && sup.i64(3) == 1 &&
                    sup.text(4) == "TAX-S001",
                "供应商角色位、税号应正确");

          xiaotu::Stmt conflict(conn, "SELECT code FROM partner WHERE name = '迁移供应商乙';");
          check(conflict.step() && conflict.text(0) == "C001-S" + std::to_string(sup_b.id),
                "供应商编码与客户冲突时应追加 -S{legacy_id}");

          xiaotu::Stmt map(conn, "SELECT COUNT(*) FROM partner_legacy_map;");
          check(map.step() && map.i64(0) == 4, "映射表应覆盖客户与供应商各 2 条");
          xiaotu::Stmt sup_map(conn,
                               "SELECT partner_id FROM partner_legacy_map"
                               " WHERE side = 'supplier' AND legacy_id = ?;");
          sup_map.bind(1, sup_a.id);
          check(sup_map.step() && sup_map.i64(0) == base + 1,
                "映射表的供应商旧 id 应指向新 partner id");
          xiaotu::Stmt cust_map(conn,
                                "SELECT partner_id FROM partner_legacy_map"
                                " WHERE side = 'customer' AND legacy_id = ?;");
          cust_map.bind(1, cust_a.id);
          check(cust_map.step() && cust_map.i64(0) == cust_a.id,
                "映射表的客户旧 id 应与 partner id 相同");

          xiaotu::Stmt key(conn, "SELECT partner_key FROM ar_ap_entry WHERE doc_no = '迁移';");
          check(key.step() && key.text(0) == "C:" + std::to_string(cust_a.id),
                "本轮不改 partner_key，C: 键应保持原样");
        });

        // 解析侧兼容 P: 键：老键行为不变，P: 按角色位 / 台账方向判定。
        const std::int64_t customer_max = cust_a.id > cust_b.id ? cust_a.id : cust_b.id;
        const std::int64_t supplier_partner = customer_max + 1;  // 供应商甲的新 partner id
        const std::int64_t both_partner = customer_max + 5000;
        const std::string cust_key = "C:" + std::to_string(cust_a.id);
        const std::string sup_key = "S:" + std::to_string(sup_a.id);
        const std::string cust_p_key = "P:" + std::to_string(cust_a.id);
        const std::string sup_p_key = "P:" + std::to_string(supplier_partner);
        const std::string both_p_key = "P:" + std::to_string(both_partner);

        mig.write([&](sqlite3* conn) {
          xiaotu::Stmt both(conn,
                            "INSERT INTO partner (id, code, name, contact, phone, address,"
                            " tax_no, is_customer, is_supplier, credit_limit_cents, credit_days,"
                            " merged_into, status, is_deleted, created_at, created_by,"
                            " updated_at, updated_by) VALUES (?, 'BOTH', '兼买卖单位', '', '', '',"
                            " '', 1, 1, 0, 0, 0, 1, 0, '2026-11-01 09:00:00', 'test',"
                            " '2026-11-01 09:00:00', 'test');");
          both.bind(1, both_partner);
          both.run();
          const auto add_entry = [&](const std::string& key, int direction,
                                     std::int64_t amount, const std::string& name,
                                     std::int64_t partner_id = 0) {
            xiaotu::Stmt stmt(conn,
                              "INSERT INTO ar_ap_entry (id, doc_id, doc_no, doc_type, direction,"
                              " partner_id, partner_name, partner_key, biz_date, due_date,"
                              " amount_cents, settled_cents, status, created_at, created_by)"
                              " VALUES (?, 0, '解析', 'OPENING_AR', ?, ?, ?, ?, ?, '', ?, 0,"
                              " 'OPEN', ?, 'test');");
            stmt.bind(1, xiaotu::next_id())
                .bind(2, static_cast<std::int64_t>(direction))
                .bind(3, partner_id)
                .bind(4, name)
                .bind(5, key)
                .bind(6, kToday)
                .bind(7, amount)
                .bind(8, kToday + " 00:00:00")
                .run();
          };
          add_entry(cust_key, 1, 300, "老客户应收", cust_a.id);
          add_entry(sup_key, -1, 200, "老供应商应付", sup_a.id);
          add_entry("T:老文本单位", 1, 100, "老文本单位");
          add_entry(cust_p_key, 1, 1000, "迁移客户甲", cust_a.id);
          add_entry(sup_p_key, -1, 800, "迁移供应商甲", supplier_partner);
          add_entry(both_p_key, -1, 500, "兼买卖单位", both_partner);
        });

        mig.read([&](sqlite3* conn) {
          check(xiaotu::partner_key_direction(conn, cust_key) == 1,
                "C: 键老数据仍判应收");
          check(xiaotu::partner_key_direction(conn, sup_key) == -1,
                "S: 键老数据仍判应付");
          check(xiaotu::partner_key_direction(conn, "T:老文本单位") == 1,
                "T: 键老数据仍判应收");
          check(xiaotu::partner_key_direction(conn, cust_p_key) == 1,
                "P: 客户档案应判应收");
          check(xiaotu::partner_key_direction(conn, sup_p_key) == -1,
                "P: 供应商档案应判应付");
          check(xiaotu::partner_key_direction(conn, both_p_key) == -1,
                "兼买卖单位台账只有应付时应按台账判应付");
          check(xiaotu::partner_key_name(conn, cust_p_key) == "迁移客户甲" &&
                    xiaotu::partner_key_name(conn, sup_p_key) == "迁移供应商甲",
                "P: 键应能解析出 partner 名称");
          check(xiaotu::partner_key_name(conn, "P:99999999").empty(),
                "不存在的 partner 不应命中档案");
        });

        // 账龄：P: 键按台账方向归集，且不影响 C:/S:/T: 的旧口径。
        xiaotu::ReportService mig_reports(mig);
        const auto aging_all = mig_reports.ar_ap_aging(0, kToday);
        bool aging_p_ar = false, aging_p_ap = false, aging_c_split = false;
        for (const auto& row : aging_all) {
          if (row.partner_key == cust_p_key && row.direction == 1 &&
              row.total_cents == 1400) {
            aging_p_ar = true;
          }
          if (row.partner_key == sup_p_key && row.direction == -1 &&
              row.total_cents == 1000) {
            aging_p_ap = true;
          }
          if (row.partner_key == cust_key) aging_c_split = true;
        }
        check(aging_p_ar && aging_p_ap,
              "账龄应把历史 C: 与新 P: 合成一行，金额相加正确");
        check(!aging_c_split, "同一单位不应再出现独立的 C: 行");
        const auto aging_ap_only = mig_reports.ar_ap_aging(-1, kToday);
        bool ap_only_ok = !aging_ap_only.empty();
        for (const auto& row : aging_ap_only) {
          if (row.direction != -1) ap_only_ok = false;
        }
        check(ap_only_ok, "账龄按应付筛选时应只返回 direction=-1 的行");

        // 对账单：P: 键应从 partner 解析名称，历史 C: 与新 P: 合并取数。
        const auto statement = mig_reports.partner_statement(cust_p_key, 1, "", "");
        check(statement.partner_name == "迁移客户甲",
              "P: 对账单应从 partner 解析名称，而不是落到无档案");
        check(statement.increase_cents == 1400,
              "P: 对账单应收发生额应合并历史 C: 与新 P: 记录");

        // 核销：P: 键在应收/应付记录里要能解析出档案名称。
        xiaotu::FinanceService mig_finance(mig);
        const auto finance_p = mig_finance.entries(0, both_p_key, "");
        check(finance_p.size() == 1 && finance_p.front().direction == -1,
              "台账按 P: 键筛选应返回该单位的应付记录");

        xiaotu::SettlementService mig_settlements(mig);
        xiaotu::SettlementCreateInput settle_input;
        settle_input.direction = -1;
        settle_input.partner_key = sup_p_key;
        settle_input.amount_cents = 100;
        settle_input.account_id = 1;
        settle_input.method = "CASH";
        settle_input.operator_name = "test";
        const auto settlement = mig_settlements.create(settle_input);
        check(settlement.partner_name == "迁移供应商甲",
              "核销 P: 键时应从 partner 解析往来单位名称");

        // 欠款 Top N：同一单位的历史 C: 与新 P: 应合成一行。
        const auto dashboard = mig_reports.dashboard(kToday);
        int debtor_rows = 0;
        std::int64_t debtor_cents = 0;
        for (const auto& debtor : dashboard.top_debtors) {
          if (debtor.partner_id == cust_a.id) {
            ++debtor_rows;
            debtor_cents = debtor.outstanding_cents;
          }
        }
        check(debtor_rows == 1 && debtor_cents == 1400,
              "欠款 Top N 应把同一单位的 C:/P: 合并为一行且金额相加正确");

        // 收付款明细：按 P: 过滤应能看到该单位历史 C: 的收付款。
        xiaotu::SettlementCreateInput prepay;
        prepay.direction = 1;
        prepay.partner_key = cust_key;
        prepay.amount_cents = 50;
        prepay.account_id = 1;
        prepay.method = "CASH";
        prepay.operator_name = "test";
        mig_settlements.create(prepay);
        bool found_historical_settle = false;
        for (const auto& row : mig_settlements.list(1, cust_p_key)) {
          if (row.partner_key == cust_key && row.amount_cents == 50) {
            found_historical_settle = true;
          }
        }
        check(found_historical_settle, "按 P: 查询收付款明细应包含历史 C: 记录");

        // T: 键保持按名称独立，不并入同名的 P: 行。
        bool t_independent = false;
        for (const auto& row : aging_all) {
          if (row.partner_key == "T:老文本单位" && row.direction == 1 &&
              row.total_cents == 100) {
            t_independent = true;
          }
        }
        check(t_independent, "T: 键应保持按名称独立");

        // 档案不存在时不产出悬空 P:，退回 T: 文本键。
        mig.read([&](sqlite3* conn) {
          check(xiaotu::partner_key_for_archive(conn, xiaotu::PartnerSide::Supplier, 987654321,
                                                "无档案单位", true) == "T:无档案单位",
                "档案不存在时应回落到 T: 文本键");
        });

        // 兼买卖单位补一条应收后，方向以台账两边都有为准，默认应收。
        mig.write([&](sqlite3* conn) {
          xiaotu::Stmt stmt(conn,
                            "INSERT INTO ar_ap_entry (id, doc_id, doc_no, doc_type, direction,"
                            " partner_id, partner_name, partner_key, biz_date, due_date,"
                            " amount_cents, settled_cents, status, created_at, created_by)"
                            " VALUES (?, 0, '解析', 'OPENING_AR', 1, 0, '兼买卖单位', ?, ?, '',"
                            " 200, 0, 'OPEN', ?, 'test');");
          stmt.bind(1, xiaotu::next_id())
              .bind(2, both_p_key)
              .bind(3, kToday)
              .bind(4, kToday + " 00:00:00")
              .run();
        });
        mig.read([&](sqlite3* conn) {
          check(xiaotu::partner_key_direction(conn, both_p_key) == 1,
                "兼买卖单位台账两边都有时默认应收，不按角色硬推");
        });

        // 来源标记：历史迁移默认 MANUAL，首次引用补建 AUTO_ARCHIVE，归并预览能区分。
        mig.read([&](sqlite3* conn) {
          xiaotu::Stmt origins(conn, "SELECT COUNT(*) FROM partner WHERE origin <> 'MANUAL';");
          check(origins.step() && origins.i64(0) == 0, "历史迁移记录应默认为 MANUAL");
        });
        const auto auto_customer = mig_customers.create(
            xiaotu::gen::CustomerRow{.code = "SRC-A", .name = "来源标记单位"});
        std::int64_t auto_partner = 0;
        mig.write([&](sqlite3* conn) {
          auto_partner = xiaotu::ensure_partner_for_archive(
              conn, xiaotu::PartnerSide::Customer, auto_customer.id);
          check(auto_partner == auto_customer.id, "自动补建客户应沿用档案 id");
        });
        mig.read([&](sqlite3* conn) {
          xiaotu::Stmt origin(conn, "SELECT origin FROM partner WHERE id = ?;");
          origin.bind(1, auto_partner);
          check(origin.step() && origin.text(0) == "AUTO_ARCHIVE",
                "首次引用补建的 partner 应标记 AUTO_ARCHIVE");
        });

        const std::int64_t manual_partner =
            (customer_max > auto_customer.id ? customer_max : auto_customer.id) + 9000;
        mig.write([&](sqlite3* conn) {
          // 手工创建的 partner 不写 origin，走默认 MANUAL。
          xiaotu::Stmt partner(conn,
                               "INSERT INTO partner (id, code, name, contact, phone, address,"
                               " tax_no, is_customer, is_supplier, credit_limit_cents,"
                               " credit_days, merged_into, status, is_deleted, created_at,"
                               " created_by, updated_at, updated_by) VALUES (?, 'SRC-M',"
                               " '来源标记单位', '', '', '', '', 1, 0, 0, 0, 0, 1, 0,"
                               " '2026-11-01 09:00:00', 'test', '2026-11-01 09:00:00', 'test');");
          partner.bind(1, manual_partner);
          partner.run();
          xiaotu::Stmt ledger(conn,
                              "INSERT INTO ar_ap_entry (id, doc_id, doc_no, doc_type, direction,"
                              " partner_id, partner_name, partner_key, biz_date, due_date,"
                              " amount_cents, settled_cents, status, created_at, created_by)"
                              " VALUES (?, 0, '来源', 'OPENING_AR', 1, ?, '来源标记单位', ?, ?, '',"
                              " 77, 0, 'OPEN', ?, 'test');");
          ledger.bind(1, xiaotu::next_id())
              .bind(2, manual_partner)
              .bind(3, "P:" + std::to_string(manual_partner))
              .bind(4, kToday)
              .bind(5, kToday + " 00:00:00")
              .run();
        });
        const auto origin_preview = xiaotu::partner_merge_preview(mig);
        bool saw_auto = false, saw_manual = false;
        for (const auto& group : origin_preview.groups) {
          for (const auto& row : group.rows) {
            if (row.origin == "AUTO_ARCHIVE") saw_auto = true;
            if (row.origin == "MANUAL") saw_manual = true;
          }
        }
        check(saw_auto && saw_manual, "归并预览应能区分自动补建与手工记录");
        const std::string origin_text = xiaotu::partner_preview_text(origin_preview);
        check(origin_text.find("自动补建") != std::string::npos &&
                  origin_text.find("手工/合并") != std::string::npos,
              "预览文本应显示来源标记");
        const std::string origin_json = xiaotu::partner_preview_json(origin_preview);
        check(origin_json.find("\"origin\":\"AUTO_ARCHIVE\"") != std::string::npos &&
                  origin_json.find("\"origin\":\"MANUAL\"") != std::string::npos,
              "预览 JSON 应输出来源标记");

        // 供应商合并：沿用 partner_legacy_map 的 S: 历史键，合并后账龄归到保留档案。
        std::int64_t sup_b_partner = 0;
        mig.read([&](sqlite3* conn) {
          xiaotu::Stmt stmt(conn,
                            "SELECT partner_id FROM partner_legacy_map"
                            " WHERE side = 'supplier' AND legacy_id = ?;");
          stmt.bind(1, sup_b.id);
          if (stmt.step()) sup_b_partner = stmt.i64(0);
        });
        check(sup_b_partner > 0, "供应商乙应有 partner 映射");
        mig.write([&](sqlite3* conn) {
          xiaotu::Stmt stmt(conn,
                            "INSERT INTO ar_ap_entry (id, doc_id, doc_no, doc_type, direction,"
                            " partner_id, partner_name, partner_key, biz_date, due_date,"
                            " amount_cents, settled_cents, status, created_at, created_by)"
                            " VALUES (?, 0, '供应商合并前', 'OPENING_AP', -1, ?, '迁移供应商乙',"
                            " ?, ?, '', 300, 0, 'OPEN', ?, 'test');");
          stmt.bind(1, xiaotu::next_id())
              .bind(2, sup_b.id)
              .bind(3, "S:" + std::to_string(sup_b.id))
              .bind(4, kToday)
              .bind(5, kToday + " 00:00:00")
              .run();
        });
        const auto sup_merged =
            xiaotu::partner_merge(mig, supplier_partner, {sup_b_partner}, "test");
        check(sup_merged.target_id == supplier_partner, "供应商合并应保留指定档案");
        const auto sup_aging = mig_reports.ar_ap_aging(-1, kToday);
        std::int64_t sup_target_total = 0;
        bool sup_source_split = false;
        for (const auto& row : sup_aging) {
          if (row.partner_key == sup_p_key) sup_target_total = row.total_cents;
          if (row.partner_key == "S:" + std::to_string(sup_b.id)) sup_source_split = true;
        }
        check(sup_target_total == 1300 && !sup_source_split,
              "供应商合并后 S: 历史键应归到保留档案一行");
        const auto partner_list = xiaotu::partner_rows(mig);
        bool supplier_ids_ok = false;
        for (const auto& row : partner_list) {
          if (row.id == sup_b_partner) {
            supplier_ids_ok = row.merged_into == supplier_partner &&
                              std::find(row.supplier_ids.begin(), row.supplier_ids.end(),
                                        sup_b.id) != row.supplier_ids.end();
          }
        }
        check(supplier_ids_ok, "合并后的 partner 清单应带出供应商旧 id 供界面隐藏");
        xiaotu::partner_merge_undo(mig, sup_merged.log_id, "test");
        const auto sup_aging_undone = mig_reports.ar_ap_aging(-1, kToday);
        std::int64_t sup_target_undone = 0;
        std::int64_t sup_source_undone = 0;
        for (const auto& row : sup_aging_undone) {
          if (row.partner_key == sup_p_key) sup_target_undone = row.total_cents;
          if (row.partner_key == "P:" + std::to_string(sup_b_partner)) {
            sup_source_undone = row.total_cents;
          }
        }
        check(sup_target_undone == 1000 && sup_source_undone == 300,
              "撤销供应商合并后账龄应回到两条档案");

        // 合并动作：两条客户档案合并后，台账、账龄与对账单归到保留档案一行且金额相加。
        mig.write([&](sqlite3* conn) {
          xiaotu::Stmt stmt(conn,
                            "INSERT INTO ar_ap_entry (id, doc_id, doc_no, doc_type, direction,"
                            " partner_id, partner_name, partner_key, biz_date, due_date,"
                            " amount_cents, settled_cents, status, created_at, created_by)"
                            " VALUES (?, 0, '合并前', 'OPENING_AR', 1, ?, '迁移客户乙', ?, ?, '',"
                            " 600, 0, 'OPEN', ?, 'test');");
          stmt.bind(1, xiaotu::next_id())
              .bind(2, cust_b.id)
              .bind(3, "C:" + std::to_string(cust_b.id))
              .bind(4, kToday)
              .bind(5, kToday + " 00:00:00")
              .run();
        });
        const auto aging_before_merge = mig_reports.ar_ap_aging(0, kToday);
        std::int64_t before_target = 0;
        std::int64_t before_source = 0;
        for (const auto& row : aging_before_merge) {
          if (row.partner_key == "P:" + std::to_string(cust_a.id)) before_target = row.total_cents;
          if (row.partner_key == "P:" + std::to_string(cust_b.id)) before_source = row.total_cents;
        }
        check(before_target == 1400 && before_source == 600,
              "合并前两条档案应各有一行账龄");

        const auto merged = xiaotu::partner_merge(
            mig, cust_a.id, {static_cast<std::int64_t>(cust_b.id)}, "test");
        check(merged.log_id > 0 && merged.target_id == cust_a.id,
              "合并应写入日志并返回保留档案");
        mig.read([&](sqlite3* conn) {
          xiaotu::Stmt source(conn, "SELECT merged_into FROM partner WHERE id = ?;");
          source.bind(1, cust_b.id);
          check(source.step() && source.i64(0) == cust_a.id,
                "被合并档案的 merged_into 应指向保留档案");
          xiaotu::Stmt audit(conn,
                             "SELECT COUNT(*) FROM audit_log WHERE action = 'partner_merge'"
                             " AND entity_id = ?;");
          audit.bind(1, cust_a.id);
          check(audit.step() && audit.i64(0) == 1, "合并动作应写审计日志");
          xiaotu::Stmt log(conn,
                           "SELECT target_snapshot, sources_snapshot, source_keys, moved_maps"
                           " FROM partner_merge_log WHERE id = ?;");
          log.bind(1, merged.log_id);
          check(log.step() && log.text(0).find("\"id\"") != std::string::npos &&
                    log.text(1).find(std::to_string(cust_b.id)) != std::string::npos &&
                    log.text(2).find("\"C:" + std::to_string(cust_b.id)) != std::string::npos,
                "合并日志应保存前后快照与源档案历史键");
        });

        const auto aging_merged = mig_reports.ar_ap_aging(0, kToday);
        bool merged_single_row = false;
        bool merged_source_split = false;
        for (const auto& row : aging_merged) {
          if (row.partner_key == "P:" + std::to_string(cust_a.id) &&
              row.total_cents == 2000) {
            merged_single_row = true;
          }
          if (row.partner_key == "P:" + std::to_string(cust_b.id) ||
              row.partner_key == "C:" + std::to_string(cust_b.id)) {
            merged_source_split = true;
          }
        }
        check(merged_single_row && !merged_source_split,
              "合并后账龄应只剩保留档案一行，金额为两条之和");
        const auto merged_statement = mig_reports.partner_statement(
            "P:" + std::to_string(cust_a.id), 1, "", "");
        check(merged_statement.partner_name == "迁移客户甲" &&
                  merged_statement.increase_cents == 2000,
              "合并后对账单应使用保留档案名称并合并发生额");

        // 撤销：没有新业务写入，完整回到合并前两条档案。
        xiaotu::partner_merge_undo(mig, merged.log_id, "test");
        mig.read([&](sqlite3* conn) {
          xiaotu::Stmt target(conn, "SELECT merged_into FROM partner WHERE id = ?;");
          target.bind(1, cust_a.id);
          check(target.step() && target.i64(0) == 0, "撤销后保留档案 merged_into 应清零");
          xiaotu::Stmt source(conn, "SELECT merged_into FROM partner WHERE id = ?;");
          source.bind(1, cust_b.id);
          check(source.step() && source.i64(0) == 0, "撤销后被合并档案应恢复独立");
          xiaotu::Stmt audit(conn,
                             "SELECT COUNT(*) FROM audit_log"
                             " WHERE action = 'partner_merge_undo' AND entity_id = ?;");
          audit.bind(1, cust_a.id);
          check(audit.step() && audit.i64(0) == 1, "撤销动作应写审计日志");
        });
        const auto aging_undone = mig_reports.ar_ap_aging(0, kToday);
        std::int64_t undone_target = 0;
        std::int64_t undone_source = 0;
        for (const auto& row : aging_undone) {
          if (row.partner_key == "P:" + std::to_string(cust_a.id)) undone_target = row.total_cents;
          if (row.partner_key == "P:" + std::to_string(cust_b.id)) undone_source = row.total_cents;
        }
        check(undone_target == 1400 && undone_source == 600,
              "撤销后账龄应回到合并前两条档案");

        // 合并后新写业务：撤销被拒绝且不做半截撤销。
        const auto merged_again = xiaotu::partner_merge(
            mig, cust_a.id, {static_cast<std::int64_t>(cust_b.id)}, "test");
        mig.write([&](sqlite3* conn) {
          xiaotu::Stmt stmt(conn,
                            "INSERT INTO ar_ap_entry (id, doc_id, doc_no, doc_type, direction,"
                            " partner_id, partner_name, partner_key, biz_date, due_date,"
                            " amount_cents, settled_cents, status, created_at, created_by)"
                            " VALUES (?, 0, '合并后', 'OPENING_AR', 1, ?, '迁移客户乙', ?, ?, '',"
                            " 900, 0, 'OPEN', ?, 'test');");
          stmt.bind(1, xiaotu::next_id())
              .bind(2, cust_b.id)
              .bind(3, "P:" + std::to_string(cust_b.id))
              .bind(4, kToday)
              .bind(5, kToday + " 00:00:00")
              .run();
        });
        const std::string undo_blocker =
            xiaotu::partner_merge_undo_blocker(mig, merged_again.log_id);
        check(undo_blocker.find("新业务") != std::string::npos,
              "合并后有新业务时应给出撤销拒绝原因");
        bool undo_rejected = false;
        try {
          xiaotu::partner_merge_undo(mig, merged_again.log_id, "test");
        } catch (const xiaotu::AppError& error) {
          undo_rejected = error.code() == "MERGE_UNDO_BLOCKED";
        }
        check(undo_rejected, "合并后新写业务再撤销应被拒绝");
        mig.read([&](sqlite3* conn) {
          xiaotu::Stmt source(conn, "SELECT merged_into FROM partner WHERE id = ?;");
          source.bind(1, cust_b.id);
          check(source.step() && source.i64(0) == cust_a.id,
                "撤销被拒绝时不应做半截还原");
          xiaotu::Stmt log(conn, "SELECT undone_at FROM partner_merge_log WHERE id = ?;");
          log.bind(1, merged_again.log_id);
          check(log.step() && log.text(0).empty(), "被拒绝的撤销不应把日志标记为已撤销");
          xiaotu::Stmt audit(conn,
                             "SELECT COUNT(*) FROM audit_log WHERE action = 'partner_merge';");
          check(audit.step() && audit.i64(0) == 3, "每次合并都应独立写审计日志");
        });
      }

      // 单据列表统一筛选 / 分页 / 合计：独立库，日期与金额都可控。
      {
        const fs::path list_dir = dir / "doc-list";
        fs::create_directories(list_dir);
        xiaotu::Db ldb(list_dir / "app.db", 2);
        xiaotu::migrate(ldb);
        check(ldb.schema_version() == 40, "列表筛选库应迁到 v40");
        // 幂等：每次启动都会跑 migrate，已应用的 v40 只能跳过。
        xiaotu::migrate(ldb);
        check(ldb.schema_version() == 40, "重复迁移应保持 v40，不重复建 token 表");
        ldb.read([&](sqlite3* conn) {
          bool has_type_date = false;
          bool has_type_status = false;
          // 注：读连接在迁移前已打开，先做一次普通查询让连接加载最新 schema；
          // 内嵌 SQLite 的 PRAGMA index_list 不会自己触发这次加载。
          {
            xiaotu::Stmt warmup(conn, "SELECT COUNT(*) FROM doc_header;");
            warmup.step();
          }
          xiaotu::Stmt indexes(conn, "PRAGMA index_list('doc_header');");
          while (indexes.step()) {
            const std::string name = indexes.text(1);
            if (name == "idx_doc_header_type_date") has_type_date = true;
            if (name == "idx_doc_header_type_status") has_type_status = true;
          }
          check(has_type_date && has_type_status,
                "v37 后 doc_header 应有类型+日期与类型+状态索引");
          // v38：备注列存在、NOT NULL、默认空串；老库升级后历史行不应出现 NULL。
          xiaotu::Stmt remark_col(
              conn,
              "SELECT type, \"notnull\", dflt_value FROM pragma_table_info('doc_header')"
              " WHERE name = 'remark';");
          check(remark_col.step() && remark_col.text(0) == "TEXT" && remark_col.i64(1) == 1 &&
                    remark_col.text(2) == "''",
                "v38 后 doc_header.remark 应为 TEXT NOT NULL DEFAULT ''");
          xiaotu::Stmt remark_null(conn, "SELECT COUNT(*) FROM doc_header WHERE remark IS NULL;");
          check(remark_null.step() && remark_null.i64(0) == 0,
                "v38 后历史单据备注应为空串而不是 NULL");
        });

        // 老数据默认空：按老列写入（不带 remark）的历史行应自动落空串。
        ldb.write([&](sqlite3* conn) {
          xiaotu::Stmt legacy(
              conn,
              "INSERT INTO doc_header (id, doc_no, doc_type, doc_date, warehouse_id, status,"
              " created_at)"
              " VALUES (?, 'OLD-REMARK-1', 'PUR_IN', '2026-01-01', 1, 'APPROVED',"
              " '2026-01-01 00:00:00');");
          legacy.bind(1, xiaotu::next_id()).run();
        });
        ldb.read([&](sqlite3* conn) {
          xiaotu::Stmt old(conn,
                           "SELECT remark FROM doc_header WHERE doc_no = 'OLD-REMARK-1';");
          check(old.step() && old.text(0).empty(), "按老列写入的历史行备注应为空串");
        });

        xiaotu::gen::ItemService list_items(ldb);
        xiaotu::gen::WarehouseService list_warehouses(ldb);
        xiaotu::gen::EmployeeService list_employees(ldb);
        xiaotu::DocumentEngine engine(ldb);
        const auto list_item =
            list_items.create(xiaotu::gen::ItemRow{.code = "L001", .name = "列表商品"});
        const std::int64_t list_warehouse = list_warehouses.list().at(0).id;
        const auto zhang =
            list_employees.create(xiaotu::gen::EmployeeRow{.code = "L-E1", .name = "张三"});
        const auto li =
            list_employees.create(xiaotu::gen::EmployeeRow{.code = "L-E2", .name = "李四"});

        const auto make_sale = [&](const std::string& date, const std::string& customer,
                                   std::int64_t salesperson_id, std::int64_t qty,
                                   std::int64_t price_cents, std::string remark = "") {
          xiaotu::DocInput sale;
          sale.doc_type = "SAL_OUT";
          sale.warehouse_id = list_warehouse;
          sale.doc_date = date;
          sale.partner = customer;
          sale.salesperson_id = salesperson_id;
          sale.remark = std::move(remark);
          sale.lines.push_back(
              xiaotu::DocLineInput{.item_id = list_item.id, .qty = q(qty),
                                   .price_cents = price_cents});
          return engine.save_and_approve(sale);
        };

        xiaotu::DocInput stock_in;
        stock_in.doc_type = "PUR_IN";
        stock_in.warehouse_id = list_warehouse;
        stock_in.doc_date = "2026-01-01";
        stock_in.partner = "列表供应商";
        stock_in.lines.push_back(
            xiaotu::DocLineInput{.item_id = list_item.id, .qty = q(100),
                                 .price_cents = 100});
        engine.save_and_approve(stock_in);

        const auto jan = make_sale("2026-01-05", "客户甲", zhang.id, 2, 1234, "加急单");  // 2468
        const auto feb = make_sale("2026-02-10", "客户乙", li.id, 1, 5000);    // 5000
        const auto mar = make_sale("2026-03-01", "客户甲", zhang.id, 1, 800);  // 800
        engine.reverse(mar.id);
        const auto percent = make_sale("2026-02-12", "折扣%客户", 0, 1, 900);  // 900
        const auto underscore = make_sale("2026-02-13", "客户_特殊", 0, 1, 700);

        xiaotu::DocInput list_draft;
        list_draft.doc_type = "SAL_OUT";
        list_draft.warehouse_id = list_warehouse;
        list_draft.doc_date = "2026-02-11";
        list_draft.partner = "客户甲";
        list_draft.salesperson_id = zhang.id;
        list_draft.lines.push_back(
            xiaotu::DocLineInput{.item_id = list_item.id, .qty = q(1), .price_cents = 1500});
        const auto draft = engine.save_draft(list_draft);

        const auto query = [&](std::string keyword, std::string from, std::string to,
                               std::string status, std::int64_t min_cents,
                               std::int64_t max_cents, int page, int page_size) {
          return engine.list_page(xiaotu::DocListQuery{
              .doc_type = "SAL_OUT",
              .keyword = std::move(keyword),
              .date_from = std::move(from),
              .date_to = std::move(to),
              .status = std::move(status),
              .amount_min_cents = min_cents,
              .amount_max_cents = max_cents,
              .page = page,
              .page_size = page_size,
          });
        };

        check(query("", "", "", "", 0, 0, 1, 20).total_count == 6,
              "SAL_OUT 共 6 张（含草稿与已反审核）");
        check(query("客户甲", "", "", "", 0, 0, 1, 20).total_count == 3,
              "关键字应匹配往来单位");
        check(query("张三", "", "", "", 0, 0, 1, 20).total_count == 3,
              "关键字应匹配业务员名称快照");
        check(query("加急", "", "", "", 0, 0, 1, 20).total_count == 1,
              "关键字应命中表头备注");
        check(query("加急", "", "", "", 0, 0, 1, 20).rows.at(0).remark == "加急单",
              "列表行应带回表头备注");
        check(query("客户", "2026-02-01", "2026-02-28", "", 0, 0, 1, 20).total_count == 4,
              "日期闭区间内命中四张客户名称含「客户」的销售单");
        check(query(jan.doc_no, "", "", "", 0, 0, 1, 20).total_count == 1,
              "关键字应能按完整单号精确命中");
        check(query("", "2026-02-01", "2026-03-31", "", 0, 0, 1, 20).total_count == 5,
              "日期闭区间应包含 2 月与 3 月的五张单据");
        check(query("", "", "", "draft", 0, 0, 1, 20).total_count == 1,
              "draft 应只返回草稿");
        check(query("", "", "", "approved", 0, 0, 1, 20).total_count == 4,
              "approved 应只返回已审核未反审核的单据");
        check(query("", "", "", "reversed", 0, 0, 1, 20).total_count == 1,
              "reversed 应映射到 VOIDED");
        check(query("", "", "", "all", 0, 0, 1, 20).total_count == 6,
              "status=all 应不过滤");
        check(query("", "", "", "", 2000, 3000, 1, 20).total_count == 1,
              "金额闭区间应只命中 2468 分的单据");
        check(query("客户甲", "2026-02-01", "2026-02-28", "draft", 1000, 2000, 1, 20)
                      .total_count == 1,
              "组合条件应同时生效");
        // LIKE 特殊字符按字面匹配，不能退化成全表匹配。
        check(query("%", "", "", "", 0, 0, 1, 20).total_count == 1,
              "关键字 % 应只匹配名称里真的含 % 的单据");
        check(query("_", "", "", "", 0, 0, 1, 20).total_count == 1,
              "关键字 _ 应只匹配名称里真的含 _ 的单据");

        const auto first_page = query("", "", "", "", 0, 0, 1, 2);
        const auto second_page = query("", "", "", "", 0, 0, 2, 2);
        const auto third_page = query("", "", "", "", 0, 0, 3, 2);
        check(first_page.rows.size() == 2 && second_page.rows.size() == 2 &&
                  third_page.rows.size() == 2,
              "page_size=2 时每页应为 2 行");
        check(first_page.total_count == 6 && second_page.total_count == 6 &&
                  third_page.total_count == 6,
              "total_count 不应随 offset 变化");
        check(first_page.total_amount_cents == 2468 + 5000 + 800 + 900 + 700 + 1500,
              "合计金额应为筛选结果逐行之和");
        check(first_page.total_amount_cents == second_page.total_amount_cents &&
                  second_page.total_amount_cents == third_page.total_amount_cents,
              "合计金额不应随分页变化");
        bool descending = first_page.rows.at(0).id > first_page.rows.at(1).id &&
                          second_page.rows.at(0).id > second_page.rows.at(1).id;
        check(descending, "分页按 id 倒序输出");
        check(query("", "2026-02-01", "2026-02-28", "", 0, 0, 1, 1000).page_size == 100,
              "page_size 超过 100 应截断为 100");
        check(query("", "", "", "", 0, 0, 0, 0).page == 1 &&
                  query("", "", "", "", 0, 0, 0, 0).page_size == 20,
              "page/page_size 非法值应回到 1 / 20");
        check(draft.id > 0 && feb.id > jan.id && percent.id > 0 && underscore.id > 0,
              "测试单据应全部落库");
        (void)percent;
        (void)underscore;
      }

      // v40：零金额流水回归。单价 0 的赠品入库、零成本库存的销售出库
      // （移动加权成本为 0）都应能正常审核；数量与金额同时为 0 仍被拒绝。
      {
        const fs::path zero_dir = dir / "zero-amount";
        fs::create_directories(zero_dir);
        xiaotu::Db zdb(zero_dir / "app.db", 2);
        xiaotu::migrate(zdb);
        check(zdb.schema_version() == 40, "零金额库应迁到 v40");

        xiaotu::gen::ItemService z_items(zdb);
        xiaotu::gen::WarehouseService z_warehouses(zdb);
        xiaotu::DocumentEngine z_docs(zdb);
        const auto gift_item =
            z_items.create(xiaotu::gen::ItemRow{.code = "Z001", .name = "赠品"});
        const std::int64_t z_wh = z_warehouses.list().at(0).id;

        xiaotu::DocInput gift_in;
        gift_in.doc_type = "OTH_IN";
        gift_in.biz_category = "GIFT";
        gift_in.warehouse_id = z_wh;
        gift_in.lines.push_back(
            xiaotu::DocLineInput{.item_id = gift_item.id, .qty = q(3), .price_cents = 0});
        const auto gift_doc = z_docs.save_and_approve(gift_in);
        check(gift_doc.id > 0, "单价 0 的赠品入库应能审核");

        xiaotu::DocInput zero_cost_sale;
        zero_cost_sale.doc_type = "SAL_OUT";
        zero_cost_sale.warehouse_id = z_wh;
        zero_cost_sale.partner = "散客";
        zero_cost_sale.lines.push_back(
            xiaotu::DocLineInput{.item_id = gift_item.id, .qty = q(1), .price_cents = 500});
        const auto sale_doc = z_docs.save_and_approve(zero_cost_sale);
        check(sale_doc.id > 0, "零成本库存的销售出库应能审核");

        zdb.read([&](sqlite3* conn) {
          xiaotu::Stmt ledger(conn,
                              "SELECT COUNT(*) FROM stock_ledger WHERE amount_cents = 0"
                              " AND qty <> 0;");
          check(ledger.step() && ledger.i64(0) >= 2,
                "赠品入库与零成本出库都应留下零金额数量流水");
        });

        // 新约束边界：qty=0 且金额=0 无意义，应被 CHECK 拒绝；
        // qty=0 且金额非 0（成本调整腿）应可写入。
        bool all_zero_rejected = false;
        zdb.write([&](sqlite3* conn) {
          xiaotu::Stmt bad(conn,
                           "INSERT INTO stock_ledger (id, doc_id, doc_line_id, biz_type,"
                           " biz_time, warehouse_id, item_id, qty, amount_cents, created_at)"
                           " VALUES (?, 0, ?, 'CST_ADJ', '2026-01-01 00:00:00', ?, ?, 0, 0,"
                           " '2026-01-01 00:00:00');");
          bad.bind(1, xiaotu::next_id())
              .bind(2, xiaotu::next_id())
              .bind(3, z_wh)
              .bind(4, gift_item.id);
          try {
            bad.run();
          } catch (const xiaotu::AppError&) {
            all_zero_rejected = true;
          }
        });
        check(all_zero_rejected, "数量与金额同时为 0 的流水应被拒绝");

        zdb.write([&](sqlite3* conn) {
          xiaotu::Stmt ok(conn,
                          "INSERT INTO stock_ledger (id, doc_id, doc_line_id, biz_type,"
                          " biz_time, warehouse_id, item_id, qty, amount_cents, created_at)"
                          " VALUES (?, 0, ?, 'CST_ADJ', '2026-01-01 00:00:00', ?, ?, 0, 5,"
                          " '2026-01-01 00:00:00');");
          ok.bind(1, xiaotu::next_id())
              .bind(2, xiaotu::next_id())
              .bind(3, z_wh)
              .bind(4, gift_item.id)
              .run();
        });
        zdb.read([&](sqlite3* conn) {
          xiaotu::Stmt kept(conn,
                           "SELECT COUNT(*) FROM stock_ledger WHERE qty = 0"
                           " AND amount_cents = 5;");
          check(kept.step() && kept.i64(0) == 1, "成本调整式零数量非零金额流水应保留");
        });
      }

      // C2：v12 及更早老库带零金额流水时，升级不得被 v13 的 CHECK 拦停。
      // 老库用 migrate_to 构造，违例流水走裸 SQL（v12 时代合法：旧 CHECK 只约束 qty）。
      {
        const fs::path legacy_dir = dir / "legacy-v12";
        fs::create_directories(legacy_dir);
        xiaotu::Db ldb(legacy_dir / "app.db", 2);
        xiaotu::migrate_to(ldb, 12);
        check(ldb.schema_version() == 12, "老库模拟应停在 v12");

        ldb.write([&](sqlite3* conn) {
          // v1 已预置 id=1 的默认仓库，老库只补商品与流水。
          xiaotu::Stmt it(conn,
                          "INSERT INTO item (id, code, name, created_at, updated_at)"
                          " VALUES (1, 'LI001', '老商品', '2026-01-01 00:00:00',"
                          " '2026-01-01 00:00:00');");
          it.run();
          xiaotu::Stmt lg1(conn,
                           "INSERT INTO stock_ledger (id, doc_id, doc_line_id, biz_type,"
                           " biz_time, warehouse_id, item_id, qty, amount_cents, created_at)"
                           " VALUES (1, 0, 1, 'OTH_IN', '2026-01-01 00:00:00', 1, 1, 10000, 0,"
                           " '2026-01-01 00:00:00');");
          lg1.run();
          // 盘点流水会被 v15 按方向改写 biz_type，违例行同样要能穿过这一步。
          xiaotu::Stmt lg2(conn,
                           "INSERT INTO stock_ledger (id, doc_id, doc_line_id, biz_type,"
                           " biz_time, warehouse_id, item_id, qty, amount_cents, created_at)"
                           " VALUES (2, 0, 2, 'STK_CNT', '2026-01-02 00:00:00', 1, 1, 5000, 0,"
                           " '2026-01-02 00:00:00');");
          lg2.run();
        });

        xiaotu::migrate(ldb);
        check(ldb.schema_version() == 40, "带零金额流水的老库应一路迁到 v40");

        ldb.read([&](sqlite3* conn) {
          xiaotu::Stmt kept(conn,
                            "SELECT biz_type, qty, amount_cents FROM stock_ledger"
                            " WHERE id = 1;");
          check(kept.step() && kept.text(0) == "OTH_IN" && kept.i64(1) == 10000 &&
                    kept.i64(2) == 0,
                "零金额入库流水应原样保留");
          xiaotu::Stmt cnt(conn, "SELECT biz_type FROM stock_ledger WHERE id = 2;");
          check(cnt.step() && cnt.text(0) == "STK_CNT_IN",
                "零金额盘点流水应穿过 v15 方向改写");
        });
        bool pragma_off = false;
        ldb.write([&](sqlite3* conn) {
          xiaotu::Stmt p(conn, "PRAGMA ignore_check_constraints;");
          pragma_off = p.step() && p.i64(0) == 0;
        });
        check(pragma_off, "升级完成后写连接应关闭约束忽略");
      }
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
