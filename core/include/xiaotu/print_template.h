#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "xiaotu/db.h"

namespace xiaotu {

// 打印目标与可打印列的白名单。模板只能引用这里登记过的目标与列，
// 纸张之外的一切展示配置都在模板里，SQL、脚本、查询条件一律不进模板。
struct PrintColumnDef {
  std::string key;
  std::string label;
  std::string type;   // TEXT / INTEGER / MONEY / PERCENT
  std::string align;  // LEFT / CENTER / RIGHT
  int width_mm = 0;   // 建议列宽，0 表示自动
};

struct PrintTargetDef {
  std::string type;  // DOC / REPORT
  std::string key;
  std::string name;
  std::vector<PrintColumnDef> columns;
};

const std::vector<PrintTargetDef>& print_targets();

// 目标不存在返回 nullptr；类型与键都参与匹配。
const PrintTargetDef* find_print_target(std::string_view target_type,
                                        std::string_view target_key);

// 列不在白名单内返回 nullptr。
const PrintColumnDef* find_print_column(const PrintTargetDef& target, std::string_view column_key);

struct PrintTemplateColumn {
  std::string column_key;
  int position = 0;
  bool visible = true;
  int width_mm = 0;       // 0 表示自动
  std::string align = "LEFT";
  int font_size_pt = 0;   // 0 表示跟随模板字号
  bool bold = false;
};

struct PrintTemplate {
  std::int64_t id = 0;
  std::string code;
  std::string name;
  std::string target_type;
  std::string target_key;
  std::string page_size = "A4";        // A4 / A5 / CUSTOM
  std::string orientation = "PORTRAIT";  // PORTRAIT / LANDSCAPE
  int paper_width_mm = 0;              // 仅 CUSTOM 使用
  int paper_height_mm = 0;             // 仅 CUSTOM 使用
  int margin_mm = 12;
  int font_size_pt = 10;
  std::string header_text;
  std::string footer_text;
  bool show_page_no = true;
  bool is_default = false;
  bool active = true;
  std::int64_t version = 1;  // 更新时作为乐观锁的期望值
  std::string created_at;
  std::string created_by;
  std::string updated_at;
  std::string updated_by;
  std::vector<PrintTemplateColumn> columns;
};

// 套用模板列配置后的渲染列；隐藏列与失效列已过滤。
struct PrintRenderColumn {
  PrintColumnDef def;
  int width_mm = 0;
  std::string align;
  int font_size_pt = 0;
  bool bold = false;
};

// 内置默认模板：来自代码注册表，不落库。未知目标返回无列的通用兜底。
PrintTemplate builtin_print_template(std::string_view target_type, std::string_view target_key);

// 按模板列顺序解析渲染列：隐藏列跳过，目标已不存在的列跳过。
std::vector<PrintRenderColumn> resolved_print_columns(const PrintTemplate& tmpl);

class PrintTemplateService {
 public:
  explicit PrintTemplateService(Db& db) : db_(db) {}

  // 只返回未删除的模板；target_type / target_key 为空表示不过滤。
  std::vector<PrintTemplate> list(std::string_view target_type = {},
                                  std::string_view target_key = {}) const;
  PrintTemplate get(std::int64_t id) const;

  // 保存前校验目标、列白名单与纸张范围；未知目标或未知列抛 INVALID_INPUT。
  PrintTemplate create(const PrintTemplate& input);
  // input.version 必须等于库内版本，成功后版本加一；不匹配抛 VERSION_CONFLICT。
  PrintTemplate update(std::int64_t id, const PrintTemplate& input);
  // 软删除；删除后该目标回退到内置默认。
  void remove(std::int64_t id);
  // 同一目标只保留一个默认模板，切换时自动取消旧的默认标记。
  PrintTemplate set_default(std::int64_t id);

  // 显式模板 → 目标默认模板 → 内置默认；任一步失效都静默回退。
  PrintTemplate resolve(std::string_view target_type, std::string_view target_key,
                        std::int64_t template_id = 0) const;

 private:
  Db& db_;
};

}  // namespace xiaotu
