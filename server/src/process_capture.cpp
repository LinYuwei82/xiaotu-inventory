#include "xiaotu/process_capture.h"

#include <algorithm>
#include <cctype>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace xiaotu {
namespace {

#if defined(_WIN32)

std::wstring utf8_to_wide(const std::string& value) {
  if (value.empty()) {
    return std::wstring();
  }
  const int length = ::MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0);
  if (length <= 0) {
    return std::wstring();
  }
  std::wstring wide(static_cast<std::size_t>(length), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), wide.data(),
                        length);
  return wide;
}

std::string wide_to_utf8(const std::wstring& value) {
  if (value.empty()) {
    return std::string();
  }
  const int length = ::WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0, nullptr,
                                           nullptr);
  if (length <= 0) {
    return std::string();
  }
  std::string text(static_cast<std::size_t>(length), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), text.data(),
                        length, nullptr, nullptr);
  return text;
}

// 取系统错误码的可读描述，拼进启动失败的提示。
// 调用方必须先保存 GetLastError：FormatMessageW 自身会改写 last-error。
std::string windows_error_text(DWORD code) {
  wchar_t* buffer = nullptr;
  const DWORD length = ::FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
  if (length == 0 || buffer == nullptr) {
    return std::string();
  }
  std::wstring wide(buffer, length);
  ::LocalFree(buffer);
  // 系统描述末尾带 \r\n 与空格，去掉后再拼接；同时清理 IGNORE_INSERTS
  // 留下的 %1、%n、%% 等占位符，避免用户看到 “%1 不是有效的 Win32 应用程序”。
  while (!wide.empty() && (wide.back() == L'\r' || wide.back() == L'\n' ||
                           wide.back() == L' ' || wide.back() == L'\t')) {
    wide.pop_back();
  }
  return clean_windows_error_message(wide_to_utf8(wide));
}

// 按 CommandLineToArgvW 的规则给单个参数加引号：反斜杠数量按官方算法翻倍，
// 保证含空格或引号的路径原样传到子进程。
std::wstring quote_windows_arg(const std::wstring& value) {
  if (!value.empty() && value.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
    return value;
  }
  std::wstring quoted = L"\"";
  std::size_t index = 0;
  while (true) {
    std::size_t backslashes = 0;
    while (index < value.size() && value[index] == L'\\') {
      ++index;
      ++backslashes;
    }
    if (index == value.size()) {
      quoted.append(backslashes * 2, L'\\');
      break;
    }
    if (value[index] == L'"') {
      quoted.append(backslashes * 2 + 1, L'\\');
    } else {
      quoted.append(backslashes, L'\\');
    }
    quoted.push_back(value[index]);
    ++index;
  }
  quoted.push_back(L'"');
  return quoted;
}

bool is_batch_script(const std::string& path) {
  std::string extension = path;
  const std::size_t dot = extension.find_last_of('.');
  if (dot != std::string::npos) {
    extension = extension.substr(dot);
  }
  std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return extension == ".bat" || extension == ".cmd";
}

// 轮询读取管道：PeekNamedPipe 无数据时短暂休眠，便于发现取消标志；
// 所有写端关闭后 Peek 返回 FALSE，循环结束。
std::string read_pipe(HANDLE pipe, const std::atomic<bool>* cancel, bool& canceled) {
  std::string text;
  char buffer[4096];
  while (true) {
    DWORD available = 0;
    if (::PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) == FALSE) {
      break;
    }
    if (available > 0) {
      const DWORD want =
          available < sizeof(buffer) ? available : static_cast<DWORD>(sizeof(buffer));
      DWORD read = 0;
      if (::ReadFile(pipe, buffer, want, &read, nullptr) == FALSE || read == 0) {
        break;
      }
      text.append(buffer, read);
      continue;
    }
    if (cancel != nullptr && cancel->load()) {
      canceled = true;
      return text;
    }
    ::Sleep(50);
  }
  return text;
}

