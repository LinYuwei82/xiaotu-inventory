#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "xiaotu/audit.h"
#include "xiaotu/backup.h"
#include "xiaotu/config.h"
#include "xiaotu/customer_service.h"
#include "xiaotu/db.h"
#include "xiaotu/error.h"
#include "xiaotu/http_server.h"
#include "xiaotu/ids.h"
#include "xiaotu/instance_lock.h"
#include "xiaotu/log.h"
#include "xiaotu/migrations.h"
#include "xiaotu/partner_preview.h"
#include "xiaotu/permissions.h"
#include "xiaotu/platform_paths.h"
#include "xiaotu/services.h"

#if defined(__APPLE__) && defined(XIAOTU_WEBVIEW_SHELL)
#include "webview.h"
#define XIAOTU_HAS_WEBVIEW 1
#endif

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

// Windows 控制台默认使用系统代码页（简体中文为 936），而日志与提示按 UTF-8 输出，
// 不切代码页会在 cmd 里显示成「灏忓厰…」这类乱码。仅在存在可见控制台时切换，
// 重定向到文件或管道的场景（壳拉起服务端）不受影响，字节仍是 UTF-8。
void configure_console_encoding() {
#if defined(_WIN32)
  if (GetConsoleWindow() == nullptr) {
    return;
  }
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
#endif
}

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop = true; }

struct Options {
  bool server = false;
  std::optional<int> port;
  std::string listen;
  std::filesystem::path data_dir;
  std::filesystem::path config_path;
  bool backup = false;
  bool backup_daily = false;
  std::filesystem::path restore_from;
  std::string set_password_user;
  bool partner_preview = false;
  std::string partner_preview_format = "text";
  bool show_help = false;
};

void print_usage(const char* program) {
  std::cout << "小兔进销存 " << XIAOTU_VERSION << "\n\n"
            << "用法: " << program << " [选项]\n\n"
            << "不带参数启动 = 单机模式（仅监听 127.0.0.1；有 WebView 壳时打开窗口，否则打印浏览器地址）\n"
            << "  --server        以网络版启动，默认监听 0.0.0.0\n"
            << "  --port <端口>   网络版监听端口，默认 8080\n"
            << "  --listen <地址> 网络版监听地址，默认 0.0.0.0\n"
            << "  --data <目录>   数据目录，默认为系统应用数据目录\n"
            << "  --config <文件> 配置文件；默认依次查找 $XIAOTU_CONFIG、<数据目录>/config.toml\n"
            << "  --backup        立即生成一次数据库备份并退出\n"
            << "  --backup-daily  今天尚未成功备份时生成每日备份并清理旧备份（供定时任务调用）\n"
            << "  --restore <文件> 用指定备份覆盖当前数据库并退出（先自动留安全备份）\n"
            << "  --set-password <用户名> 用环境变量 XIAOTU_PASSWORD 设置该用户口令并退出\n"
            << "  --partner-preview 只读预览客户/供应商重复候选并退出\n"
            << "  --partner-preview-format <text|json|csv> 预览输出格式，默认 text\n"
            << "  --version       输出版本号\n"
            << "  --help          输出本帮助\n"
            << "\n配置优先级：命令行 > 环境变量 > config.toml > 内置默认。\n";
}

