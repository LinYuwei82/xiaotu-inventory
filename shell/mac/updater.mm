// 小兔进销存 macOS 壳更新桥（mote 模式薄端口，参考 mote/shell/mac/updater.mm）
//
// 状态机、清单判定、跳过/冷却/退避全部在 core/update_engine；本文件只提供
// （见 docs/更新模块拆分重构方案.md §6）：
//   - MacTransport：NSURLSession 拉清单（失败 2s 重试一次，冷 DNS 旧行为）；
//     下载不再自研——发布构建安装走 Sparkle 旁路。
//   - MacStore：NSUserDefaults 单键存引擎 JSON（旧散键迁移已删：线上 1.0.2x
//     壳已在写单键，load() 直接兼容）。
//   - 壳侧保留：24h±2h 定时器（读引擎 next_check_epoch）、Sparkle 旁路、
//     health min_supported 轻量握手（启动阻塞页）。
//   - 已删（与 mote 对齐）：自研预下载/UX 门、恢复模式/回退上一版本/备份副本、
//     skip/download/cancelPrefetch/resume 桥（前端未调用或随回滚层失效）。
// status 契约：引擎 status_json + 3 个 mac 扩展键（enabled/channel/install_engine）。

#include "updater.h"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "xiaotu/log.h"
#include "xiaotu/update.h"
#include "xiaotu/update_engine.h"

#if __has_include(<Sparkle/Sparkle.h>)
#import <Sparkle/Sparkle.h>
#define XIAOTU_HAS_SPARKLE 1
#else
#define XIAOTU_HAS_SPARKLE 0
#endif

// 发布构建不允许退化成「打开 dmg 让用户拖拽」——安装必须由 Sparkle 完成。
#if defined(XIAOTU_RELEASE_BUILD) && !XIAOTU_HAS_SPARKLE
#error "发布构建必须链接 Sparkle.framework：请提供 XIAOTU_SPARKLE_DIR，或使用仓库内 third_party/sparkle"
#endif

namespace {
// 引擎状态单键（唯一运行期状态键）。
NSString* const kEngineStateKey = @"XiaotuUpdateEngineState";
// 灰度分桶用的安装标识：首次检查时生成 UUID，之后保持不变。
NSString* const kInstallIDKey = @"XiaotuInstallID";
NSString* const kAutoCheckKey = @"XiaotuAutoCheckEnabled";
// 一次性清理旧壳残留目录的标记（previous.app 副本 / updates 预下载包）。
NSString* const kLegacyDirsCleanedKey = @"XiaotuUpdateLegacyDirsCleaned";

constexpr NSTimeInterval kRequestTimeout = 10;

// status.last_result 与 checkNow 的 code 取值，和交接单第 2、3 节保持一致。
NSString* const kResultCheckFailed = @"CHECK_FAILED";
NSString* const kResultNotConfigured = @"NOT_CONFIGURED";

NSDictionary* fetch_json(NSURL* url, NSString** errorOut) {
  __block NSDictionary* result = nil;
  __block NSString* failure = nil;
  dispatch_semaphore_t done = dispatch_semaphore_create(0);
  NSURLSessionConfiguration* config = [NSURLSessionConfiguration ephemeralSessionConfiguration];
  config.timeoutIntervalForRequest = kRequestTimeout;
  config.timeoutIntervalForResource = kRequestTimeout;
  NSURLSession* session = [NSURLSession sessionWithConfiguration:config];
  NSURLRequest* request = [NSURLRequest requestWithURL:url
                                           cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                                       timeoutInterval:kRequestTimeout];
  [[session dataTaskWithRequest:request
              completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
                if (error != nil) {
                  failure = [NSString stringWithFormat:@"网络错误 %ld：%@",
                             static_cast<long>(error.code),
                             error.localizedDescription ?: @"unknown"];
                } else if ([response isKindOfClass:[NSHTTPURLResponse class]]) {
                  const NSInteger status = static_cast<NSHTTPURLResponse*>(response).statusCode;
                  if (status != 200) {
                    failure = [NSString stringWithFormat:@"HTTP %ld", static_cast<long>(status)];
                  } else if (data != nil) {
                    NSDictionary* parsed = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
                    if ([parsed isKindOfClass:[NSDictionary class]]) {
                      result = parsed;
                    } else {
                      failure = @"响应不是 JSON 对象";
                    }
                  } else {
                    failure = @"响应为空";
                  }
                } else if (data != nil) {
                  NSDictionary* parsed = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
                  if ([parsed isKindOfClass:[NSDictionary class]]) {
                    result = parsed;
                  }
                }
                dispatch_semaphore_signal(done);
              }] resume];
  const long wait = dispatch_semaphore_wait(
      done, dispatch_time(DISPATCH_TIME_NOW,
                          static_cast<int64_t>((kRequestTimeout + 1) * NSEC_PER_SEC)));
  if (wait != 0) {
    failure = [NSString stringWithFormat:@"请求超时（%gs）", kRequestTimeout];
  }
  [session invalidateAndCancel];
  if (result == nil && errorOut != nullptr) {
    *errorOut = failure ?: @"未知原因";
  }
  return result;
}

