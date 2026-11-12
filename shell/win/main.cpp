// 小兔进销存 Windows 桌面壳
//
// 职责：拉起同目录的 xiaotu-server.exe（单机形态），从它的标准输出里取出
// 一次性访问地址，用 WebView2 显示。关窗时终止子进程，不留孤儿。
//
// 输出解析只用 ASCII 标记（"http://127.0.0.1:"、"/#t="），不依赖中文，
// 因此服务端以 UTF-8 或 GBK 输出都能正确识别；只有给用户看错误信息时才做编码嗅探。

#include <windows.h>

// WIN32_LEAN_AND_MEAN 下 windows.h 不带 basetyps.h，而 WebView2.h 用到其中的
// interface 宏与 COM 声明，必须显式包含。
#include <objbase.h>
// CommandLineToArgvW
#include <shellapi.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "WebView2.h"

namespace {

constexpr wchar_t kWindowClass[] = L"XiaotuShellWindow";
constexpr wchar_t kWindowTitle[] = L"小兔进销存";
constexpr UINT kMsgServerUrl = WM_APP + 1;
constexpr UINT kMsgServerExited = WM_APP + 2;
constexpr UINT_PTR kTimerStartup = 1;
constexpr UINT kDefaultStartupTimeoutMs = 30000;
constexpr UINT kMinStartupTimeoutMs = 1000;
constexpr UINT kMaxStartupTimeoutMs = 600000;
constexpr int kDefaultWidth = 1280;
constexpr int kDefaultHeight = 840;

// 单实例：Local\ 前缀表示"当前登录会话"，同一台机器上不同用户（含 RDP 会话）
// 各有自己的实例，不会互相挤掉；不要换成 Global\。
constexpr wchar_t kInstanceMutexName[] = L"Local\\xiaotu-shell-single-instance";
constexpr wchar_t kWindowStateFileName[] = L"shell-window.txt";

struct AppState {
  HWND hwnd = nullptr;
  HANDLE job = nullptr;            // 关句柄即杀子进程，避免孤儿
  HANDLE child = nullptr;          // 子进程句柄
  HANDLE stdout_read = nullptr;    // 子进程 stdout/stderr 读端
  std::thread reader;

  ICoreWebView2Environment* env = nullptr;
  ICoreWebView2Controller* controller = nullptr;
  ICoreWebView2* webview = nullptr;

  std::wstring pending_url;
  bool controller_ready = false;
  bool navigated = false;
  std::atomic<bool> stopping{false};

  std::mutex output_mutex;
  std::string output_bytes;        // 子进程输出（原始字节，用于排错）
};

AppState g_app;

// 首屏等待上限：命令行 --startup-timeout <秒> > 环境变量 XIAOTU_SHELL_STARTUP_TIMEOUT > 默认 30 秒。
UINT g_startup_timeout_ms = kDefaultStartupTimeoutMs;
// 单实例互斥体句柄，进程存活期间一直持有。
HANDLE g_instance_mutex = nullptr;

// ---------- 文本与路径 ----------

// 服务端可能输出 UTF-8 或 GBK：先按 UTF-8 严格解码，失败再按系统代码页。
std::wstring decode_output(const std::string& bytes) {
  if (bytes.empty()) return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                              static_cast<int>(bytes.size()), nullptr, 0);
  UINT cp = CP_UTF8;
  if (n <= 0) {
    n = MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    cp = CP_ACP;
  }
  if (n <= 0) return std::wstring();
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(cp, 0, bytes.data(), static_cast<int>(bytes.size()), out.data(), n);
  return out;
}

std::wstring exe_directory() {
  std::vector<wchar_t> buf(MAX_PATH);
  for (;;) {
    DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (len == 0) return std::wstring();
    if (len < buf.size() - 1) {
      std::wstring path(buf.data(), len);
      size_t pos = path.find_last_of(L'\\');
      return pos == std::wstring::npos ? std::wstring() : path.substr(0, pos);
    }
    buf.resize(buf.size() * 2);
  }
}

