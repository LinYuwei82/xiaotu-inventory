#pragma once

#include <httplib.h>

#include <cstdint>

#include "xiaotu/report_service.h"

namespace xiaotu {
namespace report_common {

// 查询参数里的整数：非法值要报 400，而不是让 stoll/stoi 抛成 500。
std::int64_t query_int(const httplib::Request& req, const char* name);

ReportFilter parse_filter(const httplib::Request& req);

}  // namespace report_common
}  // namespace xiaotu
