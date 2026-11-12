#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#include "platform_paths.h"
#include "xiaotu/db.h"
#include "xiaotu/error.h"
#include "xiaotu/http_server.h"
#include "xiaotu/ids.h"
#include "xiaotu/migrations.h"
#include "xiaotu/services.h"

#if defined(__APPLE__)
#include "webview.h"
#endif

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop = true; }

struct Options {
  bool server = false;
  int port = 8080;
  std::filesystem::path data_dir;
  bool show_help = false;
};

void print_usage(const char* program) {
  std::cout << "小兔进销存 " << XIAOTU_VERSION << "\n\n"
            << "用法: " << program << " [选项]\n\n"
            << "不带参数启动 = 单机版（仅监听 127.0.0.1 随机端口，用系统 WebView 打开）\n"
            << "  --server        以网络版启动，监听 0.0.0.0\n"
            << "  --port <端口>   网络版监听端口，默认 8080\n"
            << "  --data <目录>   数据目录，默认为系统应用数据目录\n"
            << "  --version       输出版本号\n"
            << "  --help          输出本帮助\n";
}

Options parse_args(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--server") {
      options.server = true;
    } else if (arg == "--port" && i + 1 < argc) {
      options.port = std::stoi(argv[++i]);
    } else if (arg == "--data" && i + 1 < argc) {
      options.data_dir = argv[++i];
    } else if (arg == "--version") {
      std::cout << XIAOTU_VERSION << std::endl;
      std::exit(0);
    } else if (arg == "--help" || arg == "-h") {
      options.show_help = true;
    } else {
      throw xiaotu::AppError("BAD_ARGUMENT", "无法识别的参数: " + arg);
    }
  }
  return options;
}

std::string local_url(const std::string& base, const std::string& token) {
  return base + "/#t=" + token;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Options options = parse_args(argc, argv);
    if (options.show_help) {
      print_usage(argv[0]);
      return 0;
    }

    const std::filesystem::path data_dir =
        options.data_dir.empty() ? xiaotu::platform::app_data_dir() : options.data_dir;
    std::filesystem::create_directories(data_dir);
    const std::filesystem::path database_path = data_dir / "app.db";

    std::cout << "小兔进销存 " << XIAOTU_VERSION << " (" << xiaotu::platform::name()
              << (options.server ? " / 网络版" : " / 单机版") << ")\n"
              << "数据目录: " << data_dir.string() << "\n"
              << "数据库:   " << database_path.string() << std::endl;

    xiaotu::Db db(database_path, 4);
    xiaotu::migrate(db);
    std::cout << "数据库结构版本: v" << db.schema_version() << std::endl;

    xiaotu::Services services(db);
    const std::string token = xiaotu::random_hex(32);
    xiaotu::ServerInfo info{
        .mode = options.server ? "server" : "local",
        .app_name = "小兔进销存",
        .version = XIAOTU_VERSION,
    };
    xiaotu::HttpServer http(services, info, token);

    if (!options.server) {
      const int port = http.bind("127.0.0.1", 0);
      http.serve_async();
      const std::string base = "http://127.0.0.1:" + std::to_string(port);
      std::string url = local_url(base, token);
#if defined(XIAOTU_DEV_WEB)
      const char* dev = std::getenv("XIAOTU_DEV_URL");
      const std::string dev_url = dev != nullptr && *dev != '\0' ? dev : "http://127.0.0.1:5173";
      url = dev_url + "/?api=" + base + "#t=" + token;
      std::cout << "开发态前端: " << dev_url << "（修改 web/ 后刷新即可，无需重新编译）" << std::endl;
#endif
      std::cout << "服务地址: " << base << "\n访问地址: " << url << std::endl;
#if defined(__APPLE__)
      const int code = xiaotu::platform::run_webview(url, "小兔进销存");
      http.stop();
      return code;
#else
      std::cout << "当前平台未内置 WebView 壳，请用浏览器打开上面的访问地址。" << std::endl;
      while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
      http.stop();
      return 0;
#endif
    }

    const int port = http.bind("0.0.0.0", options.port);
    http.serve_async();
    std::cout << "服务地址: http://127.0.0.1:" << port << "/\n"
              << "访问凭据（10 分钟内有效，仅可使用一次）:\n  "
              << local_url("http://127.0.0.1:" + std::to_string(port), token) << "\n"
              << "局域网访问时，把 127.0.0.1 换成本机 IP。" << std::endl;

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    while (!g_stop) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    http.stop();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "启动失败: " << error.what() << std::endl;
    return 1;
  }
}
