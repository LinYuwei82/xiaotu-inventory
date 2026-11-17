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
// IFileDialog / IFileSaveDialog / IFileOpenDialog —— 文件对话框（不在 WIN32_LEAN_AND_MEAN 默认包含里）
#include <shobjidl.h>
// DISP_E_TOOFEWPARAMS 不在 WIN32_LEAN_AND_MEAN 包含的头里，直接用 E_INVALIDARG 表达参数不足。
// OAIdl.h / dispids.h 都会引入大量 oidl 常量，没必要为一个错误码 include 进来。

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <functional>

#include "WebView2.h"
#include "updater_win.h"
#include "nlohmann/json.hpp"

// OpenSSL base64 编解码 —— shell 已链接 OpenSSL::SSL OpenSSL::Crypto
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>

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

// ---------- WebView2 ↔ 前端消息桥 ----------
//
// 设计：
//   C++ 端（updater_win）在后台线程跑清单检查/下载/安装，完成后用
//   g_webview->PostWebMessageAsJson("{event: "checkNow", code: "...", status: {...}}")
//   推给前端。前端统一监听 window.chrome.webview.addEventListener('message', ...)
//
//   JS 桥本身（UpdaterHost）只暴露同步/触发型方法——返回当前状态或立即触发后台动作，
//   真正的异步结果走 PostWebMessage 推回去。避免在 IDispatch::Invoke 里阻塞 UI 线程。

// 当前 webview 指针（ControllerCompleted 里设置，WM_DESTROY 里清 null），
// 供 updater_win 的异步回调把结果推回前端。
ICoreWebView2* g_webview_for_post = nullptr;
std::mutex g_webview_mutex;

