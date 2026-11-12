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

nlohmann::json detail_to_json(const DocDetail& detail) {
  nlohmann::json lines = nlohmann::json::array();
  for (const auto& line : detail.lines) {
    lines.push_back(nlohmann::json{
        {"item_id", line.item_id},
        {"qty", line.qty},
        {"price_cents", line.price_cents},
    });
  }
  return nlohmann::json{
      {"id", detail.id},
      {"doc_no", detail.doc_no},
      {"doc_type", detail.doc_type},
      {"doc_date", detail.doc_date},
      {"warehouse_id", detail.warehouse_id},
      {"target_warehouse_id", detail.target_warehouse_id},
      {"partner_id", detail.partner_id},
      {"partner", detail.partner},
      {"status", detail.status},
      {"total_qty", detail.total_qty},
      {"total_amount_cents", detail.total_amount_cents},
      {"lines", lines},
  };
}

DocInput input_from_json(const nlohmann::json& body) {
  DocInput input;
  input.doc_type = body.value("doc_type", std::string{});
  input.warehouse_id = body.value("warehouse_id", std::int64_t{0});
  input.doc_date = body.value("doc_date", std::string{});
  input.target_warehouse_id = body.value("target_warehouse_id", std::int64_t{0});
  input.partner_id = body.value("partner_id", std::int64_t{0});
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

  server.Get(R"(/api/documents/(\d+))",
             guard("document.read",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const auto id = std::stoll(req.matches[1].str());
                     json_ok(res, detail_to_json(services.docs.detail(id)));
                   }));

  server.Post("/api/documents",
              guard("document.write",
                    [&services](const httplib::Request& req, httplib::Response& res) {
                      const nlohmann::json body = json_body(req);
                      const DocInput input = input_from_json(body);
                      const bool draft = body.value("save_as_draft", false);
                      const auto created =
                          draft ? services.docs.save_draft(input)
                                : services.docs.save_and_approve(input);
                      nlohmann::json result;
                      result["id"] = created.id;
                      result["doc_no"] = created.doc_no;
                      json_ok(res, result);
                    }));

  server.Put(R"(/api/documents/(\d+))",
             guard("document.write",
                   [&services](const httplib::Request& req, httplib::Response& res) {
                     const auto id = std::stoll(req.matches[1].str());
                     services.docs.update_draft(id, input_from_json(json_body(req)));
                     json_ok(res, detail_to_json(services.docs.detail(id)));
                   }));

  server.Delete(R"(/api/documents/(\d+))",
                guard("document.write",
                      [&services](const httplib::Request& req, httplib::Response& res) {
                        const auto id = std::stoll(req.matches[1].str());
                        services.docs.delete_draft(id);
                        nlohmann::json body;
                        body["ok"] = true;
                        json_ok(res, body);
                      }));

  server.Post(R"(/api/documents/(\d+)/approve)",
              guard("document.write",
                    [&services](const httplib::Request& req, httplib::Response& res) {
                      const auto id = std::stoll(req.matches[1].str());
                      services.docs.approve(id);
                      json_ok(res, detail_to_json(services.docs.detail(id)));
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
