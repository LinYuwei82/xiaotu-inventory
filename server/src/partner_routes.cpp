#include <httplib.h>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

#include "xiaotu/context.h"
#include "xiaotu/http_guard.h"
#include "xiaotu/http_json.h"
#include "xiaotu/partner_merge.h"
#include "xiaotu/partner_preview.h"
#include "xiaotu/services.h"

namespace xiaotu {
namespace {

nlohmann::json partner_json(const PartnerRow& row) {
  nlohmann::json supplier_ids = nlohmann::json::array();
  for (const std::int64_t id : row.supplier_ids) supplier_ids.push_back(id);
  return nlohmann::json{
      {"id", row.id},
      {"code", row.code},
      {"name", row.name},
      {"is_customer", row.is_customer != 0},
      {"is_supplier", row.is_supplier != 0},
      {"merged_into", row.merged_into},
      {"origin", row.origin},
      {"status", row.status != 0},
      {"supplier_ids", supplier_ids},
  };
}

nlohmann::json merge_outcome_json(const PartnerMergeOutcome& outcome) {
  nlohmann::json sources = nlohmann::json::array();
  for (const std::int64_t id : outcome.source_ids) sources.push_back(id);
  return nlohmann::json{{"ok", true},
                        {"log_id", outcome.log_id},
                        {"target_id", outcome.target_id},
                        {"target_code", outcome.target_code},
                        {"target_name", outcome.target_name},
                        {"source_ids", sources}};
}

nlohmann::json merge_log_json(const PartnerMergeLogRow& row) {
  nlohmann::json body{
      {"id", row.id},
      {"target_id", row.target_id},
      {"target_code", row.target_code},
      {"target_name", row.target_name},
      {"target_is_customer", row.target_is_customer != 0},
      {"target_is_supplier", row.target_is_supplier != 0},
      {"source_ids", row.source_ids},
      {"actor", row.actor},
      {"created_at", row.created_at},
      {"can_undo", row.can_undo},
      {"undo_reason", row.undo_reason},
      {"undone_at", row.undone_at},
      {"undone_by", row.undone_by},
  };
  // 快照原样透出，方便审计页展开核对；解析失败时退回字符串。
  const auto attach = [&](const char* key, const std::string& text, bool array) {
    try {
      body[key] = nlohmann::json::parse(text);
    } catch (const std::exception&) {
      body[key] = array ? nlohmann::json::array() : nlohmann::json::object();
    }
  };
  attach("target_snapshot", row.target_snapshot, false);
  attach("sources_snapshot", row.sources_snapshot, true);
  attach("moved_maps", row.moved_maps, true);
  attach("source_keys", row.source_keys, true);
  return body;
}

std::vector<std::int64_t> parse_ids(const nlohmann::json& value) {
  std::vector<std::int64_t> ids;
  if (!value.is_array()) return ids;
  for (const auto& item : value) {
    if (item.is_number_integer()) ids.push_back(item.get<std::int64_t>());
    else if (item.is_string()) {
      try {
        ids.push_back(std::stoll(item.get<std::string>()));
      } catch (const std::exception&) {
        // 忽略非法 id，交给合并服务校验。
      }
    }
  }
  return ids;
}

}  // namespace

void register_partner_routes(httplib::Server& server, Services& services) {
  // 往来单位清单：默认含已合并记录（merged_into > 0），前端用它在列表与下拉里隐藏。
  server.Get("/api/partners",
             guard("customer.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const std::string role =
                         req.has_param("role") ? req.get_param_value("role") : "";
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : partner_rows(services.db)) {
                       if (role == "customer" && row.is_customer == 0) continue;
                       if (role == "supplier" && row.is_supplier == 0) continue;
                       list.push_back(partner_json(row));
                     }
                     json_ok(res, list);
                   }));

  // 归并预览：只读，输出候选组与来源标记，供合并前人工确认。
  server.Get("/api/partners/merge-preview",
             guard("customer.read",
                   [&services](const httplib::Request&, httplib::Response& res) {
                     const PartnerPreviewReport report = partner_merge_preview(services.db);
                     res.set_content(partner_preview_json(report),
                                     "application/json; charset=utf-8");
                   }));

  server.Post("/api/partners/merge",
              guard("customer.write",
                    [&services](const httplib::Request& req, httplib::Response& res) {
                      const auto body = json_body(req);
                      const std::int64_t target_id = body.value("target_id", std::int64_t{0});
                      const std::vector<std::int64_t> source_ids =
                          parse_ids(body.value("source_ids", nlohmann::json::array()));
                      if (target_id <= 0) {
                        throw AppError("INVALID_INPUT", "请选择保留档案");
                      }
                      json_ok(res, merge_outcome_json(partner_merge(
                                         services.db, target_id, source_ids, current_user())));
                    }));

  server.Post("/api/partners/undo",
              guard("customer.write",
                    [&services](const httplib::Request& req, httplib::Response& res) {
                      const auto body = json_body(req);
                      const std::int64_t log_id = body.value("log_id", std::int64_t{0});
                      if (log_id <= 0) throw AppError("INVALID_INPUT", "请指定合并记录");
                      json_ok(res,
                              merge_outcome_json(
                                  partner_merge_undo(services.db, log_id, current_user())));
                    }));

  server.Get("/api/partners/merge-log",
             guard("customer.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     int limit = 50;
                     if (req.has_param("limit")) {
                       try {
                         limit = static_cast<int>(parse_id(req.get_param_value("limit")));
                       } catch (const std::exception&) {
                         limit = 50;
                       }
                     }
                     nlohmann::json list = nlohmann::json::array();
                     for (auto row : partner_merge_logs(services.db, limit)) {
                       if (row.can_undo) {
                         // 合并后若已写入新业务，撤销按钮要能提前显示不可用原因。
                         const std::string blocker =
                             partner_merge_undo_blocker(services.db, row.id);
                         if (!blocker.empty()) {
                           row.can_undo = false;
                           row.undo_reason = blocker;
                         }
                       }
                       list.push_back(merge_log_json(row));
                     }
                     json_ok(res, list);
                   }));
}

}  // namespace xiaotu