#else

// 用 poll 等待可读，避免阻塞在 read 上无法响应取消；100ms 轮询足够及时。
std::string read_fd(int fd, const std::atomic<bool>* cancel, bool& canceled) {
  std::string text;
  char buffer[4096];
  while (true) {
    struct pollfd entry {};
    entry.fd = fd;
    entry.events = POLLIN;
    const int ready = ::poll(&entry, 1, 100);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (ready == 0) {
      if (cancel != nullptr && cancel->load()) {
        canceled = true;
        return text;
      }
      continue;
    }
    const ssize_t read = ::read(fd, buffer, sizeof(buffer));
    if (read < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (read == 0) {
      break;
    }
    text.append(buffer, static_cast<std::size_t>(read));
  }
  return text;
}

#endif

}  // namespace

std::string clean_windows_error_message(const std::string& text) {
  // 1) 去掉 FormatMessageW(IGNORE_INSERTS) 原样留下的占位符与转义。
  //    %1–%9 / %10… 用空格替代，避免前后词粘连；%% 还原为字面 %。
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
      while (end < text.size() && text[end] >= '0' && text[end] <= '9') {
        ++end;
      }
      without.push_back(' ');
      i = end;
      continue;
    }
    if (next == 'n' || next == 'N' || next == 'r' || next == 'R' || next == 't' ||
        next == 'T') {
      without.push_back(' ');
      i += 2;
      continue;
    }
    // 不是占位符（例如 “50%”），原样保留。
    without.push_back('%');
    ++i;
  }

  // 2) 压缩空白：连续空格、制表、换行都压成一个空格，并去掉行首空格。
  std::string collapsed;
  bool pending_space = false;
  for (const char ch : without) {
    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
      pending_space = true;
      continue;
    }
    if (pending_space && !collapsed.empty()) {
      collapsed.push_back(' ');
    }
    pending_space = false;
    collapsed.push_back(ch);
  }

  auto starts_with_at = [](const std::string& value, std::size_t pos, const char* needle) {
    const std::string target(needle);
    return pos + target.size() <= value.size() &&
           value.compare(pos, target.size(), target) == 0;
  };
  auto ends_with = [](const std::string& value, const char* needle) {
    const std::string target(needle);
    return value.size() >= target.size() &&
           value.compare(value.size() - target.size(), target.size(), target) == 0;
  };

  // 3) 去掉标点前的空格、开括号后的空格，避免出现 “： 。” 或 “（ 内容”。
  const char* no_space_before[] = {",", ".", ";", ":", "!", "?", ")", "]", "}", "，",
                                   "。", "；", "：", "！", "？", "、", "）", "】",
                                   "》", "」", "』"};
  const char* no_space_after[] = {"(", "[", "{", "（", "【", "《", "「", "『", ":", "："};
  std::string tight;
  for (std::size_t i = 0; i < collapsed.size(); ++i) {
    const char ch = collapsed[i];
    if (ch == ' ' && !tight.empty()) {
      bool drop = false;
      for (const char* punctuation : no_space_before) {
        if (starts_with_at(collapsed, i + 1, punctuation)) {
          drop = true;
          break;
        }
      }
      if (!drop) {
        for (const char* opening : no_space_after) {
          if (ends_with(tight, opening)) {
            drop = true;
            break;
          }
        }
      }
      if (drop) {
        continue;
      }
    }
    tight.push_back(ch);
  }

  // 4) 占位符被删后可能留下空括号，反复清理直到稳定。
  const char* empty_pairs[] = {"()", "[]", "{}", "（）", "【】", "《》",
                               "「」", "『』", "\"\"", "''"};
  bool changed = true;
  while (changed) {
    changed = false;
    for (const char* pair : empty_pairs) {
      const std::string target(pair);
      std::size_t pos = 0;
      while ((pos = tight.find(target, pos)) != std::string::npos) {
        tight.erase(pos, target.size());
        changed = true;
      }
    }
  }

  // 5) 去掉句首/句尾孤立标点。只删“句首的闭括号”和“句尾的开括号”，
  //    避免把正常配对的 “（…）” 右括号当成孤立标点删掉。
  const char* front_orphans[] = {" ",  "\t", ",", "，", ";", "；", ":", "：", "、",
                                 ")",  "）", "]", "】", "}", "》", "」", "』"};
  const char* back_orphans[] = {" ",  "\t", ",", "，", ";", "；", ":", "：", "、",
                                "(",  "（", "[", "【", "{", "《", "「", "『"};
  bool trimmed = true;
  while (trimmed) {
    trimmed = false;
    for (const char* orphan : front_orphans) {
      const std::string target(orphan);
      if (tight.compare(0, target.size(), target) == 0) {
        tight.erase(0, target.size());
        trimmed = true;
      }
    }
    for (const char* orphan : back_orphans) {
      if (ends_with(tight, orphan)) {
        tight.erase(tight.size() - std::string(orphan).size());
        trimmed = true;
      }
    }
  }

  // 5.1) 占位符夹在标点之间时可能留下 “：。”、“，。” 这类孤立标点。
  //      只删除紧挨句末标点的冒号/逗号/分号，不改变句末标点本身。
  const char* leading_punctuation[] = {":", "：", ",", "，", ";", "；", "、"};
  const char* sentence_end[] = {".", "。", "!", "！", "?", "？"};
  bool punctuation_changed = true;
  while (punctuation_changed) {
    punctuation_changed = false;
    for (const char* leading : leading_punctuation) {
      for (const char* ending : sentence_end) {
        const std::string pattern = std::string(leading) + ending;
        std::size_t pos = 0;
        while ((pos = tight.find(pattern, pos)) != std::string::npos) {
          tight.erase(pos, std::string(leading).size());
          punctuation_changed = true;
        }
      }
    }
  }

  // 6) 删除空括号后可能再次出现连续空格，最后再压一次并去首尾空格。
  std::string cleaned;
  pending_space = false;
  for (const char ch : tight) {
    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
      pending_space = true;
      continue;
    }
    if (pending_space && !cleaned.empty()) {
      cleaned.push_back(' ');
    }
    pending_space = false;
    cleaned.push_back(ch);
  }
  while (!cleaned.empty() && cleaned.front() == ' ') {
    cleaned.erase(cleaned.begin());
  }
  while (!cleaned.empty() && cleaned.back() == ' ') {
    cleaned.pop_back();
  }

  // 7) 整句只剩句末标点（例如 “。”）时，视为没有可用描述。
  std::string without_sentence_end = cleaned;
  bool removed_sentence_end = true;
  while (removed_sentence_end && !without_sentence_end.empty()) {
    removed_sentence_end = false;
    for (const char* ending : sentence_end) {
      if (ends_with(without_sentence_end, ending)) {
        without_sentence_end.erase(without_sentence_end.size() - std::string(ending).size());
        removed_sentence_end = true;
      }
    }
  }
  if (without_sentence_end.empty()) {
    cleaned.clear();
  }
  return cleaned;
}

