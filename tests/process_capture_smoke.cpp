#include "xiaotu/process_capture.h"

#include <iostream>
#include <string>

// 用不存在的程序名启动子进程，验证失败原因可用。
// Windows：CreateProcessW 直接失败，必须 started=false 且带系统错误码；
// POSIX：fork 成功后由子进程 execvp 失败，按既有约定以 127 退出。
int main() {
  int failures = 0;
  const xiaotu::ProcessResult result =
      xiaotu::run_process_capture("xiaotu-no-such-program-20261113", {"--version"});

#if defined(_WIN32)
  if (result.started) {
    std::cerr << "不存在的程序不应启动成功" << std::endl;
    ++failures;
  }
  if (result.error.empty()) {
    std::cerr << "启动失败必须给出原因" << std::endl;
    ++failures;
  }
  // 纯文件名找不到时 CreateProcessW 稳定返回 ERROR_FILE_NOT_FOUND(2)，
  // 断言到具体的系统错误码，防止以后又把错误码取晚或拼错文案。
  if (result.error.find("系统错误 2") == std::string::npos) {
    std::cerr << "错误码应为 ERROR_FILE_NOT_FOUND(2)，实际：" << result.error << std::endl;
    ++failures;
  }
#else
  if (!result.started || !result.collected || result.exit_code != 127) {
    std::cerr << "exec 失败应以 127 退出，实际 started=" << result.started
              << " collected=" << result.collected << " exit_code=" << result.exit_code
              << std::endl;
    ++failures;
  }
#endif

  // 下面两组断言守的是「未启动」与「已启动但收集失败」必须可区分这一语义，
  // 不锁定整句措辞：未启动只要求原样透出底层错误；收集失败只要求带「已启动」
  // 语义标记、不出现「无法启动」，并保留底层错误原文。以后改文案只要两种
  // 失败仍能区分，这里就不该误红。
  {
    xiaotu::ProcessResult not_started;
    not_started.error = "无法启动程序（系统错误 2：找不到文件）";
    const std::string text = xiaotu::process_failure_text(not_started);
    if (text != not_started.error) {
      std::cerr << "未启动应原样给出系统错误描述，实际：" << text << std::endl;
      ++failures;
    }
  }
  {
    xiaotu::ProcessResult started_not_collected;
    started_not_collected.started = true;
    started_not_collected.error = "无法获取子进程退出状态：No child processes";
    const std::string text = xiaotu::process_failure_text(started_not_collected);
    if (text.find("进程已启动") == std::string::npos ||
        text.find("无法启动") != std::string::npos ||
        text.find("No child processes") == std::string::npos) {
      std::cerr << "收集失败文案不正确：" << text << std::endl;
      ++failures;
    }
  }

  if (failures == 0) {
    std::cout << "process_capture_smoke 全部通过" << std::endl;
  }
  return failures == 0 ? 0 : 1;
}