// UTF-8 → UTF-16。不能用 std::wstring(json.begin(), json.end())：
// 那样是把每个字节当成一个 wchar_t，中文会变成乱码（PostWebMessageAsJson 按 UTF-16 解析）。
static std::wstring utf8_to_wide(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  if (n <= 0) return L"";
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

static void post_web_message_json(const std::string& json) {
  std::lock_guard<std::mutex> lock(g_webview_mutex);
  if (!g_webview_for_post) return;
  std::wstring wide = utf8_to_wide(json);
  g_webview_for_post->PostWebMessageAsJson(wide.c_str());
}

// 把 updater_win 的 Completion 回调接到主线程 → PostWebMessage 推给前端。
static xiaotu::updater::Completion BindCompletion(const std::string& event) {
  return [event](const std::string& code, const std::string& detail) {
    nlohmann::json j;
    j["event"] = event;
    j["code"] = code;
    j["detail"] = detail;
    j["status"] = xiaotu::updater::UpdaterWin::instance().statusJson();
    post_web_message_json(j.dump());
  };
}

// UpdaterHost —— 最小 IDispatch 实现，暴露给前端做 WebView2 HostObject。
//
// 方法 DISPID 映射：
//   1 checkNow()            立即触发清单检查（立即返回空；结果走 PostWebMessage）
//   2 downloadUpdate()     触发后台下载 setup.exe
//   3 installUpdate()      触发 ISCC 静默覆盖
//   4 cancelPrefetch()     取消下载
//   5 setOptions(auto_check, prefetch) 设置后台检查开关
//   6 statusJson()         返回当前状态 JSON（同步）
//   7 checkMinSupportedVersion() 检查服务端最低要求
class UpdaterHost : public IDispatch {
 public:
  UpdaterHost() : ref_(1) {}
  ~UpdaterHost() = default;

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDispatch) {
      *ppv = static_cast<IDispatch*>(this);
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
    ULONG c = static_cast<ULONG>(InterlockedDecrement(&ref_));
    if (c == 0) delete this;
    return c;
  }
  HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* ctinfo) override {
    if (!ctinfo) return E_POINTER;
    *ctinfo = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR* names, UINT count, LCID, DISPID* ids) override {
    static const wchar_t* kNames[] = {
        L"checkNow", L"downloadUpdate", L"installUpdate",
        L"cancelPrefetch", L"setOptions", L"statusJson",
        L"checkMinSupportedVersion",
    };
    for (UINT i = 0; i < count; ++i) {
      ids[i] = DISPID_UNKNOWN;
      for (UINT j = 0; j < 7; ++j) {
        if (wcscmp(names[i], kNames[j]) == 0) { ids[i] = static_cast<DISPID>(j + 1); break; }
      }
      if (ids[i] == DISPID_UNKNOWN) return DISP_E_UNKNOWNNAME;
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Invoke(DISPID dispid, REFIID, LCID, WORD flags,
                                   DISPPARAMS* params, VARIANT* result,
                                   EXCEPINFO*, UINT*) override {
    (void)flags;
    auto& upd = xiaotu::updater::UpdaterWin::instance();
    auto set_str = [&](const std::string& s) -> HRESULT {
      if (!result) return S_OK;
      result->vt = VT_BSTR;
      result->bstrVal = SysAllocStringByteLen(s.c_str(), static_cast<UINT>(s.size()));
      return result->bstrVal ? S_OK : E_OUTOFMEMORY;
    };
    switch (dispid) {
      case 1: upd.checkNow(BindCompletion("checkNow")); return set_str("");
      case 2: upd.downloadUpdate(BindCompletion("downloadUpdate")); return set_str("");
      case 3: upd.installUpdate(BindCompletion("installUpdate")); return set_str("");
      case 4: upd.cancelPrefetch(BindCompletion("cancelPrefetch")); return set_str("");
      case 5: {
        if (!params || params->cArgs < 2) return E_INVALIDARG;
        bool ac = (params->rgvarg[params->cArgs - 2].vt == VT_BOOL) &&
                  (params->rgvarg[params->cArgs - 2].boolVal != VARIANT_FALSE);
        bool pf = (params->rgvarg[params->cArgs - 1].vt == VT_BOOL) &&
                  (params->rgvarg[params->cArgs - 1].boolVal != VARIANT_FALSE);
        upd.setOptions(ac, pf, BindCompletion("setOptions"));
        return set_str("");
      }
      case 6: return set_str(upd.statusJson());
      case 7: upd.checkMinSupportedVersion(BindCompletion("checkMinSupportedVersion")); return set_str("");
      default: return DISP_E_MEMBERNOTFOUND;
    }
  }

 private:
  LONG ref_ = 1;
};

// ---------- Native IO Host（文件对话框 + base64） ----------

HWND g_shell_hwnd = nullptr;  // 主窗口句柄，供文件对话框用

std::wstring u8_to_w(const std::string& s) {
  return utf8_to_wide(s);
}

std::string w_to_u8(const std::wstring& s) {
  if (s.empty()) return "";
  int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
  if (n <= 0) return "";
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
  return out;
}

// OpenSSL base64 解码（无换行）
std::vector<uint8_t> base64_decode(const std::string& b64) {
  if (b64.empty()) return {};
  BIO* b64_bio = BIO_new(BIO_f_base64());
  BIO* mem = BIO_new_mem_buf(b64.data(), static_cast<int>(b64.size()));
  if (!b64_bio || !mem) { BIO_free(b64_bio); BIO_free(mem); return {}; }
  BIO_set_flags(b64_bio, BIO_FLAGS_BASE64_NO_NL);
  BIO* chain = BIO_push(b64_bio, mem);
  size_t est = (b64.size() / 4) * 3 + 8;
  std::vector<uint8_t> out(est);
  int decoded = BIO_read(chain, out.data(), static_cast<int>(out.size()));
  if (decoded <= 0) { BIO_free_all(chain); return {}; }
  out.resize(static_cast<size_t>(decoded));
  BIO_free_all(chain);
  return out;
}

// OpenSSL base64 编码（无换行）
std::string base64_encode(const uint8_t* data, size_t len) {
  if (!data || len == 0) return "";
  BIO* b64_bio = BIO_new(BIO_f_base64());
  BIO* mem = BIO_new(BIO_s_mem());
  if (!b64_bio || !mem) { BIO_free(b64_bio); BIO_free(mem); return ""; }
  BIO_set_flags(b64_bio, BIO_FLAGS_BASE64_NO_NL);
  BIO* chain = BIO_push(b64_bio, mem);
  BIO_write(chain, data, static_cast<int>(len));
  BIO_flush(chain);
  BUF_MEM* bptr = nullptr;
  BIO_get_mem_ptr(chain, &bptr);
  std::string out(bptr->data, bptr->length);
  BIO_free_all(chain);
  return out;
}

struct DialogResult { bool ok = false; std::wstring path; };

// 弹 IFileSaveDialog。成功返回 ok=true + path；取消或失败 ok=false。
DialogResult show_save_dialog(const std::wstring& default_name) {
  DialogResult r;
  auto co_init = []() -> std::function<void()> {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (hr == S_OK) return [] { CoUninitialize(); };
    return nullptr;  // S_FALSE 或 RPC_E_CHANGED_MODE：已初始化，不用 uninit
  };
  auto co_guard = co_init();

  IFileSaveDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
    if (co_guard) co_guard();
    return r;
  }
  if (!default_name.empty()) {
    dlg->SetFileName(default_name.c_str());
    // 用户如果把扩展名删掉，这里补默认扩展名，避免存出没有后缀的文件
    auto dot = default_name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot + 1 < default_name.size()) {
      dlg->SetDefaultExtension(default_name.substr(dot + 1).c_str());
    }
  }
  dlg->SetOptions(FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM);

  HRESULT hr = dlg->Show(g_shell_hwnd);
  if (SUCCEEDED(hr)) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item))) {
      PWSTR path = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        r.ok = true; r.path = path; CoTaskMemFree(path);
      }
      item->Release();
    }
  }
  dlg->Release();
  if (co_guard) co_guard();
  return r;
}

