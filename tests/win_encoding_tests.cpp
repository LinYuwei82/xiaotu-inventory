// win_encoding 单测：跨平台可跑（macOS 走 UTF-8/UTF-32 实现，Windows 走 Win32）。
// 覆盖：严格 UTF-8 校验、宽窄往返、ACP 兜底、路径往返、系统错误文本清理、
//       stdout 重定向后的字节输出、GBK 文件 → UTF-8 的端到端读入。

#include "win_encoding/encoding.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

int g_failures = 0;

void check(bool condition, const char *expr, const char *file, int line)
{
    if (condition)
        return;
    std::printf("FAIL %s:%d  %s\n", file, line, expr);
    ++g_failures;
}

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)

std::string bytes(std::initializer_list<unsigned char> values)
{
    std::string out;
    out.reserve(values.size());
    for (const unsigned char value : values)
        out.push_back(static_cast<char>(value));
    return out;
}

std::string read_all_bytes(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void test_valid_utf8()
{
    CHECK(win_encoding::is_valid_utf8(""));
    CHECK(win_encoding::is_valid_utf8("plain ascii"));
    CHECK(win_encoding::is_valid_utf8(bytes({0xE4, 0xB8, 0xAD})));             // 中
    CHECK(win_encoding::is_valid_utf8(bytes({0xF0, 0x9F, 0x98, 0x80})));       // 😀
    CHECK(!win_encoding::is_valid_utf8(bytes({0xE4, 0xB8})));                  // 截断
    CHECK(!win_encoding::is_valid_utf8(bytes({0xC0, 0xAF})));                  // 过长编码
    CHECK(!win_encoding::is_valid_utf8(bytes({0xE0, 0x80, 0xAF})));            // 过长编码
    CHECK(!win_encoding::is_valid_utf8(bytes({0xED, 0xA0, 0x80})));            // 代理区
    CHECK(!win_encoding::is_valid_utf8(bytes({0xF4, 0x90, 0x80, 0x80})));      // > U+10FFFF
    CHECK(!win_encoding::is_valid_utf8(bytes({0x80})));                        // 孤立续字节
    CHECK(!win_encoding::is_valid_utf8(bytes({0xFF})));
}

void test_roundtrip()
{
    const std::string samples[] = {
        "",
        "plain ascii",
        "中文目录/文件.txt",
        "简体中文 + 繁體中文 + 日本語 + 한국어",
        "emoji 😀🎉 与混合 abc123",
    };
    for (const std::string &sample : samples) {
        const std::wstring wide = win_encoding::utf8_to_wide(sample);
        CHECK(win_encoding::wide_to_utf8(wide) == sample);
    }
    CHECK(win_encoding::utf8_to_wide("").empty());
    CHECK(win_encoding::wide_to_utf8(std::wstring_view()).empty());
}

void test_decode_strict_or_acp()
{
    // 合法 UTF-8 原样返回。
    const std::string utf8 = "中文";
    CHECK(win_encoding::decode_strict_utf8_or_acp(utf8) == utf8);
    CHECK(win_encoding::decode_strict_utf8_or_acp("").empty());

    // 非 UTF-8 字节必须被兜底成合法 UTF-8，且不再等于原字节。
    const std::string gbk = bytes({0xD6, 0xD0, 0xCE, 0xC4}); // 「中文」的 GBK 编码
    const std::string converted = win_encoding::decode_strict_utf8_or_acp(gbk);
    CHECK(!converted.empty());
    CHECK(win_encoding::is_valid_utf8(converted));
#if defined(_WIN32)
    // 仅中文 Windows（ACP=936）能按 GBK 还原；其它代码页只要求是合法 UTF-8。
    if (GetACP() == 936) {
        CHECK(converted == "中文");
    }
#endif
}

void test_gbk_file_pipeline()
{
    // 端到端：GBK 文档落盘 → 按字节读入 → 走与 mote 打开老文档相同的兜底解码。
    const std::filesystem::path file =
        std::filesystem::temp_directory_path() / "win_encoding_gbk_sample.txt";
    std::filesystem::remove(file);

    const std::string gbk = bytes({0xD6, 0xD0, 0xCE, 0xC4}); // 「中文」
    {
        std::FILE *fp = std::fopen(file.string().c_str(), "wb");
        CHECK(fp != nullptr);
        if (fp == nullptr)
            return;
        const std::size_t written = std::fwrite(gbk.data(), 1, gbk.size(), fp);
        std::fclose(fp);
        CHECK(written == gbk.size());
    }

    const std::string raw = read_all_bytes(file);
    CHECK(raw == gbk);
    CHECK(!win_encoding::is_valid_utf8(raw)); // 原始 GBK 不是合法 UTF-8

    const std::string text = win_encoding::decode_strict_utf8_or_acp(raw);
    CHECK(!text.empty());
    CHECK(win_encoding::is_valid_utf8(text));
#if defined(_WIN32)
    if (GetACP() == 936) {
        CHECK(text == "中文");
    }
#endif
    std::filesystem::remove(file);
}

void test_write_stdout_redirect()
{
    // 重定向场景：stdout 不是控制台时，模块必须输出原始 UTF-8 字节而不是走宽字符 API。
    const std::filesystem::path file =
        std::filesystem::temp_directory_path() / "win_encoding_stdout.txt";
    std::filesystem::remove(file);

    const std::string payload = "重定向输出 😀 ok";

#if defined(_WIN32)
    HANDLE handle = ::CreateFileW(file.wstring().c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(handle != INVALID_HANDLE_VALUE);
    if (handle == INVALID_HANDLE_VALUE)
        return;
    HANDLE saved = ::GetStdHandle(STD_OUTPUT_HANDLE);
    ::SetStdHandle(STD_OUTPUT_HANDLE, handle);
    win_encoding::write_stdout(payload);
    ::SetStdHandle(STD_OUTPUT_HANDLE, saved);
    ::CloseHandle(handle);
#else
    std::fflush(stdout);
    const int saved = ::dup(STDOUT_FILENO);
    CHECK(saved >= 0);
    if (saved < 0)
        return;
    const int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    if (fd < 0) {
        ::close(saved);
        return;
    }
    ::dup2(fd, STDOUT_FILENO);
    ::close(fd);
    win_encoding::write_stdout(payload);
    ::dup2(saved, STDOUT_FILENO);
    ::close(saved);
#endif

    CHECK(read_all_bytes(file) == payload);
    std::filesystem::remove(file);
}

void test_paths()
{
    const std::string samples[] = {
        "中文目录/文件.txt",
        "C:/Users/测试/文档/报表.xlsx",
        "emoji-😀/note.md",
    };
    for (const std::string &sample : samples) {
        const std::filesystem::path path = win_encoding::path_from_utf8(sample);
        CHECK(!path.empty());
        CHECK(win_encoding::path_to_utf8(path) == sample);
    }
    CHECK(win_encoding::path_from_utf8("").empty());
}

void test_system_error_text()
{
    // 覆盖存在占位符的码（193 = ERROR_BAD_EXE_FORMAT，描述里带 %1）、
    // 常规错误码与未知码（走 “系统错误 N” 兜底）。
    const std::uint32_t codes[] = {2u, 3u, 193u, 0xFFFFFFFFu};
    for (const std::uint32_t code : codes) {
        const std::string text = win_encoding::system_error_text(code);
        CHECK(!text.empty());
        CHECK(win_encoding::is_valid_utf8(text));
        CHECK(text.find('%') == std::string::npos);  // 占位符已清理
        CHECK(text.find("  ") == std::string::npos); // 空白已压缩
        const char tail = text.back();
        CHECK(tail != ' ' && tail != '\n' && tail != '\r' && tail != '.' && tail != ','
              && tail != ';' && tail != ':');
    }
#if defined(_WIN32)
    // 空括号是占位符被删后的典型残留。
    const std::string bad_exe = win_encoding::system_error_text(193);
    CHECK(bad_exe.find("()") == std::string::npos);
#endif
}

} // namespace

int main()
{
    test_valid_utf8();
    test_roundtrip();
    test_decode_strict_or_acp();
    test_paths();
    test_system_error_text();
    test_write_stdout_redirect();
    test_gbk_file_pipeline();

    if (g_failures != 0) {
        std::printf("win_encoding 单测失败：%d 项\n", g_failures);
        return 1;
    }
    std::printf("win_encoding 单测通过\n");
    return 0;
}