void show_error(const std::wstring& text, UINT icon = MB_ICONERROR) {
  MessageBoxW(g_app.hwnd, text.c_str(), kWindowTitle, MB_OK | icon);
}

std::wstring output_tail(size_t limit = 1200) {
  std::lock_guard<std::mutex> lock(g_app.output_mutex);
  if (g_app.output_bytes.size() <= limit) return decode_output(g_app.output_bytes);
  return decode_output(g_app.output_bytes.substr(g_app.output_bytes.size() - limit));
}

// ---------- 路径、启动参数与窗口状态 ----------

// 与服务端同一域：%LOCALAPPDATA%\com.simem.xiaotu
std::wstring app_data_dir() {
  wchar_t buf[MAX_PATH];
  DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
  std::wstring base = (len > 0 && len < MAX_PATH) ? std::wstring(buf, len) : exe_directory();
  return base + L"\\com.simem.xiaotu";
}

void ensure_directory(const std::wstring& dir) {
  size_t pos = dir.find(L'\\');
  while (pos != std::wstring::npos) {
    CreateDirectoryW(dir.substr(0, pos).c_str(), nullptr);
    pos = dir.find(L'\\', pos + 1);
  }
  CreateDirectoryW(dir.c_str(), nullptr);
}

bool parse_timeout_seconds(const wchar_t* text, UINT* out_ms) {
  if (!text || !*text) return false;
  wchar_t* end = nullptr;
  long value = wcstol(text, &end, 10);
  if (end == text || *end != L'\0') return false;
  if (value <= 0 || value > static_cast<long>(kMaxStartupTimeoutMs / 1000)) return false;
  UINT ms = static_cast<UINT>(value) * 1000;
  if (ms < kMinStartupTimeoutMs) return false;
  *out_ms = ms;
  return true;
}

// 优先级：--startup-timeout <秒> > XIAOTU_SHELL_STARTUP_TIMEOUT > 默认 30 秒。
void resolve_startup_timeout() {
  UINT ms = 0;
  wchar_t env[32] = {};
  DWORD n = GetEnvironmentVariableW(L"XIAOTU_SHELL_STARTUP_TIMEOUT", env, 32);
  if (n > 0 && n < 32 && parse_timeout_seconds(env, &ms)) g_startup_timeout_ms = ms;

  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) return;
  for (int i = 1; i < argc; ++i) {
    if (wcscmp(argv[i], L"--startup-timeout") == 0 && i + 1 < argc) {
      if (parse_timeout_seconds(argv[i + 1], &ms)) {
        g_startup_timeout_ms = ms;
      } else {
        std::wstring msg = L"--startup-timeout 参数无效：";
        msg += argv[i + 1];
        msg += L"\n应为 1–600 的秒数，已沿用当前值 "
               + std::to_wstring(g_startup_timeout_ms / 1000) + L" 秒。";
        MessageBoxW(nullptr, msg.c_str(), kWindowTitle, MB_OK | MB_ICONWARNING);
      }
      ++i;
    }
  }
  LocalFree(argv);
}

struct WindowState {
  RECT rc{};
  bool maximized = false;
  bool valid = false;
};

// 只接受"至少 200x100 落在某个显示器工作区内"的矩形，换显示器/分辨率变小后自动回退默认值。
bool visible_on_monitor(const RECT& rc) {
  if (rc.right - rc.left < 400 || rc.bottom - rc.top < 300) return false;
  HMONITOR monitor = MonitorFromRect(&rc, MONITOR_DEFAULTTONULL);
  if (!monitor) return false;
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  if (!GetMonitorInfoW(monitor, &info)) return false;
  RECT inter{};
  if (!IntersectRect(&inter, &rc, &info.rcWork)) return false;
  return (inter.right - inter.left) >= 200 && (inter.bottom - inter.top) >= 100;
}

