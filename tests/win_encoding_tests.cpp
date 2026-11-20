// win_encoding 单测：跨平台可跑（macOS 走 UTF-8/UTF-32 实现，Windows 走 Win32）。
// 覆盖：严格 UTF-8 校验、宽窄往返、ACP 兜底、路径往返、系统错误文本。

#include "win_encoding/encoding.h"

#include <cstdio>
#include <string>

#if defined(_WIN32)
#include <windows.h>
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
    const std::string text = win_encoding::system_error_text(2); // ERROR_FILE_NOT_FOUND / ENOENT
    CHECK(!text.empty());
    CHECK(win_encoding::is_valid_utf8(text));
}

} // namespace

int main()
{
    test_valid_utf8();
    test_roundtrip();
    test_decode_strict_or_acp();
    test_paths();
    test_system_error_text();

    if (g_failures != 0) {
        std::printf("win_encoding 单测失败：%d 项\n", g_failures);
        return 1;
    }
    std::printf("win_encoding 单测通过\n");
    return 0;
}
