#include "report_export.h"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "xiaotu/csv.h"
#include "xiaotu/db.h"
#include "xiaotu/http_json.h"
#include "xiaotu/log.h"

namespace xiaotu {
namespace report_common {

bool csv_requested(const httplib::Request& req) {
  return req.has_param("format") && req.get_param_value("format") == "csv";
}

bool xlsx_requested(const httplib::Request& req) {
  return req.has_param("format") && req.get_param_value("format") == "xlsx";
}

void csv_row(std::string& out, std::initializer_list<std::string> fields) {
  bool first = true;
  for (const auto& field : fields) {
    if (!first) out += ',';
    first = false;
    out += csv_escape(field);
  }
  out += "\r\n";
}

void csv_preamble(std::string& out, const std::string& title, const std::string& scope) {
  csv_row(out, {"报表", title});
  csv_row(out, {"筛选条件", scope});
  csv_row(out, {"导出时间", now_text()});
  csv_row(out, {"金额说明",
                "金额单位为元，由整数分按 1 元 = 100 分换算并保留两位小数，无浮点误差"});
  out += "\r\n";
}

void send_csv(httplib::Response& res, const std::string& filename, const std::string& body) {
  res.set_header("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  res.set_content("\xEF\xBB\xBF" + body, "text/csv; charset=utf-8");
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

CsvStream::CsvStream() { buffer_ = "\xEF\xBB\xBF"; }

bool CsvStream::raw(httplib::DataSink& sink, const std::string& text) {
  buffer_ += text;
  return buffer_.size() < kFlushBytes || flush(sink);
}

bool CsvStream::row(httplib::DataSink& sink, std::initializer_list<std::string> fields) {
  csv_row(buffer_, fields);
  return buffer_.size() < kFlushBytes || flush(sink);
}

bool CsvStream::flush(httplib::DataSink& sink) {
  if (buffer_.empty()) return true;
  const bool ok = sink.write(buffer_.data(), buffer_.size());
  buffer_.clear();
  return ok;
}

void send_csv_stream(httplib::Response& res, const std::string& filename,
                     std::function<bool(httplib::DataSink&, CsvStream&)> body) {
  res.set_header("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  res.set_chunked_content_provider(
      "text/csv; charset=utf-8",
      [body = std::move(body)](std::size_t, httplib::DataSink& sink) {
        CsvStream out;
        bool ok = false;
        // httplib 只在路由阶段包了 try/catch，provider 里抛出的异常没人接，
        // 会沿着线程池 worker 逃出去 terminate 整个进程，这里兜住并中断响应。
        try {
          ok = body(sink, out) && out.flush(sink);
        } catch (const std::exception& error) {
          log_warn(std::string("导出流中断：") + error.what());
          ok = false;
        } catch (...) {
          log_warn("导出流中断：未知异常");
          ok = false;
        }
        sink.done();
        return ok;
      });
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

std::string date_stamp() {
  std::string stamp = now_text().substr(0, 10);
  stamp.erase(std::remove(stamp.begin(), stamp.end(), '-'), stamp.end());
  return stamp;
}

}  // namespace report_common
}  // namespace xiaotu