// 引擎 ITransport::fetch 用：拿原始响应体（验签与解析交给引擎）。
bool fetch_raw_body(NSURL* url, std::string& body, std::string& err) {
  __block BOOL ok = NO;
  __block NSData* payload = nil;
  __block NSString* failure = nil;
  dispatch_semaphore_t done = dispatch_semaphore_create(0);
  NSURLSessionConfiguration* config = [NSURLSessionConfiguration ephemeralSessionConfiguration];
  config.timeoutIntervalForRequest = kRequestTimeout;
  config.timeoutIntervalForResource = kRequestTimeout;
  NSURLSession* session = [NSURLSession sessionWithConfiguration:config];
  NSURLRequest* request = [NSURLRequest requestWithURL:url
                                           cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                                       timeoutInterval:kRequestTimeout];
  [[session dataTaskWithRequest:request
              completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
                if (error != nil) {
                  failure = [NSString stringWithFormat:@"网络错误 %ld：%@",
                             static_cast<long>(error.code),
                             error.localizedDescription ?: @"unknown"];
                } else if ([response isKindOfClass:[NSHTTPURLResponse class]] &&
                           static_cast<NSHTTPURLResponse*>(response).statusCode != 200) {
                  failure = [NSString stringWithFormat:@"HTTP %ld",
                             static_cast<long>(static_cast<NSHTTPURLResponse*>(response).statusCode)];
                } else if (data.length == 0) {
                  failure = @"响应为空";
                } else {
                  payload = data;
                  ok = YES;
                }
                dispatch_semaphore_signal(done);
              }] resume];
  const long wait = dispatch_semaphore_wait(
      done, dispatch_time(DISPATCH_TIME_NOW,
                          static_cast<int64_t>((kRequestTimeout + 1) * NSEC_PER_SEC)));
  if (wait != 0) {
    failure = [NSString stringWithFormat:@"请求超时（%gs）", kRequestTimeout];
  }
  [session invalidateAndCancel];
  if (!ok) {
    err = failure.UTF8String ?: "未知原因";
    return false;
  }
  body.assign(static_cast<const char*>(payload.bytes), payload.length);
  return true;
}

NSString* json_string(NSDictionary* body, NSString* key) {
  id value = body[key];
  return [value isKindOfClass:[NSString class]] ? value : nil;
}

double now_epoch() {
  return [[NSDate date] timeIntervalSince1970];
}

std::int64_t current_build() {
  return static_cast<std::int64_t>(XIAOTU_BUILD);
}

// 灰度分桶标识：首次使用生成 UUID 并持久化，重装应用数据后才会变化。
NSString* installation_id() {
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  NSString* value = [defaults stringForKey:kInstallIDKey];
  if (value.length == 0) {
    value = [[NSUUID UUID] UUIDString];
    [defaults setObject:value forKey:kInstallIDKey];
  }
  return value;
}

