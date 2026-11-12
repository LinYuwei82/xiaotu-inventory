#include <httplib.h>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

#include "xiaotu/http_guard.h"
#include "xiaotu/http_json.h"
#include "xiaotu/services.h"

namespace xiaotu {
namespace {

// 应收应付统计口径：迁移只回填历史单据的客户引用，不补建升级前的往来记录。
constexpr const char* kArApScopeHeader = "X-Xiaotu-Data-Scope";
constexpr const char* kArApScope = "ar_ap_since_v7";

nlohmann::json settlement_json(const SettlementRow& row) {
  return nlohmann::json{
      {"id", row.id},
      {"settle_no", row.settle_no},
      {"direction", row.direction},
      {"partner_key", row.partner_key},
      {"partner_name", row.partner_name},
      {"amount_cents", row.amount_cents},
      {"settled_on", row.settled_on},
      {"reverses_id", row.reverses_id},
      {"note", row.note},
      {"account_id", row.account_id},
      {"method", row.method},
      {"operator", row.operator_name},
  };
}

}  // namespace

void register_finance_routes(httplib::Server& server, Services& services) {
  server.Get("/api/ar-ap",
             guard("finance.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     int direction = 0;
                     if (req.has_param("direction")) {
                       const std::string value = req.get_param_value("direction");
                       direction = value == "-1" || value == "AP" ? -1 : 1;
                     }
                     const std::string partner_key =
                         req.has_param("partner_key") ? req.get_param_value("partner_key") : "";
                     const std::string status =
                         req.has_param("status") ? req.get_param_value("status") : "";
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : services.finance.entries(direction, partner_key, status)) {
                       list.push_back(nlohmann::json{
                           {"id", row.id},
                           {"doc_id", row.doc_id},
                           {"doc_no", row.doc_no},
                           {"doc_type", row.doc_type},
                           {"direction", row.direction},
                           {"partner_id", row.partner_id},
                           {"partner_name", row.partner_name},
                           {"partner_key", row.partner_key},
                           {"biz_date", row.biz_date},
                           {"due_date", row.due_date},
                           {"amount_cents", row.amount_cents},
                           {"settled_cents", row.settled_cents},
                           {"outstanding_cents", row.outstanding_cents},
                           {"status", row.status},
                       });
                     }
                     json_ok(res, list);
                     res.set_header(kArApScopeHeader, kArApScope);
                   }));

  server.Get("/api/settlements",
             guard("finance.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     int direction = 0;
                     if (req.has_param("direction")) {
                       direction = req.get_param_value("direction") == "-1" ? -1 : 1;
                     }
                     const std::string partner_key =
                         req.has_param("partner_key") ? req.get_param_value("partner_key") : "";
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& row : services.settlements.list(direction, partner_key)) {
                       list.push_back(settlement_json(row));
                     }
                     json_ok(res, list);
                     res.set_header(kArApScopeHeader, kArApScope);
                   }));

  server.Post("/api/settlements",
              guard("finance.write",
                    [&services](const httplib::Request& req, httplib::Response& res) {
                      const auto body = json_body(req);
                      SettlementCreateInput input;
                      input.direction = body.value("direction", 1);
                      input.partner_key = body.value("partner_key", std::string{});
                      input.partner_name = body.value("partner_name", std::string{});
                      input.amount_cents = body.value("amount_cents", std::int64_t{0});
                      input.account_id = body.value("account_id", std::int64_t{0});
                      input.method = body.value("method", std::string{});
                      input.operator_name = body.value("operator", std::string{});
                      input.settled_on = body.value("settled_on", std::string{});
                      input.note = body.value("note", std::string{});
                      if (body.contains("allocations")) {
                        for (const auto& item : body.at("allocations")) {
                          input.allocations.push_back(AllocationInput{
                              item.value("entry_id", std::int64_t{0}),
                              item.value("amount_cents", std::int64_t{0})});
                        }
                      }
                      const auto created = services.settlements.create(input);
                      json_ok(res, settlement_json(created));
                    }));

  server.Post(R"(/api/settlements/(\d+)/reverse)",
              guard("finance.write",
                    [&services](const httplib::Request& req, httplib::Response& res) {
                      const auto created =
                          services.settlements.reverse(std::stoll(req.matches[1].str()));
                      json_ok(res, settlement_json(created));
                    }));
}

}  // namespace xiaotu
