#pragma once

// 跨平台 UTF-8 边界模块：把 Windows 上与编码有关的 Win32 样板收在一处，
// 调用方不需要 #ifdef（POSIX 侧为等价实现，不是空壳）。
//
// 覆盖范围：
//   - UTF-8 ↔ 宽字符（Windows: UTF-16；POSIX: wchar_t 即 UTF-32）
//   - 严格 UTF-8 失败时按系统代码页兜底（Windows: CP_ACP；POSIX: 逐字节 Latin-1）
//   - std::filesystem::path ↔ UTF-8（Windows 上 .string() 按 ANSI 转换，中文路径会坏）
//   - 控制台：仅当句柄真的是控制台时切 UTF-8 代码页，重定向到文件/管道时不动字节
//   - 系统错误码 → UTF-8 文本
//
// 不在本模块内：文本文件的 BOM / UTF-16 归一（属业务文本处理，留在各仓库 core）。

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace win_encoding {

// 是否为合法 UTF-8：拒绝过长编码、UTF-16 代理区、超过 U+10FFFF 的码点、截断序列。
// 空串视为合法。
bool is_valid_utf8(std::string_view utf8);

// UTF-8 → 宽字符。非法字节会被替换，不会抛异常；空输入返回空串。
std::wstring utf8_to_wide(std::string_view utf8);

// 宽字符 → UTF-8。Windows 上把未配对的代理项替换为 U+FFFD。
std::string wide_to_utf8(std::wstring_view wide);

// 先按严格 UTF-8 解码，失败再按系统代码页（Windows: CP_ACP）重新解码，
// 始终返回 UTF-8。用于「第三方输出/老文件编码不确定」的场景。
std::string decode_strict_utf8_or_acp(std::string_view bytes);

// UTF-8 ↔ 路径。Windows 上必须走这一对，中文/emoji 路径才不会在 ANSI 转换中丢失。
std::filesystem::path path_from_utf8(std::string_view utf8);
std::string path_to_utf8(const std::filesystem::path& path);

// 进程启动时调用一次：仅当 stdout 是真实控制台时把输入/输出代码页切到 UTF-8。
// 重定向到文件或管道时不做任何修改（字节仍按 UTF-8 写出）。
void init_console();

// 输出 UTF-8 文本：控制台走宽字符 API，重定向走原始 UTF-8 字节。
void write_stdout(std::string_view utf8);
void write_stderr(std::string_view utf8);

// 系统错误码 → UTF-8 文本（Windows: FormatMessageW；POSIX: strerror）。
// 查不到描述时回退为 "系统错误 <code>"。
std::string system_error_text(std::uint32_t code);

}  // namespace win_encoding