Options parse_args(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--server") {
      options.server = true;
    } else if (arg == "--port" && i + 1 < argc) {
      options.port = std::stoi(argv[++i]);
    } else if (arg == "--listen" && i + 1 < argc) {
      options.listen = argv[++i];
    } else if (arg == "--config" && i + 1 < argc) {
      options.config_path = argv[++i];
    } else if (arg == "--data" && i + 1 < argc) {
      options.data_dir = argv[++i];
    } else if (arg == "--backup") {
      options.backup = true;
    } else if (arg == "--backup-daily") {
      options.backup_daily = true;
    } else if (arg == "--restore" && i + 1 < argc) {
      options.restore_from = argv[++i];
    } else if (arg == "--set-password" && i + 1 < argc) {
      options.set_password_user = argv[++i];
    } else if (arg == "--partner-preview") {
      options.partner_preview = true;
    } else if (arg == "--partner-preview-format" && i + 1 < argc) {
      options.partner_preview_format = argv[++i];
    } else if (arg == "--version") {
      std::cout << XIAOTU_VERSION << std::endl;
      std::exit(0);
    } else if (arg == "--help" || arg == "-h") {
      options.show_help = true;
    } else {
      throw xiaotu::AppError("BAD_ARGUMENT", "无法识别的参数: " + arg);
    }
  }
  if (options.port.has_value() && (*options.port < 1 || *options.port > 65535)) {
    throw xiaotu::AppError("BAD_ARGUMENT", "--port 应在 1–65535 之间");
  }
  if (options.partner_preview_format != "text" && options.partner_preview_format != "json" &&
      options.partner_preview_format != "csv") {
    throw xiaotu::AppError("BAD_ARGUMENT", "--partner-preview-format 只支持 text/json/csv");
  }
  return options;
}

std::string local_url(const std::string& base, const std::string& token) {
  return base + "/#t=" + token;
}

// 进程内每日备份：启动时检查一次，之后每 30 分钟检查一次。
// 单机版不常驻也没关系，下次启动会补上当天缺失的备份；与外部 cron 并发时由目录锁互斥。
class DailyBackupScheduler {
 public:
  DailyBackupScheduler(xiaotu::Db& db, std::filesystem::path data_dir)
      : db_(db), data_dir_(std::move(data_dir)) {}

  ~DailyBackupScheduler() { stop(); }

  void start() {
    thread_ = std::thread([this] { loop(); });
  }

  void stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      running_.store(false);
    }
    cv_.notify_all();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

 private:
  void loop() {
    while (running_.load()) {
      try {
        const xiaotu::DailyBackupOutcome outcome = xiaotu::run_daily_backup(db_, data_dir_);
        if (outcome.ran) {
          xiaotu::log_info("每日备份完成，清理旧备份 " +
                           std::to_string(outcome.retention.removed) + " 份");
        }
      } catch (const std::exception& error) {
        xiaotu::log_error(std::string("每日备份失败：") + error.what());
      }
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait_for(lock, std::chrono::minutes(30), [this] { return !running_.load(); });
    }
  }

  xiaotu::Db& db_;
  std::filesystem::path data_dir_;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::atomic<bool> running_{true};
};

}  // namespace

