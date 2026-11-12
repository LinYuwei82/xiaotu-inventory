#pragma once

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <exception>
#include <string>

#include "xiaotu/error.h"

namespace xiaotu {

nlohmann::json json_body(const httplib::Request& req);

void json_ok(httplib::Response& res, const nlohmann::json& body);

void json_error(httplib::Response& res, int status, const std::string& code,
                const std::string& message);

// 把业务异常映射为统一错误响应（NOT_FOUND → 404，重复 → 409，其余 → 400）。
void json_from_exception(httplib::Response& res, const AppError& error);

void json_from_exception(httplib::Response& res, const std::exception& error);

}  // namespace xiaotu
