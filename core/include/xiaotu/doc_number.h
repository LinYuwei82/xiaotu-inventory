#pragma once

#include "xiaotu/db.h"

#include <string>
#include <string_view>

namespace xiaotu {

// 单据号统一生成器：{前缀}{yyyyMMdd}-{4 位流水}。
// 必须在写事务内调用；允许跳号，不允许重号（doc_no 上的唯一索引是最后防线）。
std::string allocate_doc_no(Conn& conn, std::string_view prefix, std::string_view doc_date);

}  // namespace xiaotu
