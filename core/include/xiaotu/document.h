#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xiaotu/db.h"
#include "xiaotu/models.h"

namespace xiaotu {

enum class StockMode {
  None,       // 不影响库存
  In,         // 入库
  Out,        // 出库
  Transfer,   // 调拨：源仓出、目标仓入，两侧同一事务
  Stocktake,  // 盘点：按实盘数与账面数的差额生成调整流水
  CostAdjust, // 成本调整：不改数量，只调整结存金额
};
enum class PriceSource { Manual };

// 单据往来单位类型：决定引用供应商还是客户档案。
enum class PartnerKind { None, Supplier, Customer };

// 单据角色：交易单据直接产生库存/往来；订单类单据只做确认与转出库来源。
enum class DocumentRole { Transaction, Order };

// 成本钩子：返回本次变动应结转的金额（正数，单位分）。
// 参数为数量、单价（分）与变动前的结存数量/金额，便于移动加权平均等算法复用。
using CostHook = std::function<std::int64_t(std::int64_t qty, std::int64_t price_cents,
                                            std::int64_t balance_qty,
                                            std::int64_t balance_amount)>;

// 单据定义：新增单据类型只加定义，不加流程代码。
struct DocumentDefinition {
  std::string doc_type;       // SAL_OUT / PUR_IN / ...
  std::string doc_no_prefix;  // XSCK / CGRK / ...
  StockMode stock_mode = StockMode::None;
  bool affects_stock = false;
  DocumentRole role = DocumentRole::Transaction;
  PartnerKind partner_kind = PartnerKind::None;
  // 往来方向：1 应收，-1 应付，0 不生成往来记录。
  int finance_direction = 0;
  bool finance_red = false;  // 退货：红字冲销同方向
  PriceSource price_source = PriceSource::Manual;
  CostHook cost_hook;
  // 订单类单据的下推目标（如 SAL_ORDER -> SAL_OUT、PUR_ORDER -> PUR_IN）；
  // 交易单据留空。
  std::string order_converts_to;
  // 审核 / 反审核的附加动作（往来账、库存占用等），M0 为空。
  std::function<void(sqlite3*, std::int64_t)> on_approve;
  std::function<void(sqlite3*, std::int64_t)> on_reverse;
  // 审核 / 反审核（含保存并审核）需要的权限码；成本调整单单独用 cost.adjust，
  // 起草、改草稿与删除草稿仍走 document.write。
  std::string write_permission = "document.write";
};

const std::vector<DocumentDefinition>& document_definitions();
const DocumentDefinition* find_document_definition(std::string_view doc_type);

// 移动加权平均成本：按结存金额比例结转，四舍五入到分，保证清零时金额精确归零。
// 出库成本重算与单据审核共用这一实现，避免两套口径。
std::int64_t moving_average_cost(std::int64_t qty, std::int64_t price_cents,
                                 std::int64_t balance_qty,
                                 std::int64_t balance_amount_cents);

struct DocInput {
  std::string doc_type;
  std::int64_t warehouse_id = 0;
  std::int64_t target_warehouse_id = 0;  // 仅调拨单使用：入库仓
  std::string doc_date;  // 空则取今天
  std::int64_t partner_id = 0;  // 供应商档案 id；0 表示只记录名称
  std::string partner;          // 往来单位名称快照（客户/供应商）
  // 其他出入库的业务类型：GIFT / USAGE / LOSS / OPENING / OTHER；其他单据为
  // 空串，由引擎校验方向与白名单。
  std::string biz_category;
  // 业务员：仅销售订单 / 销售出库 / 销售退货使用，0 表示未指定；名称由服务端快照。
  std::int64_t salesperson_id = 0;
  // 幂等键：同一个 request_id 重复提交只生成一张单据；空串表示老客户端（不做幂等）。
  std::string request_id;
  // 乐观锁：更新类操作要求客户端持有的版本号；-1 表示老客户端未提供，跳过校验。
  std::int64_t version = -1;
  std::vector<DocLineInput> lines;
};

// 单据详情：草稿编辑与审核前读取。
struct DocDetail {
  std::int64_t id = 0;
  std::string doc_no;
  std::string doc_type;
  std::string doc_date;
  std::int64_t warehouse_id = 0;
  std::int64_t target_warehouse_id = 0;
  std::int64_t partner_id = 0;
  std::string partner;
  std::string biz_category;  // 其他出入库的业务类型，其余单据为空
  std::int64_t salesperson_id = 0;
  std::string salesperson_name;
  std::string status;
  std::string order_status;  // 仅订单：'' / CONFIRMED / CONVERTED / CLOSED
  std::int64_t version = 0;  // 乐观锁版本，编辑页提交时原样带回
  std::int64_t total_qty = 0;
  std::int64_t total_amount_cents = 0;
  // 含税口径：净额 + 税额 = 价税合计，均按行汇总（历史单据 tax = 0）。
  std::int64_t total_net_cents = 0;
  std::int64_t total_tax_cents = 0;
  std::int64_t total_with_tax_cents = 0;
  std::vector<DocLineInput> lines;
};

// 订单下推：按订单行分配本次数量，可覆盖单价（只影响生成的单据）。
struct OrderConvertLine {
  std::int64_t order_line_id = 0;
  std::int64_t qty = 0;
  // <0 表示沿用订单单价；>=0 时覆盖本次下推单价，不回写订单。
  std::int64_t price_cents = -1;
};

struct OrderConvertInput {
  std::int64_t warehouse_id = 0;  // 0 表示沿用订单上的预计出库仓
  std::string doc_date;           // 空则取今天
  std::vector<OrderConvertLine> lines;
};

struct OrderConversion {
  std::int64_t doc_id = 0;
  std::string doc_no;
  std::string order_status;
};

// 单据通用引擎：保存即审核，在同一事务内完成单据、库存流水与余额。
class DocumentEngine {
 public:
  struct Created {
    std::int64_t id = 0;
    std::string doc_no;
    std::int64_t version = 0;
  };

  explicit DocumentEngine(Db& db) : db_(db) {}

  // config.toml [sales] credit_limit_mode：warn（默认）/ reject。
  void set_credit_limit_mode(std::string mode) { credit_limit_mode_ = std::move(mode); }

  Created save_and_approve(const DocInput& input, bool confirm_credit = false);
  // 草稿：只写单据与明细，不动库存流水。
  Created save_draft(const DocInput& input);
  // version < 0 表示老客户端未带版本，跳过乐观锁校验（写日志）。
  void update_draft(std::int64_t doc_id, const DocInput& input);
  void delete_draft(std::int64_t doc_id, std::int64_t expected_version = -1);
  // 审核草稿：在写事务内校验并生成库存流水。
  void approve(std::int64_t doc_id, bool confirm_credit = false,
               std::int64_t expected_version = -1);
  DocDetail detail(std::int64_t doc_id);
  std::vector<DocSummary> list(std::string_view doc_type, int limit = 100);
  void reverse(std::int64_t doc_id, std::int64_t expected_version = -1);
  // 订单：确认沿用 approve；转出库生成一张销售出库单并累加已转数量。
  OrderConversion convert_order(std::int64_t order_id, const OrderConvertInput& input);
  // 订单关闭：未转完时手动结束，关闭后禁止继续转出。
  void close_order(std::int64_t order_id);

 private:
  Db& db_;
  std::string credit_limit_mode_ = "warn";
};

}  // namespace xiaotu
