#include "report_xlsx_export.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <memory>
#include <vector>

#include "xiaotu/db.h"
#include "xiaotu/http_json.h"

namespace xiaotu {
namespace report_common {

bool xlsx_requested(const httplib::Request& req) {
  return req.has_param("format") && req.get_param_value("format") == "xlsx";
}

void xlsx_preamble(XlsxSheet& sheet, const std::string& title, const std::string& scope) {
  sheet.add_text_row({"报表", title});
  sheet.add_text_row({"筛选条件", scope});
  sheet.add_text_row({"导出时间", now_text()});
  sheet.add_text_row({"金额说明",
                      "金额单位为元，由整数分按 1 元 = 100 分换算并保留两位小数，无浮点误差"});
  sheet.add_text_row({});
}

void xlsx_preamble(XlsxStreamWriter& sheet, const std::string& title, const std::string& scope) {
  sheet.add_text_row({"报表", title});
  sheet.add_text_row({"筛选条件", scope});
  sheet.add_text_row({"导出时间", now_text()});
  sheet.add_text_row({"金额说明",
                      "金额单位为元，由整数分按 1 元 = 100 分换算并保留两位小数，无浮点误差"});
  sheet.add_text_row({});
}

void send_xlsx(httplib::Response& res, const std::string& filename, const std::string& body) {
  res.set_header("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  res.set_content(body, "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet");
}

void send_xlsx_file(httplib::Response& res, const std::string& filename,
                    const std::string& path) {
  auto file = std::make_shared<std::ifstream>(path, std::ios::binary);
  if (!file->is_open()) {
    std::remove(path.c_str());
    json_error(res, 500, "EXPORT_FAILED", "导出文件生成失败");
    return;
  }
  file->seekg(0, std::ios::end);
  const auto length = static_cast<std::size_t>(file->tellg());
  file->seekg(0, std::ios::beg);
  res.set_header("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  res.set_content_provider(
      length, "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
      [file](std::size_t offset, std::size_t length, httplib::DataSink& sink) {
        file->clear();
        file->seekg(static_cast<std::streamoff>(offset));
        std::vector<char> buffer(64 * 1024);
        std::size_t remaining = length;
        while (remaining > 0) {
          const std::size_t chunk = std::min(remaining, buffer.size());
          file->read(buffer.data(), static_cast<std::streamsize>(chunk));
          const auto got = static_cast<std::size_t>(file->gcount());
          if (got == 0) return false;
          if (!sink.write(buffer.data(), got)) return false;
          remaining -= got;
        }
        return true;
      },
      [path](bool) { std::remove(path.c_str()); });
}

}  // namespace report_common
}  // namespace xiaotu