WindowState load_window_state() {
  WindowState state;
  const std::wstring path = app_data_dir() + L"\\" + kWindowStateFileName;
  FILE* file = nullptr;
  if (_wfopen_s(&file, path.c_str(), L"r, ccs=UTF-8") != 0 || file == nullptr) return state;
  wchar_t line[128] = {};
  if (fgetws(line, 128, file)) {
    long x = 0, y = 0, w = 0, h = 0;
    int maximized = 0;
    if (swscanf_s(line, L"%ld %ld %ld %ld %d", &x, &y, &w, &h, &maximized) == 5) {
      RECT rc{x, y, x + w, y + h};
      if (visible_on_monitor(rc)) {
        state.rc = rc;
        state.maximized = maximized != 0;
        state.valid = true;
      }
    }
  }
  fclose(file);
  return state;
}

void save_window_state(HWND hwnd) {
  if (!hwnd) return;
  WINDOWPLACEMENT placement{};
  placement.length = sizeof(placement);
  if (!GetWindowPlacement(hwnd, &placement)) return;
  // 最大化时用 rcNormalPosition，恢复后仍回到原来的大小与位置。
  const RECT rc = placement.rcNormalPosition;
  const int maximized = (placement.showCmd == SW_SHOWMAXIMIZED) ? 1 : 0;

  const std::wstring dir = app_data_dir();
  ensure_directory(dir);
  const std::wstring path = dir + L"\\" + kWindowStateFileName;
  FILE* file = nullptr;
  if (_wfopen_s(&file, path.c_str(), L"w, ccs=UTF-8") != 0 || file == nullptr) return;
  fwprintf(file, L"%ld %ld %ld %ld %d\n", rc.left, rc.top, rc.right - rc.left,
           rc.bottom - rc.top, maximized);
  fclose(file);
}

// 第二个实例：把已有窗口提到前台后退出。
bool activate_existing_instance() {
  HWND existing = FindWindowW(kWindowClass, nullptr);
  if (!existing) return false;
  ShowWindow(existing, IsIconic(existing) ? SW_RESTORE : SW_SHOW);
  SetForegroundWindow(existing);
  return true;
}

// ---------- 子进程 ----------

bool launch_server(std::wstring* error) {
  const std::wstring dir = exe_directory();
  if (dir.empty()) {
    *error = L"无法确定程序所在目录。";
    return false;
  }
  const std::wstring exe = dir + L"\\xiaotu-server.exe";
  if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
    *error = L"与 xiaotu-shell.exe 同目录下找不到 xiaotu-server.exe。\n\n"
             L"请把壳放在服务端程序旁边（例如解压后的分发包目录）再运行。";
    return false;
  }

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE read_end = nullptr;
  HANDLE write_end = nullptr;
  if (!CreatePipe(&read_end, &write_end, &sa, 0)) {
    *error = L"创建输出管道失败。";
    return false;
  }
  SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

  HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                           OPEN_EXISTING, 0, nullptr);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  si.hStdInput = nul;
  si.hStdOutput = write_end;
  si.hStdError = write_end;

  PROCESS_INFORMATION pi{};
  std::wstring cmdline = L"\"" + exe + L"\"";
  std::vector<wchar_t> cmd(cmdline.begin(), cmdline.end());
  cmd.push_back(L'\0');

  // CREATE_NO_WINDOW：不闪控制台窗口；CREATE_NEW_PROCESS_GROUP 便于以后加优雅退出。
  BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                           CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr,
                           dir.c_str(), &si, &pi);
  CloseHandle(write_end);
  if (nul != INVALID_HANDLE_VALUE && nul != nullptr) CloseHandle(nul);

  if (!ok) {
    CloseHandle(read_end);
    *error = L"启动 xiaotu-server.exe 失败（错误码 " + std::to_wstring(GetLastError()) + L"）。";
    return false;
  }

  CloseHandle(pi.hThread);
  g_app.child = pi.hProcess;
  g_app.stdout_read = read_end;

  // 放进 job：壳异常退出时子进程也会被系统回收。
  g_app.job = CreateJobObjectW(nullptr, nullptr);
  if (g_app.job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(g_app.job, JobObjectExtendedLimitInformation, &info, sizeof(info));
    AssignProcessToJobObject(g_app.job, pi.hProcess);
  }
  return true;
}

