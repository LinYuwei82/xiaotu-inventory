#pragma once

#include <httplib.h>

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <string>

namespace xiaotu {
namespace report_common {

bool csv_requested(const httplib::Request& req);

void csv_row(std::string& out, std::initializer_list<std::string> fields);

void csv_preamble(std::string& out, const std::string& title, const std::string& scope);

void send_csv(httplib::Response& res, const std::string& filename, const std::string& body);

// 大报表 CSV 的写缓冲：攒够一块就交给 HTTP sink，内存占用与导出行数无关。
class CsvStream {
 public:
  CsvStream();

  bool raw(httplib::DataSink& sink, const std::string& text);

  bool row(httplib::DataSink& sink, std::initializer_list<std::string> fields);

  bool flush(httplib::DataSink& sink);

 private:
  static constexpr std::size_t kFlushBytes = 256 * 1024;
  std::string buffer_;
};

// 流式 CSV：提供者只被调用一次，写入过程按块推给客户端；返回 false 表示中途写失败。
void send_csv_stream(httplib::Response& res, const std::string& filename,
                     std::function<bool(httplib::DataSink&, CsvStream&)> body);

std::string date_stamp();

}  // namespace report_common
}  // namespace xiaotu