ProcessResult run_process_capture(const std::string& program,
                                  const std::vector<std::string>& args,
                                  const std::atomic<bool>* cancel) {
  ProcessResult result;
  if (program.empty()) {
    result.error = "程序路径为空";
    return result;
  }
  if (cancel != nullptr && cancel->load()) {
    result.canceled = true;
    result.error = "已取消";
    return result;
  }

#if defined(_WIN32)
  std::wstring command_line = quote_windows_arg(utf8_to_wide(program));
  for (const std::string& arg : args) {
    command_line += L' ';
    command_line += quote_windows_arg(utf8_to_wide(arg));
  }

  std::wstring application = utf8_to_wide(program);
  if (is_batch_script(program)) {
    wchar_t comspec[MAX_PATH] = {};
    const DWORD comspec_length = ::GetEnvironmentVariableW(L"ComSpec", comspec, MAX_PATH);
    application = (comspec_length > 0 && comspec_length < MAX_PATH)
                      ? std::wstring(comspec)
                      : std::wstring(L"C:\\Windows\\System32\\cmd.exe");
    command_line = quote_windows_arg(application) + L" /c \"" + command_line + L"\"";
  } else if (application.find_first_of(L"\\/") == std::wstring::npos) {
    // CreateProcessW 的 lpApplicationName 传纯文件名时不做 PATH 搜索，直接
    // ERROR_FILE_NOT_FOUND（系统错误 2，例如下载用的 "curl"）。纯文件名先解析成
    // 绝对路径；解析不到才原样交给系统。
    wchar_t resolved[MAX_PATH * 4] = {};
    const DWORD resolved_length =
        ::SearchPathW(nullptr, application.c_str(), L".exe", MAX_PATH * 4, resolved, nullptr);
    if (resolved_length > 0 && resolved_length < MAX_PATH * 4) {
      application.assign(resolved, resolved_length);
    }
  }

  SECURITY_ATTRIBUTES inherit{};
  inherit.nLength = sizeof(inherit);
  inherit.bInheritHandle = TRUE;

  HANDLE pipe_read = INVALID_HANDLE_VALUE;
  HANDLE write_pipe = INVALID_HANDLE_VALUE;
  if (::CreatePipe(&pipe_read, &write_pipe, &inherit, 0) == FALSE) {
    result.error = "无法创建输出管道";
    return result;
  }
  ::SetHandleInformation(pipe_read, HANDLE_FLAG_INHERIT, 0);

  // stdin 必须绑定到 NUL：无控制台宿主里 GetStdHandle 拿到的是无效句柄，
  // 读取 stdin 的子进程会异常退出，被误判成业务失败。
  HANDLE null_device =
      ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                    OPEN_EXISTING, 0, nullptr);

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = null_device;
  startup.hStdOutput = write_pipe;
  startup.hStdError = write_pipe;

  PROCESS_INFORMATION process{};
  const BOOL created = ::CreateProcessW(application.c_str(), &command_line[0], nullptr, nullptr,
                                        TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                                        &process);
  // 返回后第一行就保存错误码：下面的 CloseHandle 成功时是否清零 last-error
  // 没有文档保证，失败时还会改成 ERROR_INVALID_HANDLE；取描述用的
  // FormatMessageW 同样会改写它。再晚取就不再是 CreateProcess 的原因。
  const DWORD create_error = ::GetLastError();
  ::CloseHandle(write_pipe);
  if (null_device != INVALID_HANDLE_VALUE) {
    ::CloseHandle(null_device);
  }
  if (created == FALSE) {
    ::CloseHandle(pipe_read);
    const std::string detail = windows_error_text(create_error);
    result.error = "无法启动程序（系统错误 " + std::to_string(create_error) +
                   (detail.empty() ? std::string() : "：" + detail) + "）";
    return result;
  }

  // 先读管道再等待，避免子进程写满管道后阻塞、父进程又在等它退出。
  result.started = true;
  bool canceled = false;
  result.output = read_pipe(pipe_read, cancel, canceled);
  ::CloseHandle(pipe_read);
  if (canceled) {
    // 取消后立即终止 curl，避免继续占用带宽；终止后仍走统一的退出状态收集。
    ::TerminateProcess(process.hProcess, 1);
  }
  ::WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exit_code = 1;
  if (::GetExitCodeProcess(process.hProcess, &exit_code) == FALSE) {
    const DWORD code = ::GetLastError();
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    result.canceled = canceled;
    result.error = "无法获取子进程退出状态（系统错误 " + std::to_string(code) + "）";
    return result;
  }
  ::CloseHandle(process.hThread);
  ::CloseHandle(process.hProcess);
  result.collected = true;
  result.exit_code = static_cast<int>(exit_code);
  result.canceled = canceled;
  return result;