// 弹 IFileOpenDialog。filter_name 只用于显示，filter_spec 才是真正的过滤规则（如 "*.xlsx"）。
// 只传 filter_name 时从 "Excel (*.xlsx)" 这类写法里提取括号内容作为过滤规则。
DialogResult show_open_dialog(const std::wstring& filter_name, const std::wstring& filter_spec) {
  DialogResult r;
  std::wstring name = filter_name.empty() ? L"所有文件" : filter_name;
  std::wstring spec = filter_spec;
  if (spec.empty()) {
    auto lp = name.find(L'(');
    auto rp = lp == std::wstring::npos ? std::wstring::npos : name.find(L')', lp);
    if (lp != std::wstring::npos && rp != std::wstring::npos && rp > lp + 1) {
      spec = name.substr(lp + 1, rp - lp - 1);
      name = name.substr(0, lp);
      while (!name.empty() && (name.back() == L' ' || name.back() == L'\t')) name.pop_back();
    }
  }
  if (spec.empty()) spec = L"*.*";

  auto co_init = []() -> std::function<void()> {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (hr == S_OK) return [] { CoUninitialize(); };
    return nullptr;
  };
  auto co_guard = co_init();

  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
    if (co_guard) co_guard();
    return r;
  }
  {
    // pszName / pszSpec 指向的字符串必须在 Show() 之前保持有效，所以用局部变量持有
    COMDLG_FILTERSPEC fs;
    fs.pszName = name.c_str();
    fs.pszSpec = spec.c_str();
    dlg->SetFileTypes(1, &fs);
    dlg->SetFileTypeIndex(1);
    dlg->SetOptions(FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);

    HRESULT hr = dlg->Show(g_shell_hwnd);
    if (SUCCEEDED(hr)) {
      IShellItem* item = nullptr;
      if (SUCCEEDED(dlg->GetResult(&item))) {
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
          r.ok = true; r.path = path; CoTaskMemFree(path);
        }
        item->Release();
      }
    }
  }
  dlg->Release();
  if (co_guard) co_guard();
  return r;
}

// 读整个文件（Excel 导入 ≤ 100MB）
std::vector<uint8_t> read_file_bytes(const std::wstring& path) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return {};
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(h, &size) || size.QuadPart > 100LL * 1024 * 1024) { CloseHandle(h); return {}; }
  std::vector<uint8_t> buf(static_cast<size_t>(size.QuadPart));
  DWORD read = 0;
  if (size.QuadPart > 0 && !ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr)) {
    CloseHandle(h); return {};
  }
  CloseHandle(h);
  return buf;
}

