#include "xiaotu/http_util.h"

#include <string_view>

#include "xiaotu/system_shutdown.h"

namespace xiaotu {

std::string cookie_value(const httplib::Request& req, const std::string& name) {
  const std::string header = req.get_header_value("Cookie");
  std::size_t offset = 0;
  while (offset < header.size()) {
    const std::size_t end = header.find(';', offset);
    const std::string part =
        header.substr(offset, end == std::string::npos ? std::string::npos : end - offset);
    const std::size_t equals = part.find('=');
    if (equals != std::string::npos) {
      std::string key = part.substr(0, equals);
      const std::size_t first = key.find_first_not_of(' ');
      if (first != std::string::npos) {
        key = key.substr(first);
      }
      if (key == name) {
        return part.substr(equals + 1);
      }
    }
    if (end == std::string::npos) {
      break;
    }
    offset = end + 1;
  }
  return {};
}

bool state_changing(const std::string& method) {
  return method == "POST" || method == "PUT" || method == "DELETE" || method == "PATCH";
}

bool local_page_request(const httplib::Request& req, int port, const std::string& mode) {
  if (mode != "local") {
    return true;
  }
  if (!is_loopback_address(req.remote_addr)) {
    return false;
  }
  const std::string self = "http://127.0.0.1:" + std::to_string(port);
  const std::string origin = req.get_header_value("Origin");
  if (!origin.empty()) {
    return origin == self;
  }
  const std::string referer = req.get_header_value("Referer");
  if (!referer.empty()) {
    return referer.rfind(self + "/", 0) == 0;
  }
  return true;
}

std::string cookie_attributes(const httplib::Request& req, int max_age_seconds) {
  std::string attributes =
      "; Path=/; HttpOnly; SameSite=Strict; Max-Age=" + std::to_string(max_age_seconds);
  if (is_loopback_address(req.remote_addr) &&
      req.get_header_value("X-Forwarded-Proto") == "https") {
    attributes += "; Secure";
  }
  return attributes;
}

bool shutdown_from_local_origin(const httplib::Request& req) {
  if (!is_loopback_address(req.remote_addr)) {
    return false;
  }
  const std::string xff = req.get_header_value("X-Forwarded-For");
  if (xff.empty()) {
    return true;
  }
  const std::size_t comma = xff.rfind(',');
  std::string_view last = comma == std::string::npos
                              ? std::string_view(xff)
                              : std::string_view(xff).substr(comma + 1);
  while (!last.empty() && (last.front() == ' ' || last.front() == '\t')) {
    last.remove_prefix(1);
  }
  while (!last.empty() && (last.back() == ' ' || last.back() == '\t')) {
    last.remove_suffix(1);
  }
  return is_loopback_address(last);
}

}  // namespace xiaotu
