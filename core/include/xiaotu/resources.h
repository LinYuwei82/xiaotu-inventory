#pragma once

#include <cstddef>
#include <string_view>

namespace xiaotu::resources {

struct Resource {
  std::string_view path;
  std::string_view content_type;
  const unsigned char* data = nullptr;
  std::size_t size = 0;
};

// 按内嵌路径查找资源，例如 "/index.html"。未命中返回 false。
bool find(std::string_view path, Resource& out);

std::size_t count();

}  // namespace xiaotu::resources
