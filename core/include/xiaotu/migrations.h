#pragma once

#include <string_view>
#include <vector>

#include "xiaotu/db.h"

namespace xiaotu {

struct Migration {
  int version;
  const char* name;
  const char* sql;
};

const std::vector<Migration>& all_migrations();

// 代码内置的最新 schema 版本，用于判断是否需要迁移前备份。
int target_schema_version();

// 按版本顺序执行未应用的迁移，并在执行前校验已应用脚本的 checksum。
void migrate(Db& db);

// 迁移到指定版本（含）。migrate() 即迁到最新版本。
// 暴露给测试构造低版本老库，验证升级链路（如 v12 老库带零金额流水穿过 v13）。
void migrate_to(Db& db, int target_version);

// v36 回填语句原文。迁移内只执行一次；单独暴露以便测试在已有库上重复执行，
// 验证幂等与「回填前后报表逐行一致」。
std::string_view partner_id_backfill_sql();

}  // namespace xiaotu