int main(int argc, char** argv) {
  configure_console_encoding();
  try {
    const Options options = parse_args(argc, argv);
    if (options.show_help) {
      print_usage(argv[0]);
      return 0;
    }

    // ---- 配置解析：命令行 > 环境变量 > config.toml > 内置默认 ----
    std::filesystem::path data_dir =
        options.data_dir.empty() ? xiaotu::platform::app_data_dir() : options.data_dir;

    std::filesystem::path config_path;
    const char* env_config = std::getenv("XIAOTU_CONFIG");
    bool config_explicit = false;
    if (!options.config_path.empty()) {
      config_path = options.config_path;
      config_explicit = true;
    } else if (env_config != nullptr && *env_config != '\0') {
      config_path = env_config;
      config_explicit = true;
    } else {
      config_path = data_dir / "config.toml";
    }

    xiaotu::ConfigValues file_config;
    bool config_loaded = false;
    std::error_code config_error;
    if (std::filesystem::exists(config_path, config_error)) {
      file_config = xiaotu::load_config_file(config_path);
      config_loaded = true;
    } else if (config_explicit) {
      throw xiaotu::AppError("NOT_FOUND", "指定的配置文件不存在：" + config_path.string());
    }
    // 配置文件里的数据目录只在不带 --data 时生效；它不会反过来改变配置文件的查找位置。
    if (options.data_dir.empty() && file_config.data_dir.has_value()) {
      data_dir = *file_config.data_dir;
    }

    const std::string listen =
        !options.listen.empty() ? options.listen : file_config.listen.value_or("0.0.0.0");
    const int port = options.port.has_value() ? *options.port : file_config.port.value_or(8080);

    std::filesystem::path log_dir;
    if (const char* env = std::getenv("XIAOTU_LOG_DIR"); env != nullptr && *env != '\0') {
      log_dir = env;
    } else if (file_config.log_dir.has_value()) {
      log_dir = *file_config.log_dir;
    } else {
      log_dir = data_dir / "logs";
    }
    std::string level_text;
    if (const char* env = std::getenv("XIAOTU_LOG_LEVEL"); env != nullptr && *env != '\0') {
      level_text = env;
    } else {
      level_text = file_config.log_level.value_or("info");
    }

    std::filesystem::create_directories(data_dir);
    const std::filesystem::path database_path = data_dir / "app.db";

    xiaotu::LogOptions log_options;
    log_options.directory = log_dir;
    log_options.level = xiaotu::parse_log_level(level_text);
    xiaotu::init_logging(log_options);
    xiaotu::log_info(std::string("启动 版本=") + XIAOTU_VERSION +
                     " 模式=" + (options.server ? "server" : "local") +
                     " 数据目录=" + data_dir.string() + " 监听=" +
                     (options.server ? listen + ":" + std::to_string(port) : "127.0.0.1:自动"));

    // 同步目录检测：只告警不拦截，指引用户把数据目录迁回本地磁盘。
    std::string data_dir_warning;
    if (const std::string sync = xiaotu::platform::sync_service_for(data_dir); !sync.empty()) {
      data_dir_warning = "数据目录位于 " + sync +
                         " 同步目录内，多台设备同时打开可能损坏数据库；"
                         "请退出程序后把数据目录迁到本地磁盘（默认 "
                         "~/Library/Application Support/com.simem.xiaotu/）。";
      xiaotu::log_warn(data_dir_warning);
    }

    // 单实例锁：同一数据目录只允许一个实例写库。--backup 允许运行中执行，
    // --set-password 是停机维护命令，两者不参与锁；--restore 必须在停服时执行，
    // 因此照常抢锁，抢不到就提示已有实例在运行。
    const bool maintenance_only = options.backup || options.backup_daily ||
                                  !options.set_password_user.empty() || options.partner_preview;
    // --restore 必须在停服时执行，抢不到锁属于失败；--backup/--backup-daily 可与
    // 运行中的实例并存（VACUUM INTO 是只读快照），--set-password/--partner-preview
    // 是停机维护命令，它们不参与锁，也不存在「抢锁失败」。
    const bool exclusive_action = !options.restore_from.empty();
    xiaotu::InstanceLock instance_lock;
    if (!maintenance_only && !instance_lock.acquire(data_dir)) {
      std::cout << "已有小兔进销存实例在使用该数据目录"
                << (instance_lock.holder_pid().empty()
                        ? ""
                        : "（PID " + instance_lock.holder_pid() + "）")
                << "，本次启动已退出。如果确认没有其他实例，可删除锁文件后重试："
                << instance_lock.lock_path().string() << std::endl;
      xiaotu::log_warn("单实例锁被占用: " + instance_lock.lock_path().string());
      if (exclusive_action) {
        std::cout << "恢复需要独占数据目录，请先停止服务再重试。" << std::endl;
        xiaotu::log_error("恢复被拒绝：数据目录被其他实例占用");
        xiaotu::shutdown_logging();
        return 1;
      }
      xiaotu::shutdown_logging();
      return 0;
    }

    std::cout << "小兔进销存 " << XIAOTU_VERSION << " (" << xiaotu::platform::name()
              << (options.server ? " / 网络版" : " / 单机版") << ")\n"
              << "配置:     " << config_path.string()
              << (config_loaded ? "（已加载）" : "（未找到，使用内置默认）") << "\n"
              << "数据目录: " << data_dir.string() << "\n"
              << "数据库:   " << database_path.string() << "\n"
              << "日志:     " << log_dir.string() << " 级别 "
              << xiaotu::log_level_name(log_options.level) << "\n"
              << "监听:     "
              << (options.server ? listen + ":" + std::to_string(port) : std::string("127.0.0.1:自动端口"))
              << std::endl;
    if (!data_dir_warning.empty()) {
      std::cout << "警告: " << data_dir_warning << std::endl;
    }

    if (!options.restore_from.empty()) {
      if (options.backup || options.backup_daily) {
        throw xiaotu::AppError("BAD_ARGUMENT", "--backup / --backup-daily 与 --restore 不能同时使用");
      }
      const auto snapshot = xiaotu::restore_database(options.restore_from, database_path);
      std::cout << "恢复完成: " << snapshot.path.string() << " (" << snapshot.bytes
                << " 字节)" << std::endl;
      xiaotu::log_info("恢复数据库 " + snapshot.path.string() +
                       " 字节=" + std::to_string(snapshot.bytes));
      // 恢复后往「恢复出来的库」写一条审计；v5 之前的老库没有 audit_log，只留日志。
      {
        xiaotu::Db restored_db(database_path, 1);
        if (restored_db.schema_version() >= 5) {
          restored_db.write([&](sqlite3* conn) {
            xiaotu::audit_write(conn, "restore", "database", 0,
                                options.restore_from.filename().string(),
                                snapshot.path.string());
          });
        }
      }
      xiaotu::shutdown_logging();
      return 0;
    }

    const bool database_existed = std::filesystem::exists(database_path);
    if (options.backup && !database_existed) {
      throw xiaotu::AppError("NOT_FOUND", "数据库不存在，无法备份：" + database_path.string());
    }
    if (options.partner_preview && !database_existed) {
      throw xiaotu::AppError("NOT_FOUND", "数据库不存在，无法预览：" + database_path.string());
    }
    xiaotu::Db db(database_path, 4);
    if (options.backup) {
      const auto snapshot = xiaotu::run_manual_backup(db, data_dir);
      std::cout << "备份完成: " << snapshot.path.string() << " (" << snapshot.bytes
                << " 字节)" << std::endl;
      xiaotu::log_info("手动备份 " + snapshot.path.string() +
                       " 字节=" + std::to_string(snapshot.bytes));
      xiaotu::shutdown_logging();
      return 0;
    }

    // 只读预览：必须在迁移之前执行，避免老库被顺带升级。
    // 只读 customer/supplier/ar_ap_entry，不写业务表、不做合并。
    if (options.partner_preview) {
      if (db.schema_version() < 19) {
        throw xiaotu::AppError("MIGRATION_REQUIRED",
                               "数据库版本低于 v19，缺少税号字段；请先正常启动一次完成迁移");
      }
      const xiaotu::PartnerPreviewReport report = xiaotu::partner_merge_preview(db);
      if (options.partner_preview_format == "json") {
        std::cout << xiaotu::partner_preview_json(report);
      } else if (options.partner_preview_format == "csv") {
        std::cout << xiaotu::partner_preview_csv(report);
      } else {
        std::cout << xiaotu::partner_preview_text(report);
      }
      xiaotu::shutdown_logging();
      return 0;
    }

    if (options.backup_daily) {
      if (!database_existed) {
        throw xiaotu::AppError("NOT_FOUND", "数据库不存在，无法备份：" + database_path.string());
      }
      const xiaotu::DailyBackupOutcome outcome = xiaotu::run_daily_backup(db, data_dir);
      if (outcome.ran) {
        std::cout << "每日备份完成: " << outcome.backup.path.string() << " ("
                  << outcome.backup.bytes << " 字节)，清理旧备份 "
                  << outcome.retention.removed << " 份" << std::endl;
      } else {
        std::cout << "跳过每日备份: " << outcome.skipped << std::endl;
      }
      xiaotu::shutdown_logging();
      return 0;
    }

    const int previous_version = db.schema_version();
    if (database_existed && previous_version > 0 &&
        previous_version < xiaotu::target_schema_version()) {
      const auto snapshot = xiaotu::backup_database(
          db, data_dir / "backups", "pre-v" + std::to_string(previous_version));
      std::cout << "迁移前备份: " << snapshot.path.string() << std::endl;
      xiaotu::log_info("迁移前备份 " + snapshot.path.string() +
                       " 字节=" + std::to_string(snapshot.bytes));
    }
    xiaotu::migrate(db);
    // 历史分裂修补：partner 额度为 0 而 customer 镜像非 0 时并回 partner（幂等）。
    // v28 已搬过数据，这里只兜底被旧版本/外部工具写歪的库。
    {
      std::size_t repaired = 0;
      db.write([&](sqlite3* conn) { repaired = xiaotu::repair_customer_credit_mirror(conn); });
      if (repaired > 0) {
        std::cout << "信用额度镜像修补: " << repaired << " 条" << std::endl;
        xiaotu::log_info("信用额度镜像修补 " + std::to_string(repaired) + " 条");
      }
    }
    xiaotu::load_role_permissions(db);
    std::cout << "数据库结构版本: v" << db.schema_version() << std::endl;
    xiaotu::log_info("数据库结构版本 v" + std::to_string(db.schema_version()));

    xiaotu::Services services(db);
    services.docs.set_credit_limit_mode(file_config.credit_limit_mode.value_or("warn"));
    if (!options.set_password_user.empty()) {
      const char* password = std::getenv("XIAOTU_PASSWORD");
      if (password == nullptr || *password == '\0') {
        throw xiaotu::AppError("BAD_ARGUMENT",
                               "请通过环境变量 XIAOTU_PASSWORD 提供新口令");
      }
      const xiaotu::UserRow user =
          services.users.find_by_username(options.set_password_user);
      services.users.set_password(user.id, password);
      std::cout << "口令已更新: " << user.username << std::endl;
      xiaotu::log_info("更新用户口令 " + user.username);
      xiaotu::shutdown_logging();
      return 0;
    }

    // 每日备份调度：单机版与网络版都在进程内兜底；网络版还可用 deploy 下的 timer/cron。
    DailyBackupScheduler backup_scheduler(db, data_dir);
    backup_scheduler.start();

    const std::string token = xiaotu::random_hex(32);
    xiaotu::ServerInfo info{
        .mode = options.server ? "server" : "local",
        .app_name = "小兔进销存",
        .version = XIAOTU_VERSION,
        .data_dir_warning = data_dir_warning,
        .data_dir = data_dir.string(),
        .database_path = database_path.string(),
        .listen_host = options.server ? listen : std::string{},
    };
    xiaotu::HttpServer http(services, info, token);

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    if (!options.server) {
      const int port = http.bind("127.0.0.1", 0);
      xiaotu::log_info("监听 127.0.0.1:" + std::to_string(port));
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
#if defined(XIAOTU_HAS_WEBVIEW)
      const int code = xiaotu::platform::run_webview(url, "小兔进销存");
      http.stop();
      xiaotu::log_info("退出");
      xiaotu::shutdown_logging();
      return code;
#else
      std::cout << "当前构建未包含 WebView 壳，请用浏览器打开上面的访问地址。" << std::endl;
      while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
      http.stop();
      xiaotu::log_info("退出");
      xiaotu::shutdown_logging();
      return 0;
#endif
    }

    const int bound_port = http.bind(listen, port);
    xiaotu::log_info("监听 " + listen + ":" + std::to_string(bound_port));
    http.serve_async();
    std::cout << "服务地址: http://" << listen << ":" << bound_port << "/\n"
              << "局域网访问时，把监听地址换成本机 IP；使用用户名与口令登录。" << std::endl;
    if (!services.users.has_password(1)) {
      std::cout << "警告: 内置管理员尚未设置口令。请先执行（服务可先停止）:\n"
                << "  XIAOTU_PASSWORD=<新口令> " << argv[0]
                << " --data <数据目录> --set-password admin" << std::endl;
      xiaotu::log_warn("内置管理员尚未设置口令");
    }

    while (!g_stop) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    http.stop();
    xiaotu::log_info("退出");
    xiaotu::shutdown_logging();
    return 0;
  } catch (const std::exception& error) {
    xiaotu::log_error(std::string("启动失败: ") + error.what());
    std::cerr << "启动失败: " << error.what() << std::endl;
    xiaotu::shutdown_logging();
    return 1;
  }
}