// 本机 macOS 版本，形如 "14.1.2"，用于清单 min_os 判定（引擎 UpdateQuery 用）。
NSString* system_version_string() {
  NSOperatingSystemVersion version = [[NSProcessInfo processInfo] operatingSystemVersion];
  return [NSString stringWithFormat:@"%ld.%ld.%ld", static_cast<long>(version.majorVersion),
                                    static_cast<long>(version.minorVersion),
                                    static_cast<long>(version.patchVersion)];
}

bool auto_check_enabled() {
  return [[NSUserDefaults standardUserDefaults] boolForKey:kAutoCheckKey];
}

// Gatekeeper 会把从 dmg / 下载目录直接启动的应用放到只读的 AppTranslocation 路径，
// 此时 Sparkle 无法替换 bundle。提前识别并给出可执行指引，避免安装静默失败。
bool app_is_translocated() {
  return [[NSBundle mainBundle].bundlePath containsString:@"/AppTranslocation/"];
}

// 一次性清理旧壳（恢复模式/自研预下载时代）留下的目录：
//   Application Support/com.simem.xiaotu/previous —— 回退副本（可达数百 MB）
//   Application Support/com.simem.xiaotu/updates  —— 预下载包缓存
// 引擎 pending 包文件被删后由 load() 的「包丢失→回 UPDATE_AVAILABLE」分支自愈。
void clean_legacy_update_dirs_once() {
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  if ([defaults boolForKey:kLegacyDirsCleanedKey]) {
    return;
  }
  NSFileManager* manager = [NSFileManager defaultManager];
  NSURL* base = [manager URLForDirectory:NSApplicationSupportDirectory
                                inDomain:NSUserDomainMask
                       appropriateForURL:nil
                                  create:NO
                                   error:NULL];
  if (base != nil) {
    NSURL* previous = [base URLByAppendingPathComponent:@"com.simem.xiaotu/previous"
                                            isDirectory:YES];
    NSURL* updates = [base URLByAppendingPathComponent:@"com.simem.xiaotu/updates"
                                           isDirectory:YES];
    [manager removeItemAtURL:previous error:NULL];
    [manager removeItemAtURL:updates error:NULL];
  }
  [defaults setBool:YES forKey:kLegacyDirsCleanedKey];
}

// ---- 引擎端口实现（方案 §6.2） ----

class MacClock : public xiaotu::IClock {
 public:
  std::int64_t now_epoch() const override {
    return static_cast<std::int64_t>([[NSDate date] timeIntervalSince1970]);
  }
};

class MacStore : public xiaotu::IStore {
 public:
  bool load(std::string& json, std::string& err) override {
    @autoreleasepool {
      NSString* value = [[NSUserDefaults standardUserDefaults] stringForKey:kEngineStateKey];
      if (value.length == 0) {
        err = "ENGINE_STATE_EMPTY";
        return false;
      }
      json = value.UTF8String;
      return true;
    }
  }

  bool save(const std::string& json, std::string& err) override {
    @autoreleasepool {
      NSString* value = @(json.c_str());
      if (value == nil) {
        err = "ENGINE_STATE_INVALID";
        return false;
      }
      [[NSUserDefaults standardUserDefaults] setObject:value forKey:kEngineStateKey];
      return true;
    }
  }
};

class MacTransport : public xiaotu::ITransport {
 public:
  bool fetch(const std::string& url, std::string& body, std::string& err) override {
    @autoreleasepool {
      NSURL* nsURL = [NSURL URLWithString:@(url.c_str())];
      if (nsURL == nil) {
        err = "清单地址无效";
        return false;
      }
      // 首次启动常见冷 DNS / CDN 首包慢，重试一次再判失败，避免误判后等一个周期。
      for (int attempt = 0; attempt < 2; ++attempt) {
        if (attempt > 0) {
          [NSThread sleepForTimeInterval:2.0];
        }
        if (fetch_raw_body(nsURL, body, err)) {
          return true;
        }
      }
      return false;
    }
  }

  // mac 不再自研下载：发布构建安装走 Sparkle 旁路（下载/验签/替换/重启全包）。
  bool download(const std::string&, const std::filesystem::path&,
                const xiaotu::ProgressFn&, const xiaotu::CancelFn&,
                std::string& err) override {
    err = "mac 安装由 Sparkle 完成";
    return false;
  }
};

