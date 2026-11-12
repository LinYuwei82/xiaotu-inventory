#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "xiaotu/db.h"
#include "xiaotu/models.h"

namespace xiaotu {

enum class StockDirection { None, In, Out };
enum class PriceSource { Manual };

// 成本钩子：返回本次变动应结转的金额（正数，单位分）。
// 参数为数量、单价（分）与变动前的结存数量/金额，便于移动加权平均等算法复用。
using CostHook = std::function<std::int64_t(std::int64_t qty, std::int64_t price_cents,
                                            std::int64_t balance_qty,
                                            std::int64_t balance_amount)>;

// 单据定义：新增单据类型只加定义，不加流程代码。
struct DocumentDefinition {
  std::string doc_type;       // SAL_OUT / PUR_IN / ...
  std::string doc_no_prefix;  // XSCK / CGRK / ...
  StockDirection stock_direction = StockDirection::None;
  bool affects_stock = false;
  PriceSource price_source = PriceSource::Manual;
  CostHook cost_hook;
  // 审核 / 反审核的附加动作（往来账、库存占用等），M0 为空。
  std::function<void(sqlite3*, std::int64_t)> on_approve;
  std::function<void(sqlite3*, std::int64_t)> on_reverse;
};

const std::vector<DocumentDefinition>& document_definitions();
const DocumentDefinition* find_document_definition(std::string_view doc_type);

struct DocInput {
  std::string doc_type;
  std::int64_t warehouse_id = 0;
  std::string doc_date;  // 空则取今天
  std::string partner;   // M0 仅记录客户/供应商名称
  std::vector<DocLineInput> lines;
};

// 单据通用引擎：保存即审核，在同一事务内完成单据、库存流水与余额。
class DocumentEngine {
 public:
  struct Created {
    std::int64_t id = 0;
    std::string doc_no;
  };

  explicit DocumentEngine(Db& db) : db_(db) {}

  Created save_and_approve(const DocInput& input);
  std::vector<DocSummary> list(std::string_view doc_type, int limit = 100);
  void reverse(std::int64_t doc_id);

 private:
  Db& db_;
};

}  // namespace xiaotu
