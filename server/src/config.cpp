#include "xiaotu/config.h"

#include <cctype>
#include <fstream>
#include <set>
#include <string>
#include <string_view>

#include "xiaotu/error.h"

namespace xiaotu {
namespace {

std::string trim(std::string_view text) {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  return std::string(text.substr(begin, end - begin));
}

// 去掉行尾注释；引号内的 # 保留。
std::string strip_comment(const std::string& line) {
  bool in_string = false;
  bool escaped = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char ch = line[i];
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == '"') {
        in_string = false;
      }
      continue;
    }
    if (ch == '"') {
      in_string = true;
    } else if (ch == '#') {
      return line.substr(0, i);
    }
  }
  return line;
}

[[noreturn]] void fail(const std::filesystem::path& path, int line_no, const std::string& detail) {
  throw AppError("INVALID_CONFIG", path.string() + " 第 " + std::to_string(line_no) + " 行：" + detail);
}

std::string parse_string(const std::string& raw, const std::filesystem::path& path, int line_no) {
  if (raw.size() < 2 || raw.front() != '"' || raw.back() != '"') {
    fail(path, line_no, "字符串需要用双引号包裹");
  }
  std::string out;
  for (std::size_t i = 1; i + 1 < raw.size(); ++i) {
    const char ch = raw[i];
    if (ch != '\\') {
      out.push_back(ch);
      continue;
    }
    if (i + 2 >= raw.size()) {
      fail(path, line_no, "字符串以未完成的转义结尾");
    }
    const char escaped = raw[++i];
    switch (escaped) {
      case '"':
      case '\\':
        out.push_back(escaped);
        break;
      case 'n':
        out.push_back('\n');
        break;
      case 't':
        out.push_back('\t');
        break;
      default:
        fail(path, line_no, std::string("不支持的转义：\\") + escaped);
    }
  }
  return out;
}

std::int64_t parse_int(const std::string& raw, const std::filesystem::path& path, int line_no) {
  if (raw.empty()) {
    fail(path, line_no, "缺少数值");
  }
  std::size_t consumed = 0;
  long long value = 0;
  try {
    value = std::stoll(raw, &consumed, 10);
  } catch (const std::exception&) {
    fail(path, line_no, "不是合法整数：" + raw);
  }
  if (consumed != raw.size()) {
    fail(path, line_no, "不是合法整数：" + raw);
  }
  return value;
}

void assign(ConfigValues& values, const std::string& section, const std::string& key,
            const std::string& raw, const std::filesystem::path& path, int line_no,
            std::set<std::string>& seen) {
  const std::string qualified = section + "." + key;
  if (!seen.insert(qualified).second) {
    fail(path, line_no, "重复的配置项：" + qualified);
  }

  if (section == "server" && key == "listen") {
    const std::string value = parse_string(raw, path, line_no);
    if (value.empty()) {
      fail(path, line_no, "server.listen 不能为空");
    }
    values.listen = value;
    return;
  }
  if (section == "server" && key == "port") {
    const std::int64_t value = parse_int(raw, path, line_no);
    if (value < 1 || value > 65535) {
      fail(path, line_no, "server.port 应在 1–65535 之间");
    }
    values.port = static_cast<int>(value);
    return;
  }
  if (section == "storage" && key == "data_dir") {
    const std::string value = parse_string(raw, path, line_no);
    if (value.empty()) {
      fail(path, line_no, "storage.data_dir 不能为空");
    }
    values.data_dir = value;
    return;
  }
  if (section == "log" && key == "level") {
    std::string value = parse_string(raw, path, line_no);
    for (char& ch : value) {
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (value != "debug" && value != "info" && value != "warn" && value != "error") {
      fail(path, line_no, "log.level 只能是 debug / info / warn / error");
    }
    values.log_level = value;
    return;
  }
  if (section == "log" && key == "dir") {
    const std::string value = parse_string(raw, path, line_no);
    if (value.empty()) {
      fail(path, line_no, "log.dir 不能为空");
    }
    values.log_dir = value;
    return;
  }
  if (section == "sales" && key == "credit_limit_mode") {
    std::string value = parse_string(raw, path, line_no);
    for (char& ch : value) {
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (value != "warn" && value != "reject") {
      fail(path, line_no, "sales.credit_limit_mode 只能是 warn / reject");
    }
    values.credit_limit_mode = value;
    return;
  }
  fail(path, line_no, "未知配置项：" + qualified);
}

}  // namespace

ConfigValues load_config_file(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    throw AppError("NOT_FOUND", "无法读取配置文件：" + path.string());
  }

  ConfigValues values;
  std::set<std::string> seen;
  std::string section;
  std::string line;
  int line_no = 0;
  while (std::getline(input, line)) {
    ++line_no;
    const std::string content = trim(strip_comment(line));
    if (content.empty()) {
      continue;
    }
    if (content.front() == '[') {
      if (content.back() != ']') {
        fail(path, line_no, "段名缺少右方括号");
      }
      section = trim(std::string_view(content).substr(1, content.size() - 2));
      if (section != "server" && section != "storage" && section != "log" &&
          section != "sales") {
        fail(path, line_no, "未知配置段：[" + section + "]");
      }
      continue;
    }
    const std::size_t equal = content.find('=');
    if (equal == std::string::npos) {
      fail(path, line_no, "配置行缺少 =");
    }
    if (section.empty()) {
      fail(path, line_no, "配置项必须写在 [server]、[storage]、[log]、[sales] 段内");
    }
    assign(values, section, trim(content.substr(0, equal)), trim(content.substr(equal + 1)), path,
           line_no, seen);
  }
  return values;
}

}  // namespace xiaotu