// 引擎 status 字段安全读取（缺失/类型不符时给默认值）。
std::string jstr(const nlohmann::json& j, const char* key) {
  if (j.is_object() && j.contains(key) && j[key].is_string()) {
    return j[key].get<std::string>();
  }
  return {};
}

std::int64_t ji64(const nlohmann::json& j, const char* key) {
  if (j.is_object() && j.contains(key) && j[key].is_number_integer()) {
    return j[key].get<std::int64_t>();
  }
  return 0;
}

}  // namespace

#if XIAOTU_HAS_SPARKLE
@interface XiaotuUpdater () <SPUUpdaterDelegate>
#else
@interface XiaotuUpdater ()
#endif
{
  std::unique_ptr<MacStore> _store;
  std::unique_ptr<MacTransport> _transport;
  std::unique_ptr<MacClock> _clock;
  std::unique_ptr<xiaotu::UpdateEngine> _engine;
}
@property(nonatomic, strong) dispatch_queue_t queue;
@property(nonatomic, copy, nullable) NSString* manifestURL;
@property(nonatomic, copy, nullable) NSString* healthBase;
@property(nonatomic, copy) NSString* channel;
@property(nonatomic, assign) BOOL running;
// 递增令牌用于作废旧定时器；只在 self.queue 上读写。
@property(nonatomic, assign) NSUInteger timerToken;
#if XIAOTU_HAS_SPARKLE
// 必须持有，否则控制器释放后 Sparkle 的后续下载/安装会中断。只在主线程访问。
@property(nonatomic, strong, nullable) SPUStandardUpdaterController* sparkleController;
#endif
@end

@implementation XiaotuUpdater

+ (instancetype)shared {
  static XiaotuUpdater* instance = nil;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    instance = [[XiaotuUpdater alloc] init];
  });
  return instance;
}

- (instancetype)init {
  self = [super init];
  if (self != nil) {
    _queue = dispatch_queue_create("com.simem.xiaotu.update", DISPATCH_QUEUE_SERIAL);
    _channel = @"stable";
    // 开关默认开启：registerDefaults 只影响「从未写过」的键，用户改过就保留用户值。
    [[NSUserDefaults standardUserDefaults] registerDefaults:@{kAutoCheckKey : @YES}];
    clean_legacy_update_dirs_once();

    _store.reset(new MacStore());
    _transport.reset(new MacTransport());
    _clock.reset(new MacClock());

    xiaotu::EngineConfig cfg;
    cfg.platform = "macos";
    cfg.channel = "stable";
    cfg.install_id = installation_id().UTF8String ?: "";
    cfg.manual_cooldown_seconds = 60;
    cfg.auto_check_interval_seconds = 86400;
    cfg.auto_check_jitter_seconds = 7200;
    cfg.first_check_delay_seconds = 30;
    cfg.os_version = system_version_string().UTF8String ?: "";
    cfg.ignore_min_os = false;
    cfg.enabled = true;
    cfg.auto_check = auto_check_enabled();
    // mac 安装走 Sparkle，无自研预下载。
    cfg.prefetch = false;
    NSString* pubKey = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"SUPublicEDKey"];
    if (pubKey.length > 0) {
      cfg.public_key_base64 = pubKey.UTF8String;
    } else {
      xiaotu::log_warn("更新：Info.plist 未配置 SUPublicEDKey，跳过清单验签");
    }

    xiaotu::UpdateEngine::Deps deps;
    deps.transport = _transport.get();
    deps.clock = _clock.get();
    deps.store = _store.get();
    _engine.reset(new xiaotu::UpdateEngine(deps, cfg));
    // 恢复引擎状态：含崩溃恢复与「已装覆盖残留 pending」清理。
    _engine->load();
  }
  return self;
}

- (nlohmann::json)engineStatus {
  if (_engine == nullptr) {
    return nlohmann::json::object();
  }
  nlohmann::json j = _engine->status();
  if (j.is_discarded() || !j.is_object()) {
    return nlohmann::json::object();
  }
  return j;
}

