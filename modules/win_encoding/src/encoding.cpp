#include "win_encoding/encoding.h"

#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace win_encoding {
namespace {

constexpr char32_t kReplacement = 0xFFFD;

// 严格解码一个 UTF-8 码点：拒绝过长编码、代理区、> U+10FFFF、截断序列。
// 成功时回填码点与字节宽度。
bool decode_one(std::string_view text, std::size_t index, char32_t *code, std::size_t *width)
{
    const auto *bytes = reinterpret_cast<const unsigned char *>(text.data());
    const std::size_t size = text.size();
    const unsigned char lead = bytes[index];
    if (lead < 0x80) {
        *code = lead;
        *width = 1;
        return true;
    }

    std::size_t extra = 0;
    char32_t value = 0;
    unsigned char min_second = 0x80;
    unsigned char max_second = 0xBF;
    if ((lead & 0xE0) == 0xC0) {
        extra = 1;
        value = lead & 0x1F;
        if (lead < 0xC2)
            return false; // 过长编码 C0 / C1
    } else if ((lead & 0xF0) == 0xE0) {
        extra = 2;
        value = lead & 0x0F;
        if (lead == 0xE0)
            min_second = 0xA0; // 过长编码
        if (lead == 0xED)
            max_second = 0x9F; // UTF-16 代理区
    } else if ((lead & 0xF8) == 0xF0) {
        extra = 3;
        value = lead & 0x07;
        if (lead == 0xF0)
            min_second = 0x90; // 过长编码
        if (lead == 0xF4)
            max_second = 0x8F; // 超过 U+10FFFF
        if (lead > 0xF4)
            return false;
    } else {
        return false;
    }

    if (index + extra >= size)
        return false; // 截断序列

    for (std::size_t k = 1; k <= extra; ++k) {
        const unsigned char byte = bytes[index + k];
        if (k == 1) {
            if (byte < min_second || byte > max_second)
                return false;
        } else if ((byte & 0xC0) != 0x80) {
            return false;
        }
        value = (value << 6) | (byte & 0x3F);
    }

    *code = value;
    *width = extra + 1;
    return true;
}

void append_utf8(std::string *out, char32_t code)
{
    if (code < 0x80) {
        out->push_back(static_cast<char>(code));
    } else if (code < 0x800) {
        out->push_back(static_cast<char>(0xC0 | (code >> 6)));
        out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
        out->push_back(static_cast<char>(0xE0 | (code >> 12)));
        out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
        out->push_back(static_cast<char>(0xF0 | (code >> 18)));
        out->push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
}

#if !defined(_WIN32)
// POSIX 侧：非法字节替换为 U+FFFD，保证结果可继续参与编码。
std::u32string decode_utf8_lossy(std::string_view text)
{
    std::u32string out;
    std::size_t index = 0;
    while (index < text.size()) {
        char32_t code = 0;
        std::size_t width = 0;
        if (decode_one(text, index, &code, &width)) {
            out.push_back(code);
            index += width;
        } else {
            out.push_back(kReplacement);
            ++index;
        }
    }
    return out;
}
#endif

#if defined(_WIN32)
// 控制台 / 重定向统一出口：控制台按 UTF-16 写出，重定向写原始 UTF-8 字节。
void write_stream(unsigned long which, std::string_view utf8)
{
    HANDLE handle = ::GetStdHandle(which);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
        return;
    DWORD mode = 0;
    if (::GetConsoleMode(handle, &mode)) {
        const std::wstring wide = utf8_to_wide(utf8);
        DWORD written = 0;
        ::WriteConsoleW(handle, wide.data(), static_cast<DWORD>(wide.size()), &written, nullptr);
        return;
    }
    DWORD written = 0;
    ::WriteFile(handle, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
}
#endif

} // namespace

bool is_valid_utf8(std::string_view utf8)
{
    if (utf8.empty())
        return true;
#if defined(_WIN32)
    return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                 static_cast<int>(utf8.size()), nullptr, 0) > 0;
#else
    std::size_t index = 0;
    while (index < utf8.size()) {
        char32_t code = 0;
        std::size_t width = 0;
        if (!decode_one(utf8, index, &code, &width))
            return false;
        index += width;
    }
    return true;
#endif
}

std::wstring utf8_to_wide(std::string_view utf8)
{
    if (utf8.empty())
        return std::wstring();
#if defined(_WIN32)
    const int length = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                             static_cast<int>(utf8.size()), nullptr, 0);
    if (length <= 0)
        return std::wstring();
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                          wide.data(), length);
    return wide;
