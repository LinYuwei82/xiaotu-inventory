#pragma once

#include <string>

#include "xiaotu/db.h"

namespace xiaotu {

// 公司抬头：打印模板占位符与默认抬头使用。
struct CompanySettings {
  std::string name;
  std::string phone;
  std::string address;
};

// 系统参数：只开放少量本来就该由用户维护、且有代码消费的键。
struct SystemSettings {
  CompanySettings company;
  bool allow_negative_stock = false;
};

// 键白名单写死在实现里，避免演进成任意配置表。
class SettingService {
 public:
  explicit SettingService(Db& db) : db_(db) {}

  CompanySettings company() const;
  SystemSettings load() const;

  // 校验后保存公司抬头与负库存开关；只有值发生变化的键写审计
  // （action=update、entity=setting、entity_no=键名，detail 记录 from/to）。
  SystemSettings update(const SystemSettings& input);

 private:
  Db& db_;
};

}  // namespace xiaotu
