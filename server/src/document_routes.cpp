#include <httplib.h>
#include <nlohmann/json.hpp>

#include <string>

#include "xiaotu/document.h"
#include "xiaotu/http_guard.h"
#include "xiaotu/http_json.h"
#include "xiaotu/services.h"

namespace xiaotu {
namespace {

nlohmann::json summary_to_json(const DocSummary& summary) {
  return nlohmann::json{
      {"id", summary.id},
      {"doc_no", summary.doc_no},
      {"doc_date", summary.doc_date},
      {"warehouse_name", summary.warehouse_name},
      {"customer", summary.customer},
      {"total_qty", summary.total_qty},
      {"total_amount_cents", summary.total_amount_cents},
      {"status", summary.status},
      {"created_at", summary.created_at},
  };
}

DocInput input_from_json(const nlohmann::json& body) {
  DocInput input;
  input.doc_type = body.value("doc_type", std::string{});
  input.warehouse_id = body.value("warehouse_id", std::int64_t{0});
  input.doc_date = body.value("doc_date", std::string{});
  input.partner = body.value("partner", std::string{});
  if (body.contains("lines") && body["lines"].is_array()) {
    for (const auto& item : body["lines"]) {
      DocLineInput line;
      line.item_id = item.value("item_id", std::int64_t{0});
      line.qty = item.value("qty", std::int64_t{0});
      line.price_cents = item.value("price_cents", std::int64_t{0});
      input.lines.push_back(line);
    }
  }
  return input;
}

}  // namespace

void register_document_routes(httplib::Server& server, Services& services) {
  server.Get("/api/documents",
             guard("document.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const std::string doc_type = req.has_param("doc_type")
                                                      ? req.get_param_value("doc_type")
                                                      : "SAL_OUT";
                     nlohmann::json list = nlohmann::json::array();
                     for (const auto& summary : services.docs.list(doc_type)) {
                       list.push_back(summary_to_json(summary));
                     }
                     json_ok(res, list);
                   }));

  server.Post("/api/documents",
              guard("document.write",
                    [&services](const httplib::Request& req, httplib::Response& res) {
                      const DocInput input = input_from_json(json_body(req));
                      const auto created = services.docs.save_and_approve(input);
                      nlohmann::json body;
                      body["id"] = created.id;
                      body["doc_no"] = created.doc_no;
                      json_ok(res, body);
                    }));

  server.Post(R"(/api/documents/(\d+)/reverse)",
              guard("document.write",
                    [&services](const httplib::Request& req, httplib::Response& res) {
                      const auto id = std::stoll(req.matches[1].str());
                      services.docs.reverse(id);
                      nlohmann::json body;
                      body["ok"] = true;
                      json_ok(res, body);
                    }));
}

}  // namespace xiaotu