- (void)startWithManifestURL:(NSString*)manifestURL
                  healthBase:(NSString*)healthBase
                     channel:(NSString*)channel {
  @synchronized(self) {
    // 轻量握手只依赖 health，即使未配置清单也要保留。
    self.healthBase = healthBase;
    self.channel = channel.length > 0 ? channel : @"stable";
    if (manifestURL.length == 0) {
      xiaotu::log_info("更新：未配置清单地址，自动检查关闭");
      return;
    }
    if (self.running) {
      return;
    }
    self.running = YES;
    self.manifestURL = manifestURL;
    _engine->configure(manifestURL.UTF8String, self.channel.UTF8String);
  }
  dispatch_async(self.queue, ^{
    [self checkIfDue];
    [self armTimerOnQueue];
  });
  // Info.plist 中 SUEnableAutomaticChecks=false：定时检查只由本类负责，
  // 避免 Sparkle 与本类形成双份检查/提示（见 docs/更新模块技术方案.md 第 9 节）。
}

// 只处理 auto_check；prefetch / ack_prefetch_notice 等旧键忽略。
- (void)setOptions:(id)options
        completion:(void (^)(NSDictionary<NSString*, id>* result))completion {
  void (^respond)(NSDictionary*) = ^(NSDictionary* result) {
    dispatch_async(dispatch_get_main_queue(), ^{
      completion(result);
    });
  };
  NSDictionary* body = [options isKindOfClass:[NSDictionary class]] ? options : @{};
  id autoValue = body[@"auto_check"];
  if (autoValue != nil && ![autoValue isKindOfClass:[NSNumber class]]) {
    respond(@{
      @"ok" : @NO,
      @"code" : @"INVALID_ARGUMENT",
      @"status" : [self status],
    });
    return;
  }
  if (autoValue != nil) {
    [[NSUserDefaults standardUserDefaults] setBool:[autoValue boolValue] forKey:kAutoCheckKey];
  }
  _engine->set_options(true, auto_check_enabled(), false);
  dispatch_async(self.queue, ^{
    if (autoValue != nil) {
      if (auto_check_enabled()) {
        [self checkIfDue];
      } else {
        // 递增令牌作废已武装的定时器；手动检查不受影响。
        self.timerToken += 1;
      }
    }
    respond(@{
      @"ok" : @YES,
      @"code" : @"UPDATED",
      @"status" : [self status],
    });
  });
}

- (void)installUpdateWithCompletion:(void (^)(NSDictionary<NSString*, id>* result))completion {
  void (^respond)(NSDictionary*) = ^(NSDictionary* result) {
    dispatch_async(dispatch_get_main_queue(), ^{
      completion(result);
    });
  };
  // 从 dmg / 下载目录直接启动时，系统把 app 挂在只读路径，Sparkle 无法替换。
  if (app_is_translocated()) {
    respond(@{
      @"ok" : @NO,
      @"code" : @"TRANSLOCATED",
      @"message" : @"当前程序从只读位置运行（例如直接打开 dmg），"
                   @"请先把「小兔进销存」拖入「应用程序」并从那里启动，再执行更新。",
    });
    return;
  }
  nlohmann::json s = [self engineStatus];
  const std::string pendingVer = jstr(s, "version");
#if XIAOTU_HAS_SPARKLE
  // 引擎的安装判定口径：v2 macOS 清单带构建号；无待装版本直接返回。
  if (pendingVer.empty() ||
      !xiaotu::is_update_newer(pendingVer, ji64(s, "build"),
                               xiaotu::current_product_version(), current_build())) {
    respond(@{
      @"ok" : @NO,
      @"code" : @"NO_PENDING_UPDATE",
      @"message" : @"没有待安装的版本",
    });
    return;
  }
  // 脚本消息在主线程派发；控制器也只在主线程创建。
  // Sparkle 负责下载、appcast EdDSA 验签、原子替换 .app 并自动重启；
  // 自研层只做「有没有新版本 / 灰度 / 强制」判定。
  dispatch_async(dispatch_get_main_queue(), ^{
    [[[self sparkleController] updater] checkForUpdates];
  });
  respond(@{@"ok" : @YES, @"code" : @"STARTED"});
#else
  // dev 构建（未链接 Sparkle）没有自动安装能力，前端按失败文案提示手动下载。
  (void)pendingVer;
  respond(@{
    @"ok" : @NO,
    @"code" : @"UNAVAILABLE",
    @"message" : @"当前构建没有可用的自动安装（未链接 Sparkle），请手动下载安装",
  });
#endif
}