// 从累积输出里找一次性访问地址：http://.../#t=<hex>
bool extract_url(const std::string& data, std::wstring* url) {
  const std::string marker = "/#t=";
  size_t marker_pos = data.find(marker);
  if (marker_pos == std::string::npos) return false;
  size_t start = data.rfind("http://", marker_pos);
  if (start == std::string::npos) return false;
  size_t end = marker_pos + marker.size();
  while (end < data.size() && isxdigit(static_cast<unsigned char>(data[end]))) ++end;
  if (end == marker_pos + marker.size()) return false;  // token 为空，继续等
  std::string ascii = data.substr(start, end - start);
  int n = MultiByteToWideChar(CP_ACP, 0, ascii.data(), static_cast<int>(ascii.size()), nullptr, 0);
  std::wstring wide(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_ACP, 0, ascii.data(), static_cast<int>(ascii.size()), wide.data(), n);
  *url = wide;
  return true;
}

void start_reader() {
  g_app.reader = std::thread([] {
    std::string acc;
    bool reported = false;
    for (;;) {
      char chunk[4096];
      DWORD got = 0;
      if (!ReadFile(g_app.stdout_read, chunk, sizeof(chunk), &got, nullptr) || got == 0) break;
      acc.append(chunk, got);
      {
        std::lock_guard<std::mutex> lock(g_app.output_mutex);
        g_app.output_bytes.append(chunk, got);
        if (g_app.output_bytes.size() > 8192) {
          g_app.output_bytes.erase(0, g_app.output_bytes.size() - 8192);
        }
      }
      if (!reported) {
        std::wstring url;
        if (extract_url(acc, &url)) {
          reported = true;
          auto* heap = new std::wstring(url);
          if (!PostMessageW(g_app.hwnd, kMsgServerUrl, 0, reinterpret_cast<LPARAM>(heap))) {
            delete heap;
          }
        }
      }
    }
    if (!g_app.stopping.load()) {
      PostMessageW(g_app.hwnd, kMsgServerExited, 0, 0);
    }
  });
}

// ---------- WebView2 ----------

void resize_webview() {
  if (!g_app.controller_ready || !g_app.controller) return;
  RECT rc{};
  GetClientRect(g_app.hwnd, &rc);
  g_app.controller->put_Bounds(rc);
}

void navigate_when_ready(const std::wstring& url) {
  g_app.pending_url = url;
  if (!g_app.controller_ready || !g_app.webview || g_app.navigated) return;
  g_app.navigated = true;
  g_app.webview->Navigate(url.c_str());
}

class ControllerCompleted
    : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
 public:
  explicit ControllerCompleted(AppState* app) : app_(app) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown ||
        riid == __uuidof(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)) {
      *ppv = static_cast<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&ref_));
  }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG count = static_cast<ULONG>(InterlockedDecrement(&ref_));
    if (count == 0) delete this;
    return count;
  }

  HRESULT STDMETHODCALLTYPE Invoke(HRESULT result,
                                   ICoreWebView2Controller* controller) override {
    if (FAILED(result) || controller == nullptr) {
      show_error(L"创建 WebView2 视图失败（HRESULT 0x" + to_hex(result) +
                 L"）。\n\n请确认已安装 WebView2 Runtime。");
      return S_OK;
    }
    controller->AddRef();
    app_->controller = controller;
    if (FAILED(controller->get_CoreWebView2(&app_->webview)) || app_->webview == nullptr) {
      show_error(L"获取 WebView2 核心对象失败。");
      return S_OK;
    }
    app_->controller_ready = true;
    resize_webview();
    if (!app_->pending_url.empty()) navigate_when_ready(app_->pending_url);
    return S_OK;
  }

 private:
  static std::wstring to_hex(HRESULT hr) {
    wchar_t buf[16];
    swprintf_s(buf, L"%08X", static_cast<unsigned>(hr));
    return buf;
  }

  LONG ref_ = 1;
  AppState* app_;
};

