#include <httplib.h>

#include <string>

#include "xiaotu/http_json.h"
#include "xiaotu/http_server.h"
#include "xiaotu/resources.h"

namespace xiaotu {

void HttpServer::register_static_routes() {
  // 浏览器会无条件请求 /favicon.ico：返回内嵌图标，未内嵌（开发态）时保持 204。
  server_->Get("/favicon.ico", [](const httplib::Request&, httplib::Response& res) {
    resources::Resource resource;
    if (!resources::find("/favicon.ico", resource)) {
      res.status = 204;
      return;
    }
    res.set_content(reinterpret_cast<const char*>(resource.data), resource.size,
                    std::string(resource.content_type));
    res.set_header("Cache-Control", "no-cache");
  });

  server_->Get("/", [](const httplib::Request&, httplib::Response& res) {
    resources::Resource resource;
    if (!resources::find("/index.html", resource)) {
      json_error(res, 503, "NO_WEB_ASSET", "未内嵌前端资源（开发态请使用 dev server）");
      return;
    }
    res.set_content(reinterpret_cast<const char*>(resource.data), resource.size,
                    std::string(resource.content_type));
    res.set_header("Cache-Control", "no-cache");
  });

  server_->Get(R"(/([A-Za-z0-9_.\-/]+))",
               [](const httplib::Request& req, httplib::Response& res) {
                 resources::Resource resource;
                 if (!resources::find("/" + req.matches[1].str(), resource)) {
                   res.status = 404;
                   res.set_content("Not Found", "text/plain; charset=utf-8");
                   return;
                 }
                 res.set_content(reinterpret_cast<const char*>(resource.data), resource.size,
                                 std::string(resource.content_type));
                 res.set_header("Cache-Control", "no-cache");
               });
}

}  // namespace xiaotu
