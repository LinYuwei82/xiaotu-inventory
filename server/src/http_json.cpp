#include "xiaotu/http_json.h"

#include <string>

namespace xiaotu {

nlohmann::json json_body(const httplib::Request& req) {
  if (req.body.empty()) {
    return nlohmann::json::object();
  }
  try {
    return nlohmann::json::parse(req.body);
  } catch (const std::exception&) {
    throw AppError("BAD_REQUEST", "请求体不是合法 JSON");
  }
}

void json_ok(httplib::Response& res, const nlohmann::json& body) {
  res.status = 200;
  res.set_content(body.dump(), "application/json; charset=utf-8");
  res.set_header("Cache-Control", "no-store");
}

void json_error(httplib::Response& res, int status, const std::string& code,
                const std::string& message) {
  nlohmann::json body;
  body["code"] = code;
  body["message"] = message;
  res.status = status;
  res.set_content(body.dump(), "application/json; charset=utf-8");
  res.set_header("Cache-Control", "no-store");
}

void json_from_exception(httplib::Response& res, const AppError& error) {
  int status = 400;
  const std::string& code = error.code();
  if (code == "NOT_FOUND") {
    status = 404;
  } else if (code == "FORBIDDEN") {
    status = 403;
  } else if (code == "FEATURE_UNAVAILABLE") {
    // 版本能力隐藏：与权限不足同为 403，但错误码区分，便于前端提示升级。
    status = 403;
  } else if (code == "QUOTA_EXCEEDED") {
    status = 409;
  } else if (code == "ACCOUNT_LOCKED") {
    // 与登录限流保持同一语义：稍后重试，而不是请求格式错误。
    status = 429;
  } else if (code.rfind("DUPLICATE", 0) == 0 || code == "VERSION_CONFLICT") {
    status = 409;
  } else if (code == "BIND_FAILED" || code == "INTERNAL") {
    status = 500;
  }
  json_error(res, status, code, error.what());
}

void json_from_exception(httplib::Response& res, const std::exception& error) {
  json_error(res, 500, "INTERNAL", std::string("服务器内部错误: ") + error.what());
}

}  // namespace xiaotu
