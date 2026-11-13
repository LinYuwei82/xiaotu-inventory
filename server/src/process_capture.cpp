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
  // 系统描述末尾带 \r\n 与空格，去掉后再拼进括号。
  while (!wide.empty() && (wide.back() == L'\r' || wide.back() == L'\n' ||
                           wide.back() == L' ' || wide.back() == L'\t')) {
    wide.pop_back();
  }
  return wide_to_utf8(wide);
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

std::string read_pipe(HANDLE pipe) {
  std::string text;
  char buffer[4096];
  DWORD read = 0;
  while (::ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
    text.append(buffer, read);
  }
  return text;
}

#else

std::string read_fd(int fd) {
  std::string text;
  char buffer[4096];
  ssize_t read = 0;
  while ((read = ::read(fd, buffer, sizeof(buffer))) > 0) {
    text.append(buffer, static_cast<std::size_t>(read));
  }
  return text;
}

#endif

}  // namespace

ProcessResult run_process_capture(const std::string& program,
                                  const std::vector<std::string>& args) {
  ProcessResult result;
  if (program.empty()) {
    result.error = "程序路径为空";
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
  result.output = read_pipe(pipe_read);
  ::CloseHandle(pipe_read);
  ::WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exit_code = 1;
  ::GetExitCodeProcess(process.hProcess, &exit_code);
  ::CloseHandle(process.hThread);
  ::CloseHandle(process.hProcess);
  result.started = true;
  result.exit_code = static_cast<int>(exit_code);
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
  result.output = read_fd(pipe_fd[0]);
  ::close(pipe_fd[0]);

  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  result.started = true;
  if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    result.exit_code = 128 + WTERMSIG(status);
  }
  return result;
#endif
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
