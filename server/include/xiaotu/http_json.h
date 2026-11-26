#pragma once

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <exception>
#include <string>
#include <string_view>

#include "xiaotu/error.h"

namespace xiaotu {

nlohmann::json json_body(const httplib::Request& req);

void json_ok(httplib::Response& res, const nlohmann::json& body);

void json_error(httplib::Response& res, int status, const std::string& code,
                const std::string& message);

// 把业务异常映射为统一错误响应（NOT_FOUND → 404，重复 → 409，其余 → 400）。
void json_from_exception(httplib::Response& res, const AppError& error);

void json_from_exception(httplib::Response& res, const std::exception& error);

// 安全整数解析：非数字、超长或尾部有杂质一律抛 INVALID_INPUT（400），
// 避免裸 std::stoll/stoi 把 std::invalid_argument / out_of_range 冒泡成 500。
std::int64_t parse_id(std::string_view text);

}  // namespace xiaotu