#else
    static_assert(sizeof(wchar_t) == 4, "POSIX 侧按 wchar_t = UTF-32 实现");
    const std::u32string codes = decode_utf8_lossy(utf8);
    return std::wstring(reinterpret_cast<const wchar_t *>(codes.data()), codes.size());
#endif
}

std::string wide_to_utf8(std::wstring_view wide)
{
    if (wide.empty())
        return std::string();
#if defined(_WIN32)
    const int length = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                             static_cast<int>(wide.size()), nullptr, 0,
                                             nullptr, nullptr);
    if (length <= 0)
        return std::string();
    std::string utf8(static_cast<std::size_t>(length), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                          utf8.data(), length, nullptr, nullptr);
    return utf8;
#else
    std::string utf8;
    utf8.reserve(wide.size());
    for (const wchar_t unit : wide)
        append_utf8(&utf8, static_cast<char32_t>(unit));
    return utf8;
#endif
}

std::string decode_strict_utf8_or_acp(std::string_view bytes)
{
    if (bytes.empty())
        return std::string();
    if (is_valid_utf8(bytes))
        return std::string(bytes);
#if defined(_WIN32)
    const int length = ::MultiByteToWideChar(CP_ACP, 0, bytes.data(),
                                             static_cast<int>(bytes.size()), nullptr, 0);
    if (length <= 0)
        return std::string();
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    ::MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()),
                          wide.data(), length);
    return wide_to_utf8(wide);
#else
    // POSIX 无 ACP：按 Latin-1 逐字节映射，保证输出一定是合法 UTF-8。
    std::string utf8;
    utf8.reserve(bytes.size() * 2);
    for (const char byte : bytes)
        append_utf8(&utf8, static_cast<char32_t>(static_cast<unsigned char>(byte)));
    return utf8;
#endif
}

std::filesystem::path path_from_utf8(std::string_view utf8)
{
    if (utf8.empty())
        return std::filesystem::path();
#if defined(_WIN32)
    return std::filesystem::path(utf8_to_wide(utf8));
#else
    return std::filesystem::path(std::string(utf8));
#endif
}

std::string path_to_utf8(const std::filesystem::path &path)
{
#if defined(_WIN32)
    return wide_to_utf8(path.wstring());
#else
    return path.string();
#endif
}

void init_console()
{
#if defined(_WIN32)
    HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    // 只有句柄真的是控制台才切代码页；重定向到文件/管道时字节必须保持原样。
    if (out == nullptr || out == INVALID_HANDLE_VALUE || !::GetConsoleMode(out, &mode))
        return;
    ::SetConsoleOutputCP(CP_UTF8);
    HANDLE in = ::GetStdHandle(STD_INPUT_HANDLE);
    if (in != nullptr && in != INVALID_HANDLE_VALUE && ::GetConsoleMode(in, &mode))
        ::SetConsoleCP(CP_UTF8);
#endif
}

void write_stdout(std::string_view utf8)
{
#if defined(_WIN32)
    write_stream(STD_OUTPUT_HANDLE, utf8);
#else
    if (!utf8.empty())
        std::fwrite(utf8.data(), 1, utf8.size(), stdout);
    std::fflush(stdout);
#endif
}

