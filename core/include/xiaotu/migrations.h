#pragma once

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

}  // namespace xiaotu