- (void)checkNowWithCompletion:(void (^)(NSDictionary<NSString*, id>* result))completion {
  void (^respond)(NSDictionary*) = ^(NSDictionary* result) {
    dispatch_async(dispatch_get_main_queue(), ^{
      completion(result);
    });
  };
  if (self.manifestURL.length == 0) {
    respond(@{
      @"ok" : @NO,
      @"code" : kResultNotConfigured,
      @"update_available" : @NO,
      @"status" : [self status],
    });
    return;
  }
  dispatch_async(self.queue, ^{
    const bool judged = _engine->check(true);
    nlohmann::json s = [self engineStatus];
    const bool hasPending = !jstr(s, "version").empty();
    if (!judged && jstr(s, "last_result") == "COOLDOWN") {
      respond(@{
        @"ok" : @NO,
        @"code" : @"COOLDOWN",
        @"retry_after" : @(ji64(s, "retry_after")),
        @"update_available" : @NO,
        @"status" : [self status],
      });
      return;
    }
    [self armTimerOnQueue];
    respond(@{
      @"ok" : @(judged),
      @"code" : judged ? @"CHECKED" : kResultCheckFailed,
      @"update_available" : @(hasPending),
      @"status" : [self status],
    });
  });
}

// 轻量握手：服务端 health 里的 min_supported_version 高于当前壳版本时返回 blocked。
// 与定时检查分开，启动即可调用，不必等首轮自动检查。
- (void)checkMinSupportedVersionWithCompletion:
    (void (^)(NSDictionary<NSString*, id>* result))completion {
  void (^respond)(NSDictionary*) = ^(NSDictionary* result) {
    dispatch_async(dispatch_get_main_queue(), ^{
      completion(result);
    });
  };
  NSString* base = self.healthBase;
  if (base.length == 0) {
    respond(@{@"blocked" : @NO});
    return;
  }
  dispatch_async(self.queue, ^{
    NSDictionary* health = fetch_json([NSURL URLWithString:[base stringByAppendingString:@"/api/health"]], nil);
    NSString* minimum = json_string(health, @"min_supported_version");
    NSString* current = [NSString stringWithUTF8String:xiaotu::current_product_version()];
    if (minimum.length == 0) {
      respond(@{@"blocked" : @NO, @"current" : current ?: @""});
      return;
    }
    xiaotu::ProductVersion currentVersion{};
    xiaotu::ProductVersion requiredVersion{};
    const BOOL blocked =
        xiaotu::parse_product_version(current.UTF8String, currentVersion) &&
        xiaotu::parse_product_version(minimum.UTF8String, requiredVersion) &&
        xiaotu::compare_product_version(currentVersion, requiredVersion) < 0;
    respond(@{
      @"blocked" : @(blocked),
      @"minimum" : minimum,
      @"current" : current ?: @"",
    });
  });
}

- (NSDictionary<NSString*, id>*)status {
  // 以引擎统一 status_json 为底（方案 §2.4），壳只补 3 个 mac 扩展键；
  // 前端 normalizeUpdateStatus 负责归一成内部形状。
  nlohmann::json s = [self engineStatus];
  NSMutableDictionary<NSString*, id>* out = [NSMutableDictionary dictionary];
  const std::string text = s.dump();
  NSData* data = [NSData dataWithBytes:text.data() length:text.size()];
  id base = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
  if ([base isKindOfClass:[NSDictionary class]]) {
    [out addEntriesFromDictionary:base];
  }
  // enabled 维持旧语义：配置了清单地址即启用（引擎的 enabled 是总开关位）。
  out[@"enabled"] = @(self.manifestURL.length > 0);
  out[@"channel"] = self.channel ?: @"stable";
#if XIAOTU_HAS_SPARKLE
  // 安装引擎：sparkle 表示下载与替换都由 Sparkle 完成，前端隐藏自研下载入口。
  out[@"install_engine"] = @"sparkle";
#else
  out[@"install_engine"] = @"package";
#endif
  return out;
}