#else
  int pipe_fd[2] = {-1, -1};
  if (::pipe(pipe_fd) != 0) {
    // 先存 errno：后面的字符串构造与拼接不保证不改写它。
    const int saved_errno = errno;
    result.error = std::string("无法创建输出管道：") + std::strerror(saved_errno);
    return result;
  }

  const pid_t pid = ::fork();
  if (pid < 0) {
    // 同上：close() 失败会改写 errno，必须在关闭管道之前保存。
    const int saved_errno = errno;
    ::close(pipe_fd[0]);
    ::close(pipe_fd[1]);
    result.error = std::string("无法创建子进程：") + std::strerror(saved_errno);
    return result;
  }
  // fork 成功后进程已经起来了：后续即使收集退出状态失败，也不能说成「未启动」。
  result.started = true;
  if (pid == 0) {
    ::close(pipe_fd[0]);
    ::dup2(pipe_fd[1], STDOUT_FILENO);
    ::dup2(pipe_fd[1], STDERR_FILENO);
    ::close(pipe_fd[1]);
    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char*>(program.c_str()));
    for (const std::string& arg : args) {
      argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    ::execvp(program.c_str(), argv.data());
    // exec 失败时用固定退出码，父进程只能看到失败，具体原因由启动前的检查兜底。
    _exit(127);
  }

  ::close(pipe_fd[1]);
  bool canceled = false;
  result.output = read_fd(pipe_fd[0], cancel, canceled);
  ::close(pipe_fd[0]);
  if (canceled) {
    // 取消后终止子进程，避免 curl 继续占用带宽；SIGTERM 足够结束 curl。
    ::kill(pid, SIGTERM);
  }

  int status = 0;
  pid_t waited = -1;
  do {
    waited = ::waitpid(pid, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited < 0) {
    // 收集退出状态失败（例如 ECHILD，子进程已被别处回收）。此时 status 仍是
    // 初值 0，若继续往下走会被 WIFEXITED(0) 当成「正常退出、退出码 0」。
    const int saved_errno = errno;
    result.exit_code = -1;
    result.canceled = canceled;
    result.error = std::string("无法获取子进程退出状态：") + std::strerror(saved_errno);
    return result;
  }
  result.collected = true;
  result.canceled = canceled;
  if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    result.exit_code = 128 + WTERMSIG(status);
  }
  return result;
