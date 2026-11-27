#include "xiaotu/item_category.h"

#include "xiaotu/db.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

#include "xiaotu/audit.h"
#include "xiaotu/crud.h"
#include "xiaotu/error.h"
#include "xiaotu/ids.h"

namespace xiaotu {
namespace {

std::string trim_copy(std::string value) {
  auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
  value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
  return value;
}

int depth_of(const std::string& path) {
  return std::max(1, static_cast<int>(std::count(path.begin(), path.end(), '/')) - 1);
}

ItemCategoryRow load(Conn& conn, std::int64_t id) {
  Stmt stmt(conn,
            "SELECT id, parent_id, name, path, sort_order, status FROM item_category"
            " WHERE id = ? AND is_deleted = 0;");
  stmt.bind(1, id);
  if (!stmt.step()) {
    throw AppError("NOT_FOUND", "商品分类不存在或已删除");
  }
  ItemCategoryRow row;
  row.id = stmt.i64(0);
  row.parent_id = stmt.i64(1);
  row.name = stmt.text(2);
  row.path = stmt.text(3);
  row.sort_order = stmt.i64(4);
  row.status = stmt.i64(5) != 0;
  row.depth = depth_of(row.path);
  return row;
}

void ensure_sibling_unique(Conn& conn, std::int64_t parent_id, const std::string& name,
                           std::int64_t exclude_id) {
  Stmt stmt(conn,
            "SELECT 1 FROM item_category"
            " WHERE parent_id = ? AND name = ? AND is_deleted = 0 AND id <> ? LIMIT 1;");
  stmt.bind(1, parent_id).bind(2, name).bind(3, exclude_id);
  if (stmt.step()) {
    throw AppError("DUPLICATE_NAME", "同一级下已存在分类「" + name + "」");
  }
}

std::string validated_name(const std::string& raw) {
  const std::string name = trim_copy(raw);
  if (name.empty()) {
    throw AppError("INVALID_INPUT", "分类名称不能为空");
  }
  if (name.size() > 50) {
    throw AppError("INVALID_INPUT", "分类名称最多 50 个字符");
  }
  return name;
}

std::string parent_path(Conn& conn, std::int64_t parent_id) {
  if (parent_id == 0) return "/";
  const ItemCategoryRow parent = load(conn, parent_id);
  return parent.path;
}

}  // namespace

std::vector<ItemCategoryRow> ItemCategoryService::list() {
  std::vector<ItemCategoryRow> rows;
  db_.read([&](Conn& conn) {
    Stmt stmt(conn,
              "SELECT c.id, c.parent_id, c.name, c.path, c.sort_order, c.status,"
              " (SELECT COUNT(*) FROM item i"
              "   WHERE i.category_id = c.id AND i.is_deleted = 0)"
              " FROM item_category c WHERE c.is_deleted = 0;");
    while (stmt.step()) {
      ItemCategoryRow row;
      row.id = stmt.i64(0);
      row.parent_id = stmt.i64(1);
      row.name = stmt.text(2);
      row.path = stmt.text(3);
      row.sort_order = stmt.i64(4);
      row.status = stmt.i64(5) != 0;
      row.depth = depth_of(row.path);
      row.item_count = stmt.i64(6);
      rows.push_back(std::move(row));
    }
  });
  // 按 path 的数值段排序，保证父在子前；同层按 sort_order、id。
  auto components = [](const std::string& path) {
    std::vector<std::int64_t> parts;
    std::string current;
    for (const char ch : path) {
      if (ch == '/') {
        if (!current.empty()) {
          parts.push_back(std::stoll(current));
          current.clear();
        }
      } else {
        current.push_back(ch);
      }
    }
    return parts;
  };
  std::stable_sort(rows.begin(), rows.end(), [&](const ItemCategoryRow& a,
                                                 const ItemCategoryRow& b) {
    const auto left = components(a.path);
    const auto right = components(b.path);
    if (left != right) return left < right;
    if (a.sort_order != b.sort_order) return a.sort_order < b.sort_order;
    return a.id < b.id;
  });
  return rows;
}

ItemCategoryRow ItemCategoryService::create(std::int64_t parent_id, const std::string& name,
                                            std::int64_t sort_order, bool status) {
  const std::string clean = validated_name(name);
  ItemCategoryRow created;
  db_.write([&](Conn& conn) {
    if (parent_id != 0) {
      load(conn, parent_id);  // 父分类必须存在
    }
    ensure_sibling_unique(conn, parent_id, clean, 0);
    const std::int64_t id = next_id();
    const std::string path = parent_path(conn, parent_id) + std::to_string(id) + "/";
    const AuditStamp stamp = audit_stamp();
    Stmt stmt(conn,
              "INSERT INTO item_category (id, parent_id, name, path, sort_order, status,"
              " is_deleted, created_at, created_by, updated_at, updated_by)"
              " VALUES (?, ?, ?, ?, ?, ?, 0, ?, ?, ?, ?);");
    stmt.bind(1, id)
        .bind(2, parent_id)
        .bind(3, clean)
        .bind(4, path)
        .bind(5, sort_order)
        .bind(6, static_cast<std::int64_t>(status ? 1 : 0))
        .bind(7, stamp.at)
        .bind(8, stamp.by)
        .bind(9, stamp.at)
        .bind(10, stamp.by)
        .run();
    created.id = id;
    created.parent_id = parent_id;
    created.name = clean;
    created.path = path;
    created.sort_order = sort_order;
    created.status = status;
    created.depth = depth_of(path);
    audit_write(conn, "create", "item_category", id, clean);
  });
  return created;
}

ItemCategoryRow ItemCategoryService::update(std::int64_t id, std::int64_t parent_id,
                                            const std::string& name, std::int64_t sort_order,
                                            bool status) {
  const std::string clean = validated_name(name);
  ItemCategoryRow updated;
  db_.write([&](Conn& conn) {
    const ItemCategoryRow current = load(conn, id);
    ensure_sibling_unique(conn, parent_id, clean, id);
    std::string path = current.path;
    if (parent_id != current.parent_id) {
      if (parent_id != 0) {
        const ItemCategoryRow parent = load(conn, parent_id);
        if (parent.path.rfind(current.path, 0) == 0) {
          throw AppError("INVALID_INPUT", "不能把分类移动到自己的下级");
        }
      }
      path = parent_path(conn, parent_id) + std::to_string(id) + "/";
      // 整棵子树前缀替换，孩子无需逐条递归。
      Stmt move(conn,
                "UPDATE item_category SET path = ? || substr(path, ?)"
                " WHERE path LIKE ? AND id <> ?;");
      move.bind(1, path)
          .bind(2, static_cast<std::int64_t>(current.path.size() + 1))
          .bind(3, current.path + "%")
          .bind(4, id)
          .run();
    }
    const AuditStamp stamp = audit_stamp();
    Stmt stmt(conn,
              "UPDATE item_category SET parent_id = ?, name = ?, path = ?, sort_order = ?,"
              " status = ?, updated_at = ?, updated_by = ?"
              " WHERE id = ? AND is_deleted = 0;");
    stmt.bind(1, parent_id)
        .bind(2, clean)
        .bind(3, path)
        .bind(4, sort_order)
        .bind(5, static_cast<std::int64_t>(status ? 1 : 0))
        .bind(6, stamp.at)
        .bind(7, stamp.by)
        .bind(8, id)
        .run();
    updated.id = id;
    updated.parent_id = parent_id;
    updated.name = clean;
    updated.path = path;
    updated.sort_order = sort_order;
    updated.status = status;
    updated.depth = depth_of(path);
    audit_write(conn, "update", "item_category", id, clean);
  });
  return updated;
}

void ItemCategoryService::remove(std::int64_t id) {
  db_.write([&](Conn& conn) {
    const ItemCategoryRow current = load(conn, id);
    {
      Stmt children(conn,
                    "SELECT COUNT(*) FROM item_category WHERE parent_id = ? AND is_deleted = 0;");
      children.bind(1, id);
      if (children.step() && children.i64(0) > 0) {
        throw AppError("CATEGORY_HAS_CHILDREN", "该分类下还有子分类，请先处理子分类");
      }
    }
    {
      Stmt items(conn,
                 "SELECT COUNT(*) FROM item WHERE category_id = ? AND is_deleted = 0;");
      items.bind(1, id);
      if (items.step() && items.i64(0) > 0) {
        throw AppError("CATEGORY_HAS_ITEMS", "该分类下还有商品，请先移走商品");
      }
    }
    soft_delete(conn, "item_category", id);
    audit_write(conn, "delete", "item_category", id, current.name);
  });
}

std::vector<std::int64_t> ItemCategoryService::subtree_ids(std::int64_t id) {
  std::vector<std::int64_t> ids;
  if (id == 0) return ids;
  db_.read([&](Conn& conn) {
    std::string path;
    {
      Stmt stmt(conn, "SELECT path FROM item_category WHERE id = ? AND is_deleted = 0;");
      stmt.bind(1, id);
      if (!stmt.step()) return;
      path = stmt.text(0);
    }
    Stmt stmt(conn,
              "SELECT id FROM item_category WHERE path LIKE ? AND is_deleted = 0;");
    stmt.bind(1, path + "%");
    while (stmt.step()) ids.push_back(stmt.i64(0));
  });
  return ids;
}

}  // namespace xiaotu
