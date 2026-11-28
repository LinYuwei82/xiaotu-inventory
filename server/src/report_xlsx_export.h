#pragma once

#include <httplib.h>

#include <string>

#include "xiaotu/xlsx.h"

namespace xiaotu {
namespace report_common {

bool xlsx_requested(const httplib::Request& req);

void xlsx_preamble(XlsxSheet& sheet, const std::string& title, const std::string& scope);

void xlsx_preamble(XlsxStreamWriter& sheet, const std::string& title, const std::string& scope);

void send_xlsx(httplib::Response& res, const std::string& filename, const std::string& body);

// 把已生成的 xlsx 临时文件按块写给客户端，响应结束后删掉临时文件。
void send_xlsx_file(httplib::Response& res, const std::string& filename,
                    const std::string& path);

}  // namespace report_common
}  // namespace xiaotu
