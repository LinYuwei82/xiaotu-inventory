#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "xiaotu/db.h"

namespace xiaotu {

struct AuditRow {
  std::int64_t id = 0;
  std::string at;
  std::string actor;
  std::string action;
  std::string entity;
  std::int64_t entity_id = 0;
  std::string entity_no;
  std::string detail;
};

// 审计日志只追加：谁、什么时候、对什么对象做了什么、变更摘要。
// 必须在业务写事务内调用（传入同一个 sqlite3*），业务失败时随事务一起回滚。
void audit_write(sqlite3* conn, std::string_view action, std::string_view entity,
                 std::int64_t entity_id, std::string_view entity_no,
                 std::string_view detail = {});

std::vector<AuditRow> audit_recent(Db& db, int limit = 100);

}  // namespace xiaotu
