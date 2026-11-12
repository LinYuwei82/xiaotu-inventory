#pragma once

#include <sqlite3.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace xiaotu {

struct AuditStamp {
  std::string at;
  std::string by;
};

// 由当前请求上下文自动生成审计信息（创建人 / 修改人、时间）。
AuditStamp audit_stamp();

// 统一软删除：置 is_deleted = 1 并刷新修改人与修改时间。
void soft_delete(sqlite3* conn, std::string_view table, std::int64_t id);

// 唯一性预校验（表名、列名来自生成器的可信常量，值一律参数绑定）。
void ensure_unique(sqlite3* conn, std::string_view table, std::string_view column,
                   std::string_view value, std::int64_t exclude_id, std::string_view label);

void ensure_exists(sqlite3* conn, std::string_view table, std::int64_t id, std::string_view label);

}  // namespace xiaotu
