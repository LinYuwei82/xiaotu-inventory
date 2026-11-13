#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "xiaotu/db.h"
#include "xiaotu/gen/customer_dao.h"
#include "xiaotu/gen/customer_service.h"

namespace xiaotu {

// 客户档案的对外服务。
//
// 信用额度从 v28 起以 partner.credit_limit_cents 为唯一来源，customer 表只做
// 兼容老查询/报表的镜像。生成层只做单表 CRUD，凡是涉及额度的读写都走这里，
// 避免页面、导入和审核校验各写各的。
class CustomerService {
 public:
  explicit CustomerService(Db& db) : db_(db), base_(db) {}

  std::vector<gen::CustomerRow> list();
  gen::CustomerRow create(gen::CustomerRow input);
  gen::CustomerRow update(std::int64_t id, gen::CustomerRow input);
  void remove(std::int64_t id);

  // 该客户当前生效的信用额度（分）。找不到 partner 时回落到 customer 镜像。
  std::int64_t credit_limit_cents(std::int64_t customer_id);

 private:
  static void validate(const gen::CustomerRow& row);

  Db& db_;
  gen::CustomerService base_;
};

// 信用额度的唯一写入点：写 partner，并把同一个值镜像回 customer。
// 必须在写事务里调用；客户不存在或补建 partner 失败时抛错，不做半截写入。
void set_customer_credit_limit(sqlite3* conn, std::int64_t customer_id, std::int64_t cents);

// 修补历史分裂：partner 额度为 0 而 customer 镜像非 0 时，把镜像值并回 partner。
// 只处理这一种情况，其余差异一律以 partner 为准。返回修补条数。
std::size_t repair_customer_credit_mirror(sqlite3* conn);

}  // namespace xiaotu