bool write_file_bytes(const std::wstring& path, const uint8_t* data, size_t len) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  bool ok = (len == 0) || WriteFile(h, data, static_cast<DWORD>(len), &written, nullptr);
  CloseHandle(h);
  return ok;
}

// NativeIoHost —— saveFile + openFile，暴露给前端做 WebView2 HostObject。
// DISPID 映射：1 saveFile(filenameUtf8, contentBase64Utf8) → bool
//              2 openFile(filterNameUtf8) → JSON {"ok":true,"name":"...","base64":"...","size":n} 或 {"ok":false}
class NativeIoHost : public IDispatch {
 public:
  NativeIoHost() : ref_(1) {}
  ~NativeIoHost() = default;

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDispatch) {
      *ppv = static_cast<IDispatch*>(this); AddRef(); return S_OK;
    }
    *ppv = nullptr; return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&ref_)); }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG c = static_cast<ULONG>(InterlockedDecrement(&ref_));
    if (c == 0) delete this;
    return c;
  }
  HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* ctinfo) override { if (!ctinfo) return E_POINTER; *ctinfo = 0; return S_OK; }
  HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR* names, UINT count, LCID, DISPID* ids) override {
    static const wchar_t* kNames[] = { L"saveFile", L"openFile" };
    for (UINT i = 0; i < count; ++i) {
      ids[i] = DISPID_UNKNOWN;
      for (UINT j = 0; j < 2; ++j)
        if (wcscmp(names[i], kNames[j]) == 0) { ids[i] = static_cast<DISPID>(j + 1); break; }
      if (ids[i] == DISPID_UNKNOWN) return DISP_E_UNKNOWNNAME;
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Invoke(DISPID dispid, REFIID, LCID, WORD, DISPPARAMS* params,
                                   VARIANT* result, EXCEPINFO*, UINT*) override {
    auto set_str = [&](const std::string& s) -> HRESULT {
      if (!result) return S_OK;
      result->vt = VT_BSTR;
      result->bstrVal = SysAllocStringByteLen(s.c_str(), static_cast<UINT>(s.size()));
      return result->bstrVal ? S_OK : E_OUTOFMEMORY;
    };
    auto set_bool = [&](bool b) -> HRESULT {
      if (!result) return S_OK;
      result->vt = VT_BOOL;
      result->boolVal = b ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
    };
    // DISPPARAMS 参数倒序：rgvarg[0] 是最后一个参数
    auto arg = [&](int idx) -> std::string {
      if (!params || idx < 0 || static_cast<int>(idx) >= static_cast<int>(params->cArgs)) return "";
      VARIANT* v = &params->rgvarg[params->cArgs - 1 - idx];
      if (v->vt == VT_BSTR) return w_to_u8(v->bstrVal ? v->bstrVal : L"");
      return "";
    };

    switch (dispid) {
      case 1: {  // saveFile(filename, contentBase64) → bool
        std::wstring name = u8_to_w(arg(0));
        const std::string b64 = arg(1);
        auto bytes = base64_decode(b64);
        // 有内容但解码失败：不能静默写出 0 字节文件（用户会以为导出成功）
        if (!b64.empty() && bytes.empty()) return set_bool(false);
        if (name.empty()) return set_bool(false);
        DialogResult dr = show_save_dialog(name);
        if (!dr.ok) return set_bool(false);
        return set_bool(write_file_bytes(dr.path, bytes.data(), bytes.size()));
      }
      case 2: {  // openFile(filterName, filterSpec) → JSON
        DialogResult dr = show_open_dialog(u8_to_w(arg(0)), u8_to_w(arg(1)));
        if (!dr.ok) return set_str("{\"ok\":false}");
        auto bytes = read_file_bytes(dr.path);
        if (bytes.empty()) return set_str("{\"ok\":false,\"error\":\"read_failed\"}");
        std::wstring fname = dr.path;
        auto slash = fname.find_last_of(L"\\/");
        if (slash != std::wstring::npos) fname = fname.substr(slash + 1);
        nlohmann::json j = {
          {"ok", true},
          {"name", w_to_u8(fname)},
          {"base64", base64_encode(bytes.data(), bytes.size())},
          {"size", bytes.size()}
        };
        return set_str(j.dump());
      }
      default: return DISP_E_MEMBERNOTFOUND;
    }
  }

 private:
  LONG ref_ = 1;
};

// PrintToPdf 回调 —— 通知前端导出完成/失败
class PrintToPdfCompleted final
    : public ICoreWebView2PrintToPdfCompletedHandler {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == __uuidof(ICoreWebView2PrintToPdfCompletedHandler)) {
      *ppv = static_cast<ICoreWebView2PrintToPdfCompletedHandler*>(this); AddRef(); return S_OK;
    }
    *ppv = nullptr; return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&ref_)); }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG c = static_cast<ULONG>(InterlockedDecrement(&ref_));
    if (c == 0) delete this;
    return c;
  }
  HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode, BOOL result) override {
    const char* code = SUCCEEDED(errorCode) && result ? "ok" : "error";
    nlohmann::json j = {{"event", "printPdf"}, {"code", code}};
    post_web_message_json(j.dump());
    return S_OK;
  }

 private:
  LONG ref_ = 1;
};