#endif
}

std::string process_failure_text(const ProcessResult& result) {
  if (!result.started) {
    return result.error.empty() ? "程序未能启动" : result.error;
  }
  return "进程已启动，但无法获取退出状态：" +
         (result.error.empty() ? std::string("原因未知") : result.error);
}

std::string last_output_line(const std::string& output) {
  std::string line;
  std::size_t end = output.size();
  while (end > 0) {
    const std::size_t start = output.find_last_of('\n', end - 1);
    const std::size_t from = start == std::string::npos ? 0 : start + 1;
    line = output.substr(from, end - from);
    // 去掉行尾 \r 与空白，只有空行时继续往上找。
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
      line.pop_back();
    }
    if (!line.empty()) {
      // 去掉行首空白。
      const std::size_t first = line.find_first_not_of(" \t");
      return first == std::string::npos ? std::string() : line.substr(first);
    }
    if (start == std::string::npos) {
      break;
    }
    end = start;
  }
  return {};
}

std::string compact_output(const std::string& output, std::size_t max_bytes) {
  std::string text;
  for (const char ch : output) {
    if (ch == '\r') {
      continue;
    }
    text.push_back(ch == '\n' ? ' ' : ch);
  }
  const std::size_t first = text.find_first_not_of(" \t");
  text = first == std::string::npos ? std::string() : text.substr(first);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
    text.pop_back();
  }
  if (text.size() > max_bytes) {
    text.resize(max_bytes);
    text += "…";
  }
  return text;
}

}  // namespace xiaotu
