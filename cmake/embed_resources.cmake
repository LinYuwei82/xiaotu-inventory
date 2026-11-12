# 生成内嵌前端静态资源的 C++ 源文件。
#
# 入参：
#   WEB_DIR  前端产物目录
#   FILES    以 | 分隔的文件名列表，例如 "index.html|app.js|styles.css"
#   OUT      生成的 .cpp 路径
#
# 产物提供 xiaotu::resources::find(path, out)，路径形如 "/index.html"。

if(NOT WEB_DIR OR NOT FILES OR NOT OUT)
  message(FATAL_ERROR "embed_resources.cmake 需要 WEB_DIR、FILES、OUT 三个参数")
endif()

string(REPLACE "|" ";" FILE_LIST "${FILES}")

set(GEN "// 本文件由 cmake/embed_resources.cmake 生成，请勿手工修改。\n")
string(APPEND GEN "#include \"xiaotu/resources.h\"\n\n")
string(APPEND GEN "namespace xiaotu::resources {\nnamespace {\n\n")

set(ARRAYS "")
set(ENTRIES "")

foreach(f IN LISTS FILE_LIST)
  file(READ "${WEB_DIR}/${f}" HEX_CONTENT HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," BYTES "${HEX_CONTENT}")

  set(SYM "${f}")
  string(REPLACE "/" "_" SYM "${SYM}")
  string(REPLACE "." "_" SYM "${SYM}")
  string(REPLACE "-" "_" SYM "${SYM}")

  if(f MATCHES "\\.html$")
    set(MIME "text/html; charset=utf-8")
  elseif(f MATCHES "\\.js$")
    set(MIME "application/javascript; charset=utf-8")
  elseif(f MATCHES "\\.css$")
    set(MIME "text/css; charset=utf-8")
  else()
    set(MIME "application/octet-stream")
  endif()

  string(APPEND ARRAYS "const unsigned char res_${SYM}[] = {${BYTES}0x00};\n")
  string(APPEND ENTRIES "    {\"/${f}\", \"${MIME}\", res_${SYM}, sizeof(res_${SYM}) - 1},\n")
endforeach()

string(APPEND GEN "${ARRAYS}\n")
string(APPEND GEN "const Resource kResources[] = {\n${ENTRIES}};\n\n")
string(APPEND GEN "}  // namespace\n\n")
string(APPEND GEN "bool find(std::string_view path, Resource& out) {\n")
string(APPEND GEN "  for (const auto& resource : kResources) {\n")
string(APPEND GEN "    if (resource.path == path) {\n")
string(APPEND GEN "      out = resource;\n")
string(APPEND GEN "      return true;\n")
string(APPEND GEN "    }\n  }\n  return false;\n}\n\n")
string(APPEND GEN "std::size_t count() { return sizeof(kResources) / sizeof(kResources[0]); }\n\n")
string(APPEND GEN "}  // namespace xiaotu::resources\n")

file(WRITE "${OUT}.tmp" "${GEN}")
execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OUT}.tmp" "${OUT}")
file(REMOVE "${OUT}.tmp")