void write_stderr(std::string_view utf8)
{
#if defined(_WIN32)
    write_stream(STD_ERROR_HANDLE, utf8);
#else
    if (!utf8.empty())
        std::fwrite(utf8.data(), 1, utf8.size(), stderr);
    std::fflush(stderr);
#endif
}

#if defined(_WIN32)
// 清理 FormatMessageW(FORMAT_MESSAGE_IGNORE_INSERTS) 的产物：
//   1) 去掉 %1 / %n / %r / %t 等占位符（用空格替代，避免词粘连），%% 还原为字面 %；
//   2) 压缩连续空白；
//   3) 删除占位符消失后留下的空括号，反复清理直到稳定；
//   4) 去掉首尾空白与句末标点（系统描述常以 “。” 或 “.” 结尾）。
std::string clean_error_text(const std::string &text)
{
    std::string without;
    without.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] != '%') {
            without.push_back(text[i++]);
            continue;
        }
        if (i + 1 >= text.size()) {
            without.push_back('%');
            ++i;
            continue;
        }
        const char next = text[i + 1];
        if (next == '%') {
            without.push_back('%');
            i += 2;
            continue;
        }
        if (next >= '0' && next <= '9') {
            std::size_t end = i + 1;
            while (end < text.size() && text[end] >= '0' && text[end] <= '9')
                ++end;
            without.push_back(' ');
            i = end;
            continue;
        }
        if (next == 'n' || next == 'N' || next == 'r' || next == 'R' || next == 't'
            || next == 'T') {
            without.push_back(' ');
            i += 2;
            continue;
        }
        // 不是占位符（例如 “50%”），原样保留。
        without.push_back('%');
        ++i;
    }

    std::string collapsed;
    bool pending_space = false;
    for (const char ch : without) {
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            pending_space = true;
            continue;
        }
        if (pending_space && !collapsed.empty())
            collapsed.push_back(' ');
        pending_space = false;
        collapsed.push_back(ch);
    }

    const char *empty_pairs[] = {"()",  "[]",  "{}",  "（）", "【】",
                                 "《》", "「」", "『』", "\"\"", "''"};
    bool changed = true;
    while (changed) {
        changed = false;
        for (const char *pair : empty_pairs) {
            const std::string target(pair);
            std::size_t pos = 0;
            while ((pos = collapsed.find(target, pos)) != std::string::npos) {
                collapsed.erase(pos, target.size());
                changed = true;
            }
        }
    }

    bool trimmed = true;
    while (trimmed && !collapsed.empty()) {
        trimmed = false;
        while (!collapsed.empty()) {
            const char tail = collapsed.back();
            if (tail == ' ' || tail == '\t' || tail == '\r' || tail == '\n' || tail == '.'
                || tail == ',' || tail == ';' || tail == ':') {
                collapsed.pop_back();
                trimmed = true;
                continue;
            }
            break;
        }
        for (const char *mark : {"。", "，", "；", "：", "！", "？"}) {
            const std::string suffix(mark);
            if (collapsed.size() >= suffix.size()
                && collapsed.compare(collapsed.size() - suffix.size(), suffix.size(), suffix)
                       == 0) {
                collapsed.erase(collapsed.size() - suffix.size());
                trimmed = true;
                break;
            }
        }
    }
    return collapsed;
}
#endif // _WIN32

std::string system_error_text(std::uint32_t code)
{
#if defined(_WIN32)
    wchar_t *buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(code),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::string text;
    if (length > 0 && buffer != nullptr)
        text = clean_error_text(wide_to_utf8(std::wstring(buffer, length)));
    if (buffer != nullptr)
        ::LocalFree(buffer);
    if (text.empty())
        text = "系统错误 " + std::to_string(code);
    return text;
#else
    const char *text = std::strerror(static_cast<int>(code));
    if (text != nullptr && *text != '\0')
        return std::string(text);
    return "系统错误 " + std::to_string(code);
#endif
}

} // namespace win_encoding