// 按引擎 next_check_epoch 武装一次性定时器；必须在 self.queue 上调用。
- (void)armTimerOnQueue {
  self.timerToken += 1;
  const NSUInteger token = self.timerToken;
  if (!self.running || self.manifestURL.length == 0) {
    return;
  }
  // 自动检查关闭时不武装任何定时器；手动检查仍可走 checkNow。
  if (!auto_check_enabled()) {
    return;
  }
  nlohmann::json s = [self engineStatus];
  const double delay = static_cast<double>(ji64(s, "next_check_epoch")) - now_epoch();
  const int64_t nanoseconds = static_cast<int64_t>(MAX(1.0, delay) * NSEC_PER_SEC);
  __weak XiaotuUpdater* weakSelf = self;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, nanoseconds), self.queue, ^{
    XiaotuUpdater* strongSelf = weakSelf;
    if (strongSelf == nil || token != strongSelf.timerToken) {
      return;  // 已被更新的排期取代
    }
    [strongSelf checkIfDue];
    [strongSelf armTimerOnQueue];
  });
}

// 到期自动检查：间隔/退避门槛由引擎内部把关（check(false) 未到期直接返回）。
// 必须在 self.queue 上调用。Sparkle 不做后台自查（mote 同款）：
// 下载与安装只在用户于关于页显式点「安装更新」时由 Sparkle 触发。
- (void)checkIfDue {
  if (!self.running || self.manifestURL.length == 0 || !auto_check_enabled()) {
    return;
  }
  const bool judged = _engine->check(false);
  if (!judged) {
    return;
  }
  nlohmann::json s = [self engineStatus];
  if (jstr(s, "state") == "UPDATE_AVAILABLE") {
    xiaotu::log_info("更新：发现新版本 " + jstr(s, "version"));
  }
}

#if XIAOTU_HAS_SPARKLE
- (SPUStandardUpdaterController*)sparkleController {
  if (_sparkleController == nil) {
    _sparkleController =
        [[SPUStandardUpdaterController alloc] initWithStartingUpdater:YES
                                                    updaterDelegate:self
                                                 userDriverDelegate:nil];
  }
  return _sparkleController;
}

// MARK: - SPUUpdaterDelegate — 拦截 Sparkle 默认弹窗
//
// 根因（2026-11-19 用户截图）：壳层有两条独立更新链路：
//   ① 自研 manifest_v2（XiaotuUpdater 读清单 → 自定义 UI → 前端 showNotice）
//   ② Sparkle（SPUStandardUpdaterController 自己拉 appcast.xml 自己弹默认窗）
// 两者并行时，Sparkle 不管 manifest_v2 的 pending_version 状态，按自己的版本比较
// 弹一个"您使用的就是最新版！"的默认 NSAlert，与自研链路的"发现新版本"UI 打架。
// 注册 updaterDelegate 为 self 后，下面三个空实现会让 Sparkle 完全哑巴——
// 它只负责下载和安装，UI 控制权全部交还给自研 manifest_v2 链路。

// Sparkle 检查后没发现新版本 → 拦截默认"已是最新"弹窗。
- (void)updater:(SPUUpdater*)updater didNotFindUpdate:(NSError* _Nullable)error {}

// 用户在 Sparkle 默认面板里取消检查 → 静默忽略。
- (void)updaterUserDidCancel:(SPUUpdater*)updater error:(NSError* _Nullable)error {}

// Sparkle 完成一次检查循环（无论结果）→ 拦截结束时的状态弹窗。
- (void)updater:(SPUUpdater*)updater
    didFinishUpdateCycleForUpdate:(SPUStandardUpdaterController*)updaterController
                            error:(NSError* _Nullable)error {}
#endif

@end