// WebMessageReceived handler —— 前端用 chrome.webview.postMessage({kind:"printPdf",...}) 发出的消息。
class WebMessageReceivedHandler final
    : public ICoreWebView2WebMessageReceivedEventHandler {
 public:
  explicit WebMessageReceivedHandler(ICoreWebView2* webview) : webview_(webview) { webview_->AddRef(); }
  ~WebMessageReceivedHandler() { if (webview_) webview_->Release(); }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == __uuidof(ICoreWebView2WebMessageReceivedEventHandler)) {
      *ppv = static_cast<ICoreWebView2WebMessageReceivedEventHandler*>(this); AddRef(); return S_OK;
    }
    *ppv = nullptr; return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&ref_)); }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG c = static_cast<ULONG>(InterlockedDecrement(&ref_));
    if (c == 0) delete this;
    return c;
  }
  HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) override {
    (void)sender;
    // 前端既可能 postMessage(JSON 字符串)，也可能 postMessage(对象)。
    // TryGetWebMessageAsString 只对「字符串消息」成功；对象消息必须用 get_WebMessageAsJson，
    // 否则对象消息会被当成失败直接丢弃（PDF 导出点了没反应就是这么来的）。
    std::string msg;
    PWSTR raw = nullptr;
    if (SUCCEEDED(args->TryGetWebMessageAsString(&raw)) && raw) {
      msg = w_to_u8(raw);
      CoTaskMemFree(raw);
    } else {
      raw = nullptr;
      if (FAILED(args->get_WebMessageAsJson(&raw)) || !raw) return S_OK;
      msg = w_to_u8(raw);
      CoTaskMemFree(raw);
    }

    try {
      auto j = nlohmann::json::parse(msg);
      std::string kind = j.value("kind", "");
      if (kind == "printPdf") {
        // 前端传来 { filename, paper }，用 WebView2.PrintToPdf（ICoreWebView2_7）把当前页面渲染成 PDF
        std::string fname_utf8 = j.value("filename", "导出.pdf");
        std::wstring fname = u8_to_w(fname_utf8);
        DialogResult dr = show_save_dialog(fname);
        if (!dr.ok) return S_OK;
        // QI 到 ICoreWebView2_7 才能调 PrintToPdf
        ICoreWebView2_7* wv7 = nullptr;
        if (SUCCEEDED(webview_->QueryInterface(IID_PPV_ARGS(&wv7)))) {
          wv7->PrintToPdf(dr.path.c_str(), nullptr, new PrintToPdfCompleted());
          wv7->Release();
        } else {
          nlohmann::json err = {{"event", "printPdf"}, {"code", "error"}, {"message", "no_wv7"}};
          post_web_message_json(err.dump());
        }
      }
    } catch (...) { /* JSON 解析失败忽略 */ }
    return S_OK;
  }

 private:
  LONG ref_ = 1;
  ICoreWebView2* webview_;
};

// ---------- 文本与路径 ----------
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

