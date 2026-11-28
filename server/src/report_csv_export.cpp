#include "report_csv_export.h"

#include <algorithm>
#include <exception>
#include <string>
#include <utility>

#include "xiaotu/csv.h"
#include "xiaotu/db.h"
#include "xiaotu/log.h"

namespace xiaotu {
namespace report_common {

bool csv_requested(const httplib::Request& req) {
  return req.has_param("format") && req.get_param_value("format") == "csv";
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

std::string date_stamp() {
  std::string stamp = now_text().substr(0, 10);
  stamp.erase(std::remove(stamp.begin(), stamp.end(), '-'), stamp.end());
  return stamp;
}

}  // namespace report_common
}  // namespace xiaotu