class EnvironmentCompleted
    : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
 public:
  explicit EnvironmentCompleted(AppState* app) : app_(app) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown ||
        riid == __uuidof(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)) {
      *ppv = static_cast<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&ref_));
  }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG count = static_cast<ULONG>(InterlockedDecrement(&ref_));
    if (count == 0) delete this;
    return count;
  }

  HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Environment* env) override {
    if (FAILED(result) || env == nullptr) {
      show_error(L"初始化 WebView2 失败（HRESULT 0x" + to_hex(result) +
                 L"）。\n\n本机需要 WebView2 Runtime：\n"
                 L"https://go.microsoft.com/fwlink/p/?LinkId=2124703\n"
                 L"安装后重新启动本程序。");
      return S_OK;
    }
    env->AddRef();
    app_->env = env;
    HRESULT hr = env->CreateCoreWebView2Controller(
        app_->hwnd, new ControllerCompleted(app_));
    if (FAILED(hr)) {
      show_error(L"创建 WebView2 控制器失败（HRESULT 0x" + to_hex(hr) + L"）。");
    }
    return S_OK;
  }

 private:
  static std::wstring to_hex(HRESULT hr) {
    wchar_t buf[16];
    swprintf_s(buf, L"%08X", static_cast<unsigned>(hr));
    return buf;
  }

  LONG ref_ = 1;
  AppState* app_;
};

// WebView2 的用户数据目录沿用服务端应用数据目录下的子目录。
std::wstring webview_user_data_dir() {
  std::wstring dir = app_data_dir() + L"\\webview2";
  ensure_directory(dir);  // 已存在时返回 ERROR_ALREADY_EXISTS，忽略
  return dir;
}

bool init_webview() {
  std::wstring user_data = webview_user_data_dir();
  HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
      nullptr, user_data.c_str(), nullptr, new EnvironmentCompleted(&g_app));
  if (FAILED(hr)) {
    show_error(L"无法创建 WebView2 环境（HRESULT 0x" +
               std::to_wstring(static_cast<unsigned>(hr)) +
               L"）。\n\n本机需要 WebView2 Runtime：\n"
               L"https://go.microsoft.com/fwlink/p/?LinkId=2124703");
    return false;
  }
  return true;
}

// ---------- 窗口 ----------