// HRESULT 统一按 8 位十六进制输出：0x80070002 不能按十进制拼成 0x2147942402。
std::wstring hresult_hex(HRESULT hr) {
  wchar_t buf[16];
  swprintf_s(buf, L"%08X", static_cast<unsigned>(hr));
  return buf;
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
      show_error(L"创建 WebView2 视图失败（HRESULT 0x" + hresult_hex(result) +
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
    // 给前端暴露 updater_win HostObject：
    //   window.chrome.webview.hostObjects.updater_win.checkNow() / .statusJson() 等
    // WebView2 的 AddHostObjectToScript 接收 IDispatch*（VARIANT 的 vt=VT_DISPATCH），
    // 失败时我们 Release 让它 refcount 归零自动 delete（UpdaterHost 的 Release 做）。
    {
      VARIANT v{};
      v.vt = VT_DISPATCH;
      v.pdispVal = new UpdaterHost();
      std::wstring name = L"updater_win";
      HRESULT hr = app_->webview->AddHostObjectToScript(name.c_str(), &v);
      if (FAILED(hr)) {
        // AddHostObjectToScript 失败时不接管引用，手动 Release 回收。
        v.pdispVal->Release();
      }
    }
    // 给前端暴露 native_io HostObject —— 文件打开/保存对话框：
    //   window.chrome.webview.hostObjects.native_io.saveFile(filename, base64)
    //   window.chrome.webview.hostObjects.native_io.openFile(filterName)
    {
      VARIANT v{};
      v.vt = VT_DISPATCH;
      v.pdispVal = new NativeIoHost();
      std::wstring name = L"native_io";
      HRESULT hr = app_->webview->AddHostObjectToScript(name.c_str(), &v);
      if (FAILED(hr)) v.pdispVal->Release();
    }
    // 注册 WebMessageReceived —— 前端用 chrome.webview.postMessage({...}) 发的消息。
    //   { kind: "printPdf", filename, paper } → 原生用 PrintToPdf 把当前页面渲染成 PDF
    app_->webview->add_WebMessageReceived(new WebMessageReceivedHandler(app_->webview), nullptr);
    // 给文件对话框用的主窗口句柄
    g_shell_hwnd = app_->hwnd;
    // 给 updater_win 异步回调用的 webview 指针
    {
      std::lock_guard<std::mutex> lock(g_webview_mutex);
      g_webview_for_post = app_->webview;
    }
    resize_webview();
    if (!app_->pending_url.empty()) navigate_when_ready(app_->pending_url);
    return S_OK;
  }

 private:
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
      show_error(L"初始化 WebView2 失败（HRESULT 0x" + hresult_hex(result) +
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
      show_error(L"创建 WebView2 控制器失败（HRESULT 0x" + hresult_hex(hr) + L"）。");
    }
    return S_OK;
  }

 private:
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
    show_error(L"无法创建 WebView2 环境（HRESULT 0x" + hresult_hex(hr) +
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
      // 先清 post 指针：updater_win 后台回调还在跑，清 null 就不会往已释放的 webview 推消息。
      {
        std::lock_guard<std::mutex> lock(g_webview_mutex);
        g_webview_for_post = nullptr;
      }
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
  // 资源 ID 1 是 shell/win/version.rc.in 里 "1 ICON" 嵌进去的那份图标（候选 A）。
  // 不设 hIcon/hIconSm 时，标题栏与任务栏按钮画的是系统默认图标，只有资源管理器、
  // 桌面/开始菜单快捷方式读 exe 资源段，表现为「换了图标但不彻底」。
  wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
  // 小图标不能复用 LoadIconW：它固定按 SM_CXICON（通常 32px）取，高 DPI 下再被
  // 下采样到 16px 会发糊；LoadImageW 按 SM_CXSMICON/SM_CYSMICON 直接取对应尺寸。
  wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                             GetSystemMetrics(SM_CXSMICON),
                                             GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
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

  // 启动自动更新桥：manifest_url 先用 OSS 默认路径，之后可以让服务端 /api/health 下发覆盖。
  // channel 默认 stable，用户切换 beta/rc 时通过 JS 桥 setOptions 改。
  // Windows 清单单独放一个目录：macOS 的 latest.json 里 platform=macos，
  // 两者共用同一路径会互相覆盖/被判成平台不匹配。
  xiaotu::updater::UpdaterWin::instance().start(
      "https://dl.xiaotu.online/windows/stable/latest.json",
      "stable");

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
