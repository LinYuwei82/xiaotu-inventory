#include "report_common.h"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "xiaotu/csv.h"
#include "xiaotu/db.h"
#include "xiaotu/error.h"
#include "xiaotu/http_json.h"
#include "xiaotu/log.h"

namespace xiaotu {
namespace report_common {

std::int64_t query_int(const httplib::Request& req, const char* name) {
  const std::string text = req.get_param_value(name);
  std::size_t consumed = 0;
  std::int64_t value = 0;
  const auto reject = [name, &text]() {
    throw AppError("INVALID_INPUT", std::string("参数 ") + name + " 不是有效整数：" + text);
  };
  try {
    value = std::stoll(text, &consumed);
  } catch (const std::exception&) {
    reject();
  }
  if (consumed != text.size()) {
    reject();
  }
  return value;
}

ReportFilter parse_filter(const httplib::Request& req) {
  ReportFilter filter;
  if (req.has_param("from")) filter.from = req.get_param_value("from");
  if (req.has_param("to")) filter.to = req.get_param_value("to");
  if (req.has_param("item_id")) filter.item_id = query_int(req, "item_id");
  if (req.has_param("warehouse_id")) {
    filter.warehouse_id = query_int(req, "warehouse_id");
  }
  if (req.has_param("customer_id")) {
    filter.customer_id = query_int(req, "customer_id");
  }
  if (req.has_param("supplier_id")) {
    filter.supplier_id = query_int(req, "supplier_id");
  }
  if (req.has_param("salesperson_id")) {
    filter.salesperson_id = query_int(req, "salesperson_id");
  }
  // limit 走 int：超过 int 范围的值以前会在 static_cast 后回绕成 0（不限行数），
  // 一次请求就能把全量流水读进内存，这里直接拒绝。
  if (req.has_param("limit")) {
    const std::int64_t limit = query_int(req, "limit");
    if (limit < 0 || limit > std::numeric_limits<int>::max()) {
      throw AppError("INVALID_INPUT", "参数 limit 超出范围");
    }
    filter.limit = static_cast<int>(limit);
  }
  if (req.has_param("offset")) {
    const std::int64_t offset = query_int(req, "offset");
    filter.offset = offset < 0 ? 0 : static_cast<int>(
                                         std::min<std::int64_t>(
                                             offset, std::numeric_limits<int>::max()));
  }
  return filter;
}

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

std::string yuan(std::int64_t cents) {
  const bool negative = cents < 0;
  const std::int64_t abs = negative ? -cents : cents;
  std::string fraction = std::to_string(abs % 100);
  if (fraction.size() < 2) fraction.insert(0, 2 - fraction.size(), '0');
  return (negative ? "-" : "") + std::to_string(abs / 100) + "." + fraction;
}

std::string period_label(const ReportFilter& filter) {
  if (filter.from.empty() && filter.to.empty()) return "全部期间";
  return (filter.from.empty() ? std::string("起始不限") : filter.from) + " 至 " +
         (filter.to.empty() ? std::string("截止不限") : filter.to);
}

std::string dimension_label(std::int64_t id, const char* name) {
  return id == 0 ? std::string(name) + " 全部"
                 : std::string(name) + " ID " + std::to_string(id);
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

std::string margin_text(std::int64_t revenue_cents, std::int64_t profit_cents) {
  if (revenue_cents == 0) return "—";
  // 毛利率保留一位小数，用整数千分比换算，展示层也不引入浮点。
  const std::int64_t direction = revenue_cents < 0 ? -1 : 1;
  const std::int64_t revenue = revenue_cents < 0 ? -revenue_cents : revenue_cents;
  const std::int64_t profit = profit_cents * direction;
  const std::int64_t negative = profit < 0 ? -1 : 1;
  const std::int64_t absolute = profit < 0 ? -profit : profit;
  const std::int64_t tenths = (absolute * 1000 + revenue / 2) / revenue;
  return (negative < 0 ? "-" : "") + std::to_string(tenths / 10) + "." +
         std::to_string(tenths % 10) + "%";
}

std::string biz_category_label(const std::string& category) {
  if (category == "GIFT") return "赠品";
  if (category == "USAGE") return "领用";
  if (category == "LOSS") return "报损";
  if (category == "OPENING") return "期初";
  if (category == "OTHER") return "其他";
  return category;
}

std::string cost_basis_label(const SystemSettings& settings) {
  std::string label = settings.cost_basis == "NET" ? "不含税净额" : "含税价";
  if (!settings.cost_basis_changed_at.empty()) {
    label += "（切换于 " + settings.cost_basis_changed_at + "）";
  }
  return label;
}

std::string method_label(const std::string& method) {
  if (method == "CASH") return "现金";
  if (method == "BANK") return "银行";
  if (method == "WECHAT") return "微信";
  if (method == "ALIPAY") return "支付宝";
  if (method == "OTHER") return "其他";
  return method.empty() ? "未记录" : method;
}

}  // namespace report_common
}  // namespace xiaotu