void shutdown_child() {
  g_app.stopping.store(true);
  if (g_app.job) {
    // KILL_ON_JOB_CLOSE：关闭 job 句柄会带走子进程及其可能的子进程。
    CloseHandle(g_app.job);
    g_app.job = nullptr;
  }
  if (g_app.child) {
    DWORD wait = WaitForSingleObject(g_app.child, 0);
    if (wait == WAIT_TIMEOUT) {
      TerminateProcess(g_app.child, 0);
      WaitForSingleObject(g_app.child, 3000);
    }
    CloseHandle(g_app.child);
    g_app.child = nullptr;
  }
  if (g_app.stdout_read) {
    CloseHandle(g_app.stdout_read);  // 让读线程的 ReadFile 返回
    g_app.stdout_read = nullptr;
  }
  if (g_app.reader.joinable()) g_app.reader.join();
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_SIZE:
      resize_webview();
      return 0;

    case kMsgServerUrl: {
      std::wstring* url = reinterpret_cast<std::wstring*>(lparam);
      if (url) {
        KillTimer(hwnd, kTimerStartup);
        navigate_when_ready(*url);
        delete url;
      }
      return 0;
    }

    case kMsgServerExited:
      if (!g_app.navigated) {
        KillTimer(hwnd, kTimerStartup);
        DWORD code = 0;
        if (g_app.child) GetExitCodeProcess(g_app.child, &code);
        std::wstring text = L"服务端未能启动（退出码 " + std::to_wstring(code) +
                            L"），没有给出访问地址。\n\n"
                            L"常见原因：数据目录不可写、数据库被其他实例占用、"
                            L"配置文件有误。\n\n最后输出：\n" + output_tail();
        show_error(text);
        DestroyWindow(hwnd);
      }
      return 0;

    case WM_TIMER:
      if (wparam == kTimerStartup && !g_app.navigated) {
        KillTimer(hwnd, kTimerStartup);
        show_error(L"等待服务端启动超时（" + std::to_wstring(g_startup_timeout_ms / 1000) +
                   L" 秒）。\n\n可用 --startup-timeout <秒> 或环境变量 "
                   L"XIAOTU_SHELL_STARTUP_TIMEOUT 调整等待上限。\n\n最后输出：\n" +
                   output_tail());
        DestroyWindow(hwnd);
      }
      return 0;

    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;

    case WM_DESTROY:
      save_window_state(hwnd);
      shutdown_child();
      if (g_app.webview) {
        g_app.webview->Release();
        g_app.webview = nullptr;
      }
      if (g_app.controller) {
        g_app.controller->Close();
        g_app.controller->Release();
        g_app.controller = nullptr;
      }
      if (g_app.env) {
        g_app.env->Release();
        g_app.env = nullptr;
      }
      PostQuitMessage(0);
      return 0;

    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
  SetProcessDPIAware();
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  resolve_startup_timeout();

  // 单实例：Local\ 命名互斥体按登录会话隔离（不同用户/RDP 会话互不影响），
  // 已存在时把旧窗口提到前台再退出，避免两个壳各起一个服务端抢同一数据目录。
  g_instance_mutex = CreateMutexW(nullptr, TRUE, kInstanceMutexName);
  if (g_instance_mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
    activate_existing_instance();
    CloseHandle(g_instance_mutex);
    g_instance_mutex = nullptr;
    CoUninitialize();
    return 0;
  }

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = window_proc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  wc.lpszClassName = kWindowClass;
  if (!RegisterClassExW(&wc)) {
    MessageBoxW(nullptr, L"注册窗口类失败。", kWindowTitle, MB_OK | MB_ICONERROR);
    CoUninitialize();
    return 1;
  }

  // 恢复上次的窗口位置与大小；越界（换显示器/分辨率变小）时用默认值。
  const WindowState placement = load_window_state();
  int x = CW_USEDEFAULT;
  int y = CW_USEDEFAULT;
  int width = kDefaultWidth;
  int height = kDefaultHeight;
  if (placement.valid) {
    x = placement.rc.left;
    y = placement.rc.top;
    width = placement.rc.right - placement.rc.left;
    height = placement.rc.bottom - placement.rc.top;
  }

  g_app.hwnd = CreateWindowExW(0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW, x, y, width,
                               height, nullptr, nullptr, instance, nullptr);
  if (!g_app.hwnd) {
    MessageBoxW(nullptr, L"创建窗口失败。", kWindowTitle, MB_OK | MB_ICONERROR);
    CoUninitialize();
    return 1;
  }

  if (placement.valid && placement.maximized) {
    ShowWindow(g_app.hwnd, SW_SHOWMAXIMIZED);
  } else {
    ShowWindow(g_app.hwnd, show == 0 ? SW_SHOWNORMAL : show);
  }
  UpdateWindow(g_app.hwnd);

  std::wstring error;
  if (!launch_server(&error)) {
    show_error(error);
    DestroyWindow(g_app.hwnd);
    CoUninitialize();
    return 1;
  }

  // 先建 WebView2 再等地址：环境初始化与子进程启动并行，缩短首屏时间。
  if (!init_webview()) {
    DestroyWindow(g_app.hwnd);
    CoUninitialize();
    return 1;
  }

  start_reader();
  SetTimer(g_app.hwnd, kTimerStartup, g_startup_timeout_ms, nullptr);

  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  if (g_instance_mutex) {
    CloseHandle(g_instance_mutex);
    g_instance_mutex = nullptr;
  }
  CoUninitialize();
  return static_cast<int>(msg.wParam);
}
