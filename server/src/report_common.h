#pragma once

#include <httplib.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>

#include "xiaotu/report_service.h"
#include "xiaotu/setting_service.h"
#include "xiaotu/xlsx.h"

namespace xiaotu {
namespace report_common {

// 账龄口径与应收应付台账一致：只统计 v7 迁移后审核的单据。
inline constexpr const char* kArApScopeHeader = "X-Xiaotu-Data-Scope";
inline constexpr const char* kArApScope = "ar_ap_since_v7";

// ---------- 查询参数 ----------

// 查询参数里的整数：非法值要报 400，而不是让 stoll/stoi 抛成 500。
std::int64_t query_int(const httplib::Request& req, const char* name);

ReportFilter parse_filter(const httplib::Request& req);

// ---------- CSV 导出 ----------

bool csv_requested(const httplib::Request& req);

bool xlsx_requested(const httplib::Request& req);

void csv_row(std::string& out, std::initializer_list<std::string> fields);

// 金额统一按「元、两位小数」输出；从整数分换算，避免浮点误差。
std::string yuan(std::int64_t cents);

std::string period_label(const ReportFilter& filter);

std::string dimension_label(std::int64_t id, const char* name);

void csv_preamble(std::string& out, const std::string& title, const std::string& scope);

void send_csv(httplib::Response& res, const std::string& filename, const std::string& body);

// ---------- XLSX 导出 ----------

void xlsx_preamble(XlsxSheet& sheet, const std::string& title, const std::string& scope);

void xlsx_preamble(XlsxStreamWriter& sheet, const std::string& title, const std::string& scope);

void send_xlsx(httplib::Response& res, const std::string& filename, const std::string& body);

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

// 把已生成的 xlsx 临时文件按块写给客户端，响应结束后删掉临时文件。
void send_xlsx_file(httplib::Response& res, const std::string& filename,
                    const std::string& path);

std::string date_stamp();

// ---------- 展示标签 ----------

std::string margin_text(std::int64_t revenue_cents, std::int64_t profit_cents);

std::string biz_category_label(const std::string& category);

// 成本口径说明：报表标注当前口径与切换日，避免把切换前后的毛利直接对比。
std::string cost_basis_label(const SystemSettings& settings);

std::string method_label(const std::string& method);

}  // namespace report_common
}  // namespace xiaotu
