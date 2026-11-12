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

// 登录等尚无会话的场景：actor 由调用方显式给出，不走当前请求上下文。
void audit_write_as(sqlite3* conn, std::string_view actor, std::string_view action,
                    std::string_view entity, std::int64_t entity_id,
                    std::string_view entity_no, std::string_view detail = {});

std::vector<AuditRow> audit_recent(Db& db, int limit = 100);

// 查询条件：日期用 YYYY-MM-DD，空串表示不限；entity_id 为 0 表示不限。
struct AuditFilter {
  std::string from;
  std::string to;
  std::string entity;
  std::int64_t entity_id = 0;
  std::string actor;
  int page = 1;
  int page_size = 50;
};

struct AuditPage {
  std::vector<AuditRow> rows;
  std::int64_t total = 0;
  int page = 1;
  int page_size = 50;
};

AuditPage audit_query(Db& db, const AuditFilter& filter);

}  // namespace xiaotu
