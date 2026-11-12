#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "xiaotu/db.h"

namespace xiaotu {

// 商品分类：parent_id 自引用 + path 冗余（形如 /1/7/23/）。
// 按分类查商品时用 path 前缀匹配带出整棵子树，不做递归查询。
struct ItemCategoryRow {
  std::int64_t id = 0;
  std::int64_t parent_id = 0;
  std::string name;
  std::string path;
  std::int64_t sort_order = 0;
  bool status = true;
  int depth = 0;             // 由 path 推导，根为 1
  std::int64_t item_count = 0;  // 直接归属该分类的商品数
};

class ItemCategoryService {
 public:
  explicit ItemCategoryService(Db& db) : db_(db) {}

  // 按树的前序顺序返回（父在子前），前端按 depth 缩进。
  std::vector<ItemCategoryRow> list();

  ItemCategoryRow create(std::int64_t parent_id, const std::string& name,
                         std::int64_t sort_order, bool status);
  ItemCategoryRow update(std::int64_t id, std::int64_t parent_id, const std::string& name,
                         std::int64_t sort_order, bool status);
  void remove(std::int64_t id);

  // 分类子树 id（含自身），用于商品查询；分类不存在时返回空。
  std::vector<std::int64_t> subtree_ids(std::int64_t id);

 private:
  Db& db_;
};

}  // namespace xiaotu
