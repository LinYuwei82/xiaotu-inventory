// 小兔进销存 macOS 壳更新桥（P2：core/update_engine 薄端口）
//
// 状态机、清单判定、下载校验、跳过/冷却/退避、崩溃恢复全部在 core/update_engine；
// 本文件只提供（见 docs/更新模块拆分重构方案.md §6）：
//   - MacTransport：NSURLSession 拉清单（失败 2s 重试一次，冷 DNS 旧行为）/
//     下载（写 .part → 进度回调 → 原子改名）
//   - MacHasher：CommonCrypto 流式 SHA-256
//   - MacInstaller：仅 dev 构建（打开 dmg 兜底）；发布构建安装走 Sparkle 旁路
//   - MacStore：NSUserDefaults 单键存引擎 JSON + 旧散键一次性迁移
//   - 壳侧保留：Sparkle 旁路、prefetch UX 门（流量提示确认/自动重试/取消）、
//     定时器（读引擎 next_check_epoch）、恢复模式与回滚、health 轻量握手。
// 桥方法签名与 status 字段集合保持前端契约不变（docs/前端交接-更新模块.md 第 2、3 节）。

#include "updater.h"

#import <AppKit/AppKit.h>
#import <CommonCrypto/CommonDigest.h>
#import <Foundation/Foundation.h>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unistd.h>

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
// ---- 引擎状态单键 + 旧散键（一次性迁移 + 壳侧仍使用的键） ----
NSString* const kEngineStateKey = @"XiaotuUpdateEngineState";

NSString* const kSkippedVersionKey = @"XiaotuSkippedVersion";
NSString* const kSkipUntilKey = @"XiaotuSkipUntil";
NSString* const kNextCheckKey = @"XiaotuNextCheckAt";
NSString* const kLaunchAttemptsKey = @"XiaotuLaunchAttempts";
NSString* const kPendingBuildKey = @"XiaotuPendingBuild";
NSString* const kPendingVersionKey = @"XiaotuPendingVersion";
NSString* const kPendingReasonKey = @"XiaotuPendingReason";
NSString* const kPendingDeadlineKey = @"XiaotuPendingDeadline";
NSString* const kPendingNotesKey = @"XiaotuPendingNotes";
NSString* const kMandatoryKey = @"XiaotuPendingMandatory";
NSString* const kIntermediateVersionKey = @"XiaotuIntermediateVersion";
NSString* const kIntermediateBuildKey = @"XiaotuIntermediateBuild";
NSString* const kIntermediateSchemaKey = @"XiaotuIntermediateSchema";
NSString* const kPausedKey = @"XiaotuUpdatePaused";
NSString* const kLastCheckKey = @"XiaotuLastCheckAt";
NSString* const kLastManualCheckKey = @"XiaotuLastManualCheckAt";
NSString* const kLastResultKey = @"XiaotuLastCheckResult";
// 清单检查失败计数：成功一次即清零；失败后按 1/10/60 分钟退避（引擎口径）。
NSString* const kCheckFailuresKey = @"XiaotuCheckFailures";
// 服务端轻量握手要求的最低客户端版本；未配置或从未握手为 nil。
NSString* const kMinSupportedKey = @"XiaotuMinSupportedVersion";
// 灰度分桶用的安装标识：首次检查时生成 UUID，之后保持不变。
NSString* const kInstallIDKey = @"XiaotuInstallID";
NSString* const kAutoCheckKey = @"XiaotuAutoCheckEnabled";
NSString* const kPrefetchKey = @"XiaotuPrefetchEnabled";
NSString* const kPrefetchNoticeKey = @"XiaotuPrefetchNoticeAcked";
// 旧版预下载散键：仅迁移时读取，运行期状态以引擎为准。
NSString* const kPrefetchStateKey = @"XiaotuPrefetchState";
NSString* const kPrefetchFileKey = @"XiaotuPrefetchFile";
NSString* const kPendingURLKey = @"XiaotuPendingPackageURL";
NSString* const kPendingSHAKey = @"XiaotuPendingPackageSHA256";
NSString* const kPendingSizeKey = @"XiaotuPendingPackageSize";
// 升级验证：安装时记下目标版本，新版本稳定启动 3 次后清理旧包。
// v2 清单的 macOS 没有构建号，验证期只能用版本号表示。
NSString* const kValidatingVersionKey = @"XiaotuValidatingVersion";
NSString* const kStableLaunchKey = @"XiaotuStableLaunchCount";
// 恢复模式：连续启动失败达到阈值后置位，直到用户回退或选择继续使用当前版本。
NSString* const kRecoveryRequiredKey = @"XiaotuUpdateRecoveryRequired";
NSString* const kPreviousVersionKey = @"XiaotuPreviousVersion";
NSString* const kPreviousBuildKey = @"XiaotuPreviousBuild";

// 后台预下载展示状态（前端契约取值；由引擎状态 + 壳侧门叠加推导，见 status）。
NSString* const kPrefetchIdle = @"idle";
NSString* const kPrefetchNeedsConsent = @"needs_consent";
NSString* const kPrefetchRunning = @"running";
NSString* const kPrefetchRetrying = @"retrying";
NSString* const kPrefetchDone = @"done";
NSString* const kPrefetchFailed = @"failed";
NSString* const kPrefetchCanceled = @"canceled";

constexpr int kMaxPrefetchAttempts = 3;
constexpr NSTimeInterval kPrefetchRetryFirstDelay = 60;   // 首次失败后 1 分钟
constexpr NSTimeInterval kPrefetchRetrySecondDelay = 600;  // 第二次失败后 10 分钟

constexpr NSTimeInterval kRequestTimeout = 10;
constexpr NSTimeInterval kDownloadRequestTimeout = 30;
constexpr NSTimeInterval kDownloadResourceTimeout = 1800;
constexpr int kMaxLaunchFailures = 3;
// 新版本连续稳定运行 3 次后，删除安装前保留的旧版本副本。
constexpr int kStableLaunchesBeforeCleanup = 3;

// status.last_result 与 checkNow 的 code 取值，和交接单第 2、3 节保持一致。
NSString* const kResultCheckFailed = @"CHECK_FAILED";
NSString* const kResultNotConfigured = @"NOT_CONFIGURED";
NSString* const kResultPaused = @"PAUSED";
NSString* const kResultNoUpdate = @"NO_UPDATE";
NSString* const kResultUpdateAvailable = @"UPDATE_AVAILABLE";
NSString* const kResultIncompatible = @"INCOMPATIBLE_SCHEMA";

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

bool prefetch_enabled() {
  return [[NSUserDefaults standardUserDefaults] boolForKey:kPrefetchKey];
}

// Gatekeeper 会把从 dmg / 下载目录直接启动的应用放到只读的 AppTranslocation 路径，
// 此时 Sparkle 无法替换 bundle。提前识别并给出可执行指引，避免安装静默失败。
bool app_is_translocated() {
  return [[NSBundle mainBundle].bundlePath containsString:@"/AppTranslocation/"];
}

// 预下载缓存目录：Application Support 下按 bundle id 分开，卸载时随应用数据一起清理。
// 引擎的 download_dir 指向这里，包名由引擎按产物 URL 决定。
NSURL* prefetch_directory() {
  NSFileManager* manager = [NSFileManager defaultManager];
  NSURL* base = [manager URLForDirectory:NSApplicationSupportDirectory
                                inDomain:NSUserDomainMask
                       appropriateForURL:nil
                                  create:YES
                                   error:NULL];
  if (base == nil) {
    return nil;
  }
  NSURL* directory = [base URLByAppendingPathComponent:@"com.simem.xiaotu/updates"
                                           isDirectory:YES];
  [manager createDirectoryAtURL:directory
      withIntermediateDirectories:YES
                       attributes:nil
                            error:NULL];
  return directory;
}

// 回退副本目录：只保留最近 1 份升级前 .app，与预下载缓存分开。
NSURL* previous_directory() {
  NSFileManager* manager = [NSFileManager defaultManager];
  NSURL* base = [manager URLForDirectory:NSApplicationSupportDirectory
                                inDomain:NSUserDomainMask
                       appropriateForURL:nil
                                  create:YES
                                   error:NULL];
  if (base == nil) {
    return nil;
  }
  NSURL* directory = [base URLByAppendingPathComponent:@"com.simem.xiaotu/previous"
                                           isDirectory:YES];
  [manager createDirectoryAtURL:directory
      withIntermediateDirectories:YES
                       attributes:nil
                            error:NULL];
  return directory;
}

NSURL* previous_app_url() {
  NSURL* directory = previous_directory();
  return directory != nil ? [directory URLByAppendingPathComponent:@"previous.app"] : nil;
}

// 备份目录：服务端自动备份（含升级前的 pre-v* 库）都在这里。
NSURL* backup_directory() {
  NSFileManager* manager = [NSFileManager defaultManager];
  NSURL* base = [manager URLForDirectory:NSApplicationSupportDirectory
                                inDomain:NSUserDomainMask
                       appropriateForURL:nil
                                  create:NO
                                   error:NULL];
  return base != nil ? [base URLByAppendingPathComponent:@"com.simem.xiaotu/backups"
                                             isDirectory:YES]
                     : nil;
}

// 单引号转义，保证含空格、引号的路径原样传给 /bin/sh。
NSString* shell_quote(NSString* text) {
  NSString* escaped = [text stringByReplacingOccurrencesOfString:@"'" withString:@"'\\''"];
  return [NSString stringWithFormat:@"'%@'", escaped];
}

// 安装包 SHA-256：用 CommonCrypto 流式读取，避免把几十 MB 的包整个读进内存。
NSString* sha256_of_file(NSURL* url) {
  NSInputStream* stream = [NSInputStream inputStreamWithURL:url];
  if (stream == nil) {
    return nil;
  }
  [stream open];
  CC_SHA256_CTX context;
  CC_SHA256_Init(&context);
  uint8_t buffer[64 * 1024];
  NSInteger read = 0;
  while ((read = [stream read:buffer maxLength:sizeof(buffer)]) > 0) {
    CC_SHA256_Update(&context, buffer, static_cast<CC_LONG>(read));
  }
  [stream close];
  if (read < 0) {
    return nil;
  }
  unsigned char digest[CC_SHA256_DIGEST_LENGTH];
  CC_SHA256_Final(digest, &context);
  NSMutableString* hex = [NSMutableString stringWithCapacity:CC_SHA256_DIGEST_LENGTH * 2];
  for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; ++i) {
    [hex appendFormat:@"%02x", digest[i]];
  }
  return hex;
}

// ---- 旧散键 → 引擎 JSON 一次性迁移（方案 §6.3） ----
// 仅在新单键不存在时执行；引擎 load() 会在此基础上做崩溃恢复与残留清理。
NSString* migrate_legacy_engine_state() {
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  NSString* pendingVersion = [defaults stringForKey:kPendingVersionKey];
  NSString* legacyPrefetchState = [defaults stringForKey:kPrefetchStateKey];
  NSString* legacyPrefetchFile = [defaults stringForKey:kPrefetchFileKey];
  const BOOL hasPending = pendingVersion.length > 0;
  const BOOL fileExists =
      legacyPrefetchFile.length > 0 &&
      [[NSFileManager defaultManager] fileExistsAtPath:legacyPrefetchFile];

  std::string state = "IDLE";
  if (hasPending) {
    if ([legacyPrefetchState isEqualToString:kPrefetchDone] && fileExists) {
      state = "DONE";
    } else if ([legacyPrefetchState isEqualToString:kPrefetchRunning] ||
               [legacyPrefetchState isEqualToString:kPrefetchRetrying]) {
      state = "DOWNLOADING";  // 引擎 load() 的崩溃恢复会退回 UPDATE_AVAILABLE
    } else {
      state = "UPDATE_AVAILABLE";
    }
  }

  nlohmann::json j;
  j["state"] = state;
  j["last_result"] = [defaults stringForKey:kLastResultKey] ?
      std::string([[defaults stringForKey:kLastResultKey] UTF8String]) : std::string();
  j["last_error"] = "";
  j["last_check_epoch"] = static_cast<std::int64_t>([defaults doubleForKey:kLastCheckKey]);
  j["next_check_epoch"] = static_cast<std::int64_t>([defaults doubleForKey:kNextCheckKey]);
  j["last_manual_check_epoch"] =
      static_cast<std::int64_t>([defaults doubleForKey:kLastManualCheckKey]);
  j["consecutive_failures"] = static_cast<int>([defaults integerForKey:kCheckFailuresKey]);
  j["install_id"] = std::string(installation_id().UTF8String ?: "");
  j["manifest_version"] = hasPending ? std::string(pendingVersion.UTF8String) : std::string();
  j["manifest_build"] = static_cast<std::int64_t>([defaults integerForKey:kPendingBuildKey]);
  NSString* sha = [defaults stringForKey:kPendingSHAKey];
  j["manifest_sha256"] = sha.length > 0 ? std::string(sha.UTF8String) : std::string();
  const std::int64_t pendingSize =
      hasPending ? static_cast<std::int64_t>([defaults integerForKey:kPendingSizeKey]) : 0;
  j["manifest_size"] = pendingSize;
  NSString* pkgURL = [defaults stringForKey:kPendingURLKey];
  j["manifest_setup_url"] = pkgURL.length > 0 ? std::string(pkgURL.UTF8String) : std::string();
  NSString* notes = [defaults stringForKey:kPendingNotesKey];
  j["notes_url"] = notes.length > 0 ? std::string(notes.UTF8String) : std::string();
  j["mandatory"] = [defaults boolForKey:kMandatoryKey];
  NSString* reason = [defaults stringForKey:kPendingReasonKey];
  j["mandatory_reason"] = reason.length > 0 ? std::string(reason.UTF8String) : std::string();
  NSString* deadline = [defaults stringForKey:kPendingDeadlineKey];
  j["mandatory_deadline"] =
      deadline.length > 0 ? std::string(deadline.UTF8String) : std::string();
  if (state == "DONE") {
    j["pending_path"] = std::string(legacyPrefetchFile.UTF8String);
    j["pending_size"] = pendingSize;
    j["bytes_done"] = pendingSize;
    j["bytes_total"] = pendingSize;
  } else {
    j["pending_path"] = "";
    j["pending_size"] = 0;
    j["bytes_done"] = 0;
    j["bytes_total"] = 0;
  }
  NSString* skipped = [defaults stringForKey:kSkippedVersionKey];
  j["skipped_version"] = skipped.length > 0 ? std::string(skipped.UTF8String) : std::string();
  j["skip_until_epoch"] = static_cast<std::int64_t>([defaults doubleForKey:kSkipUntilKey]);
  j["enabled"] = true;
  j["auto_check"] = auto_check_enabled();
  j["prefetch"] = prefetch_enabled();
  // channel 不迁移：引擎以 configure() 注入的构建频道为准。
  return @(j.dump().c_str());
}

}  // namespace

// ---- 引擎下载代理与端口实现（方案 §6.2） ----

class MacClock : public xiaotu::IClock {
 public:
  std::int64_t now_epoch() const override {
    return static_cast<std::int64_t>([[NSDate date] timeIntervalSince1970]);
  }
};

class MacHasher : public xiaotu::IHasher {
 public:
  bool sha256_file(const std::filesystem::path& p, std::string& hex, std::string& err) override {
    @autoreleasepool {
      NSString* path = @(p.c_str());
      NSString* value = sha256_of_file([NSURL fileURLWithPath:path]);
      if (value == nil) {
        err = "SHA256_COMPUTE_FAILED";
        return false;
      }
      hex = value.UTF8String;
      return true;
    }
  }
};

class MacStore : public xiaotu::IStore {
 public:
  bool load(std::string& json, std::string& err) override {
    @autoreleasepool {
      NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
      NSString* value = [defaults stringForKey:kEngineStateKey];
      if (value.length == 0) {
        // 一次性迁移：把旧散键收拢成引擎 schema 并回写单键。
        value = migrate_legacy_engine_state();
        [defaults setObject:value forKey:kEngineStateKey];
      }
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

// 引擎下载用的 URLSession 下载代理：写 dest.part，进度与取消都经回调。
// 必须位于全局作用域——ObjC 类声明不能放进 C++ namespace。
@interface XiaotuEngineDownload : NSObject <NSURLSessionDownloadDelegate>
@property(nonatomic, strong) NSURLSession* session;
@property(nonatomic, strong) NSURLSessionDownloadTask* task;
@property(nonatomic, strong) NSURL* partURL;
// 会被取消方与 URLSession 回调线程同时访问，保持原子属性。
@property(atomic, assign) BOOL cancelled;
// 轮询引擎 cancel_flag_：返回 true 立即中断下载。
@property(atomic, copy, nullable) bool (^shouldCancel)();
@property(atomic, copy, nullable) void (^onProgress)(int64_t received, int64_t total);
@property(atomic, copy, nullable) void (^onFinish)(NSError* error);
@end

@implementation XiaotuEngineDownload

- (void)startWithURL:(NSURL*)url destination:(NSURL*)destination {
  self.partURL = destination;
  NSURLSessionConfiguration* config = [NSURLSessionConfiguration ephemeralSessionConfiguration];
  // 安装包可能较大，单次请求超时留足时间；连接失败仍由系统尽快返回。
  config.timeoutIntervalForRequest = kDownloadRequestTimeout;
  config.timeoutIntervalForResource = kDownloadResourceTimeout;
  self.session = [NSURLSession sessionWithConfiguration:config delegate:self delegateQueue:nil];
  self.task = [self.session downloadTaskWithURL:url];
  [self.task resume];
}

- (void)cancel {
  self.cancelled = YES;
  self.shouldCancel = nil;
  self.onProgress = nil;
  self.onFinish = nil;
  [self.task cancel];
  [self.session invalidateAndCancel];
}

- (void)URLSession:(NSURLSession*)session
              downloadTask:(NSURLSessionDownloadTask*)downloadTask
              didWriteData:(int64_t)bytesWritten
     totalBytesWritten:(int64_t)totalBytesWritten
totalBytesExpectedToWrite:(int64_t)totalBytesExpectedToWrite {
  bool (^shouldCancel)() = self.shouldCancel;
  if (shouldCancel != nil && shouldCancel()) {
    self.cancelled = YES;
    [downloadTask cancel];
    return;
  }
  void (^onProgress)(int64_t, int64_t) = self.onProgress;
  if (onProgress != nil) {
    onProgress(totalBytesWritten, totalBytesExpectedToWrite);
  }
}

- (void)URLSession:(NSURLSession*)session
      downloadTask:(NSURLSessionDownloadTask*)downloadTask
didFinishDownloadingToURL:(NSURL*)location {
  if (self.cancelled) {
    return;
  }
  NSError* error = nil;
  NSFileManager* manager = [NSFileManager defaultManager];
  [manager removeItemAtURL:self.partURL error:NULL];
  if (![manager moveItemAtURL:location toURL:self.partURL error:&error]) {
    [self reportError:error];
    return;
  }
  [self reportError:nil];
}

- (void)URLSession:(NSURLSession*)session
              task:(NSURLSessionTask*)task
didCompleteWithError:(NSError*)error {
  if (self.cancelled || error == nil) {
    return;
  }
  [self reportError:error];
}

- (void)reportError:(nullable NSError*)error {
  void (^finish)(NSError*) = self.onFinish;
  self.onFinish = nil;
  // 取消路径已经 invalidate 过，重复调用会抛异常。
  if (!self.cancelled && self.session != nil) {
    [self.session finishTasksAndInvalidate];
  }
  if (finish != nil) {
    finish(error);
  }
}

@end

namespace {

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

  bool download(const std::string& url, const std::filesystem::path& dest,
                const xiaotu::ProgressFn& on_progress, const xiaotu::CancelFn& cancel,
                std::string& err) override {
    @autoreleasepool {
      NSURL* nsURL = [NSURL URLWithString:@(url.c_str())];
      if (nsURL == nil) {
        err = "安装包地址无效";
        return false;
      }
      std::filesystem::path partPath = dest;
      partPath += ".part";
      NSURL* partURL = [NSURL fileURLWithPath:@(partPath.c_str())];
      NSURL* destURL = [NSURL fileURLWithPath:@(dest.c_str())];

      __block NSError* failure = nil;
      dispatch_semaphore_t done = dispatch_semaphore_create(0);
      XiaotuEngineDownload* download = [[XiaotuEngineDownload alloc] init];
      download.shouldCancel = ^bool { return cancel != nullptr && cancel(); };
      download.onProgress = ^(int64_t received, int64_t total) {
        if (on_progress != nullptr) {
          on_progress(received, total);
        }
      };
      download.onFinish = ^(NSError* error) {
        failure = error;
        dispatch_semaphore_signal(done);
      };
      [download startWithURL:nsURL destination:partURL];
      const long wait = dispatch_semaphore_wait(
          done, dispatch_time(DISPATCH_TIME_NOW,
                              static_cast<int64_t>((kDownloadResourceTimeout + 60) * NSEC_PER_SEC)));
      if (wait != 0 || download.cancelled || failure != nil) {
        if (!download.cancelled) {
          [download cancel];
        }
        [[NSFileManager defaultManager] removeItemAtURL:partURL error:NULL];
        if (wait != 0) {
          err = "下载超时";
        } else if (download.cancelled) {
          err = "CANCELED";
        } else {
          err = failure.localizedDescription.length > 0
                    ? std::string(failure.localizedDescription.UTF8String)
                    : std::string("下载失败");
        }
        return false;
      }
      [[NSFileManager defaultManager] removeItemAtURL:destURL error:NULL];
      NSError* moveError = nil;
      if (![[NSFileManager defaultManager] moveItemAtURL:partURL toURL:destURL error:&moveError]) {
        [[NSFileManager defaultManager] removeItemAtURL:partURL error:NULL];
        err = moveError.localizedDescription.length > 0
                  ? std::string(moveError.localizedDescription.UTF8String)
                  : std::string("无法保存安装包");
        return false;
      }
      return true;
    }
  }
};

#if !XIAOTU_HAS_SPARKLE
// 仅开发构建（未链接 Sparkle）保留「打开 dmg 让用户拖拽」的兜底安装；
// 发布构建由 CMake 强制 Sparkle，安装走旁路，不进引擎。
class MacInstaller : public xiaotu::IInstaller {
 public:
  xiaotu::InstallResult install(const xiaotu::PendingPackage& pkg) override {
    @autoreleasepool {
      NSString* path = @(pkg.path.c_str());
      if (![[NSFileManager defaultManager] fileExistsAtPath:path]) {
        return {false, "FAILED", "安装包文件不存在"};
      }
      if ([[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:path]]) {
        return {true, "STARTED", ""};
      }
      return {false, "FAILED", "无法打开安装包"};
    }
  }
};
#endif

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

bool jbool(const nlohmann::json& j, const char* key) {
  if (j.is_object() && j.contains(key) && j[key].is_boolean()) {
    return j[key].get<bool>();
  }
  return false;
}

// 引擎 last_result → mac 前端契约取值映射（前端只认交接单里的枚举）。
// 下载/安装过程码归一为 UPDATE_AVAILABLE（进度由 prefetch_state 表达）；
// 通道/平台不匹配按旧 mac 口径记 NO_UPDATE；传输与解析类错误记 CHECK_FAILED。
NSString* map_engine_last_result(NSString* raw, bool hasPending) {
  if (raw.length == 0) {
    return nil;
  }
  static NSDictionary* const kToNoUpdate = @{
    @"CHANNEL_MISMATCH" : kResultNoUpdate,
    @"PLATFORM_MISMATCH" : kResultNoUpdate,
  };
  static NSDictionary* const kToUpdateAvailable = @{
    @"DOWNLOAD_STARTED" : kResultUpdateAvailable,
    @"DOWNLOADED" : kResultUpdateAvailable,
    @"CANCELED" : kResultUpdateAvailable,
    @"VERIFY_FAILED" : kResultUpdateAvailable,
    @"INSTALL_FAILED" : kResultUpdateAvailable,
    @"UPDATE_REQUIRED" : kResultUpdateAvailable,
  };
  static NSSet* const kToCheckFailed = [NSSet setWithObjects:
      @"MANIFEST_JSON_INVALID", @"MANIFEST_SIGNATURE_INVALID", @"MANIFEST_INVALID",
      @"MANIFEST_URL_EMPTY", @"NO_TRANSPORT", @"VERSION_INVALID",
      @"MANIFEST_PLATFORM_MISSING", @"MANIFEST_ARTIFACTS_MISSING", nil];
  NSString* mapped = kToNoUpdate[raw] ?: kToUpdateAvailable[raw];
  if (mapped != nil) {
    return mapped;
  }
  if ([kToCheckFailed containsObject:raw]) {
    return kResultCheckFailed;
  }
  if ([raw isEqualToString:@"COOLDOWN"]) {
    return hasPending ? kResultUpdateAvailable : kResultNoUpdate;
  }
  return raw;  // NO_UPDATE / UPDATE_AVAILABLE / SKIPPED / NOT_IN_ROLLOUT /
               // OS_UNSUPPORTED / INCOMPATIBLE_* / CHECK_FAILED 等原样透传
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
  std::unique_ptr<MacHasher> _hasher;
  std::unique_ptr<MacClock> _clock;
#if !XIAOTU_HAS_SPARKLE
  std::unique_ptr<MacInstaller> _installer;
#endif
  std::unique_ptr<xiaotu::UpdateEngine> _engine;
}
@property(nonatomic, strong) dispatch_queue_t queue;
@property(nonatomic, copy, nullable) NSString* manifestURL;
@property(nonatomic, copy, nullable) NSString* healthBase;
@property(nonatomic, copy) NSString* channel;
@property(nonatomic, assign) BOOL running;
// 递增令牌用于作废旧定时器；只在 self.queue 上读写。
@property(nonatomic, assign) NSUInteger timerToken;
// 预下载 UX 门（壳侧）：展示态叠加、流量提示确认、自动重试与取消记录。
// 真实下载状态与进度以引擎为准，这里只保留前端契约需要的展示信息。
@property(nonatomic, copy) NSString* prefetchState;
@property(nonatomic, copy, nullable) NSString* prefetchMessage;
@property(nonatomic, copy, nullable) NSString* prefetchVersion;
@property(nonatomic, assign) NSInteger prefetchAttempts;
@property(nonatomic, assign) NSUInteger prefetchRetryToken;
// 引擎 download_pending() 是否正在后台线程执行。只在 self.queue 上读写。
@property(nonatomic, assign) BOOL downloadActive;
@property(nonatomic, assign) BOOL downloadManual;
// 用户取消过的版本：不再自动重试，手动下载仍可。
@property(nonatomic, copy, nullable) NSString* canceledVersion;
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
    // 两个开关默认开启：registerDefaults 只影响「从未写过」的键，用户改过就保留用户值。
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    [defaults registerDefaults:@{kAutoCheckKey : @YES, kPrefetchKey : @YES}];
    _prefetchState = kPrefetchIdle;

    _store.reset(new MacStore());
    _transport.reset(new MacTransport());
    _hasher.reset(new MacHasher());
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
    cfg.prefetch = prefetch_enabled();
    NSURL* directory = prefetch_directory();
    if (directory != nil) {
      cfg.download_dir = directory.path.UTF8String ?: "";
    }
    NSString* pubKey = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"SUPublicEDKey"];
    if (pubKey.length > 0) {
      cfg.public_key_base64 = pubKey.UTF8String;
    } else {
      xiaotu::log_warn("更新：Info.plist 未配置 SUPublicEDKey，跳过清单验签");
    }

    xiaotu::UpdateEngine::Deps deps;
    deps.transport = _transport.get();
    deps.hasher = _hasher.get();
    deps.clock = _clock.get();
    deps.store = _store.get();
#if !XIAOTU_HAS_SPARKLE
    _installer.reset(new MacInstaller());
    deps.installer = _installer.get();
#endif
    _engine.reset(new xiaotu::UpdateEngine(deps, cfg));
    // 恢复引擎状态：含旧散键迁移、崩溃恢复与「已装覆盖残留 pending」清理。
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
    // 轻量握手与本地 schema 只依赖 health，即使未配置清单也要保留。
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

- (void)noteLaunch {
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  NSInteger attempts = [defaults integerForKey:kLaunchAttemptsKey] + 1;
  [defaults setInteger:attempts forKey:kLaunchAttemptsKey];
  NSString* validatingVersion = [defaults stringForKey:kValidatingVersionKey];
  // 引擎里的待更新版本（已被本机覆盖的残留会在 load() 清掉）。
  const std::string pendingVer = jstr([self engineStatus], "version");
  // 两种情况都算“更新后不稳定”：新版本仍在验证期，或更高版本已下载但尚未装上。
  // v2 的 macOS 没有构建号，这里按版本号判定（与清单的更新判定同一口径）。
  if (xiaotu::should_enter_recovery_version(
          static_cast<int>(attempts), kMaxLaunchFailures,
          validatingVersion.UTF8String ?: "", pendingVer,
          xiaotu::current_product_version())) {
    [defaults setBool:YES forKey:kPausedKey];
    [defaults setBool:YES forKey:kRecoveryRequiredKey];
    xiaotu::log_error("更新：连续 " + std::to_string(attempts) +
                      " 次启动未进入稳定状态，已进入恢复模式，可回退上一版本");
  }
  __weak XiaotuUpdater* weakSelf = self;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC), self.queue, ^{
    XiaotuUpdater* strongSelf = weakSelf;
    if (strongSelf == nil) {
      return;
    }
    NSUserDefaults* currentDefaults = [NSUserDefaults standardUserDefaults];
    [currentDefaults setInteger:0 forKey:kLaunchAttemptsKey];
    // 恢复模式保持暂停：等用户明确选择回退或继续使用当前版本，避免自动更新又装上问题版本。
    const BOOL recovery = [currentDefaults boolForKey:kRecoveryRequiredKey];
    if (!recovery) {
      [currentDefaults setBool:NO forKey:kPausedKey];
    }
    // 新版本连续稳定运行 3 次后，安装前保留的旧副本就可以删了。
    // 恢复模式期间保留副本（用户可能还要回退）；旧版本自身启动时 validating 不等于当前版本，不会误删。
    NSString* validating = [currentDefaults stringForKey:kValidatingVersionKey];
    if (!recovery &&
        xiaotu::is_same_product_version(validating.UTF8String ?: "",
                                        xiaotu::current_product_version())) {
      NSInteger stable = [currentDefaults integerForKey:kStableLaunchKey] + 1;
      if (stable >= kStableLaunchesBeforeCleanup) {
        [strongSelf removePreviousBundle];
        [currentDefaults removeObjectForKey:kValidatingVersionKey];
        stable = 0;
        xiaotu::log_info("更新：新版本已稳定启动 " +
                         std::to_string(kStableLaunchesBeforeCleanup) + " 次，已删除旧版本副本");
      }
      [currentDefaults setInteger:stable forKey:kStableLaunchKey];
    }
    // 稳定运行后若此前因暂停没有武装定时器，这里补上。
    [strongSelf armTimerOnQueue];
  });
}

// 删除旧版本副本与元数据；保留回退脚本与日志便于排查。
- (void)removePreviousBundle {
  NSURL* previous = previous_app_url();
  if (previous != nil) {
    [[NSFileManager defaultManager] removeItemAtURL:previous error:NULL];
  }
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  [defaults removeObjectForKey:kPreviousVersionKey];
  [defaults removeObjectForKey:kPreviousBuildKey];
}

- (BOOL)previousBundleAvailable {
  NSURL* previous = previous_app_url();
  return previous != nil && [[NSFileManager defaultManager] fileExistsAtPath:previous.path];
}

// 安装前保留当前 .app：只留最近 1 份，并记下目标版本进入验证期。
- (BOOL)preparePreviousBundleWithTargetVersion:(NSString*)targetVersion
                                         error:(NSString**)errorOut {
  NSURL* bundle = [[NSBundle mainBundle] bundleURL];
  NSURL* previous = previous_app_url();
  if (bundle == nil || previous == nil) {
    if (errorOut != nullptr) {
      *errorOut = @"无法定位应用包或缓存目录";
    }
    return NO;
  }
  NSFileManager* manager = [NSFileManager defaultManager];
  [manager removeItemAtURL:previous error:NULL];
  NSError* error = nil;
  if (![manager copyItemAtURL:bundle toURL:previous error:&error]) {
    if (errorOut != nullptr) {
      *errorOut = error.localizedDescription ?: @"复制应用包失败";
    }
    return NO;
  }
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  NSString* version =
      [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
  [defaults setObject:version ?: @"" forKey:kPreviousVersionKey];
  [defaults setInteger:current_build() forKey:kPreviousBuildKey];
  [defaults setObject:targetVersion ?: @"" forKey:kValidatingVersionKey];
  [defaults setInteger:0 forKey:kStableLaunchKey];
  xiaotu::log_info("更新：已保留当前版本副本用于回退，目标版本 " +
                   std::string(targetVersion.UTF8String ?: ""));
  return YES;
}

// 写回退脚本：等当前进程退出后，用旧副本替换 .app 并重新启动。
// 失败时回滚现场并重新打开当前版本，避免用户既回不去也打不开。
- (nullable NSString*)writeRollbackScriptWithError:(NSString**)errorOut {
  NSURL* bundle = [[NSBundle mainBundle] bundleURL];
  NSURL* previous = previous_app_url();
  NSURL* directory = previous_directory();
  if (bundle == nil || previous == nil || directory == nil) {
    if (errorOut != nullptr) {
      *errorOut = @"无法定位应用包或缓存目录";
    }
    return nil;
  }
  NSURL* scriptURL = [directory URLByAppendingPathComponent:@"rollback.sh"];
  NSURL* logURL = [directory URLByAppendingPathComponent:@"rollback.log"];
  NSString* content = [NSString
      stringWithFormat:@"#!/bin/sh\n"
                       "CURRENT=%@\n"
                       "PREVIOUS=%@\n"
                       "LOG=%@\n"
                       "PID=%d\n"
                       "log() { printf '%%s %%s\\n' \"$(date '+%%Y-%%m-%%d %%H:%%M:%%S')\" "
                       "\"$1\" >> \"$LOG\"; }\n"
                       "log '等待当前进程退出'\n"
                       "i=0\n"
                       "while kill -0 \"$PID\" 2>/dev/null; do\n"
                       "  i=$((i+1))\n"
                       "  if [ \"$i\" -gt 240 ]; then log '等待超时，放弃回退'; "
                       "/usr/bin/open \"$CURRENT\"; exit 1; fi\n"
                       "  sleep 0.5\n"
                       "done\n"
                       "TMP=\"${CURRENT}.rollback-backup\"\n"
                       "rm -rf \"$TMP\"\n"
                       "if mv \"$CURRENT\" \"$TMP\"; then\n"
                       "  if /usr/bin/ditto \"$PREVIOUS\" \"$CURRENT\"; then\n"
                       "    rm -rf \"$TMP\" \"$PREVIOUS\"\n"
                       "    log '回退完成，重新启动旧版本'\n"
                       "    /usr/bin/open \"$CURRENT\"\n"
                       "    exit 0\n"
                       "  fi\n"
                       "  log '复制旧版本失败，恢复当前版本'\n"
                       "  rm -rf \"$CURRENT\"\n"
                       "  mv \"$TMP\" \"$CURRENT\"\n"
                       "else\n"
                       "  log '移动当前版本失败，可能没有写入权限'\n"
                       "fi\n"
                       "/usr/bin/open \"$CURRENT\"\n"
                       "exit 1\n",
                       shell_quote(bundle.path), shell_quote(previous.path),
                       shell_quote(logURL.path), static_cast<int>(getpid())];
  NSError* error = nil;
  if (![content writeToURL:scriptURL atomically:YES encoding:NSUTF8StringEncoding error:&error]) {
    if (errorOut != nullptr) {
      *errorOut = error.localizedDescription ?: @"写入回退脚本失败";
    }
    return nil;
  }
  [[NSFileManager defaultManager] setAttributes:@{NSFilePosixPermissions : @(0700)}
                                   ofItemAtPath:scriptURL.path
                                          error:NULL];
  return scriptURL.path;
}

- (NSDictionary<NSString*, id>*)previousBundleStatus {
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  NSString* version = [defaults stringForKey:kPreviousVersionKey];
  return @{
    @"available" : @([self previousBundleAvailable]),
    @"version" : version.length > 0 ? version : (id)[NSNull null],
    @"build" : @([defaults integerForKey:kPreviousBuildKey]),
  };
}

- (void)rollbackUpdateWithCompletion:(void (^)(NSDictionary<NSString*, id>* result))completion {
  void (^respond)(NSDictionary*) = ^(NSDictionary* result) {
    dispatch_async(dispatch_get_main_queue(), ^{
      completion(result);
    });
  };
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  if (![defaults boolForKey:kRecoveryRequiredKey]) {
    respond(@{
      @"ok" : @NO,
      @"code" : @"NOT_RECOVERY",
      @"message" : @"当前不在恢复模式",
    });
    return;
  }
  if (![self previousBundleAvailable]) {
    respond(@{
      @"ok" : @NO,
      @"code" : @"NO_PREVIOUS",
      @"message" : @"没有可回退的旧版本副本，请从官网重新下载上一版本",
    });
    return;
  }
  NSString* scriptError = nil;
  NSString* scriptPath = [self writeRollbackScriptWithError:&scriptError];
  if (scriptPath == nil) {
    respond(@{
      @"ok" : @NO,
      @"code" : @"FAILED",
      @"message" : scriptError ?: @"无法准备回退脚本",
    });
    return;
  }
  NSTask* task = [[NSTask alloc] init];
  task.executableURL = [NSURL fileURLWithPath:@"/bin/sh"];
  task.arguments = @[ scriptPath ];
  NSError* launchError = nil;
  if (![task launchAndReturnError:&launchError]) {
    respond(@{
      @"ok" : @NO,
      @"code" : @"FAILED",
      @"message" : launchError.localizedDescription ?: @"无法启动回退脚本",
    });
    return;
  }
  // 旧版本启动后不再提示恢复；失败计数与验证期一并清掉。
  [defaults setBool:NO forKey:kRecoveryRequiredKey];
  [defaults setInteger:0 forKey:kLaunchAttemptsKey];
  [defaults setInteger:0 forKey:kStableLaunchKey];
  [defaults setBool:NO forKey:kPausedKey];
  [defaults removeObjectForKey:kValidatingVersionKey];
  respond(@{
    @"ok" : @YES,
    @"code" : @"ROLLING_BACK",
    @"message" : @"正在退出并回退到上一版本，请稍候…",
  });
  // 用 stop 而不是 terminate：回退已经过用户显式确认，跳过未保存内容的拦截，
  // 让 run loop 正常退出后由主流程关闭服务，脚本再等进程结束后替换 .app。
  dispatch_async(dispatch_get_main_queue(), ^{
    [NSApp stop:nil];
    NSEvent* wake = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                      location:NSZeroPoint
                                 modifierFlags:0
                                     timestamp:0
                                  windowNumber:0
                                       context:nil
                                       subtype:0
                                         data1:0
                                         data2:0];
    [NSApp postEvent:wake atStart:YES];
  });
}

- (void)openBackupFolderWithCompletion:(void (^)(NSDictionary<NSString*, id>* result))completion {
  void (^respond)(NSDictionary*) = ^(NSDictionary* result) {
    dispatch_async(dispatch_get_main_queue(), ^{
      completion(result);
    });
  };
  NSURL* directory = backup_directory();
  if (directory == nil ||
      ![[NSFileManager defaultManager] fileExistsAtPath:directory.path]) {
    respond(@{
      @"ok" : @NO,
      @"code" : @"NOT_FOUND",
      @"message" : @"尚未生成备份目录",
    });
    return;
  }
  if ([[NSWorkspace sharedWorkspace] openURL:directory]) {
    respond(@{@"ok" : @YES, @"code" : @"OPENED"});
  } else {
    respond(@{
      @"ok" : @NO,
      @"code" : @"FAILED",
      @"message" : @"无法打开备份目录",
    });
  }
}

// ---- 预下载 UX 门（壳侧；真实下载状态与进度在引擎） ----

// 作废展示态并清理引擎包之外的缓存残留；只在 self.queue 上调用。
- (void)resetPrefetchGate {
  self.prefetchRetryToken += 1;
  self.prefetchState = kPrefetchIdle;
  self.prefetchMessage = nil;
  self.prefetchVersion = nil;
  self.prefetchAttempts = 0;
  self.canceledVersion = nil;
  self.downloadManual = NO;
}

// 清理更新缓存目录：只保留引擎当前 pending 的包，其余（旧版本/半截文件）删除。
- (void)removePrefetchFilesExceptPending {
  NSURL* directory = prefetch_directory();
  if (directory == nil) {
    return;
  }
  const std::string keep = jstr([self engineStatus], "pending_path");
  NSString* keepPath = keep.empty() ? nil : @(keep.c_str());
  NSFileManager* manager = [NSFileManager defaultManager];
  NSArray<NSURL*>* files = [manager contentsOfDirectoryAtURL:directory
                                  includingPropertiesForKeys:nil
                                                     options:NSDirectoryEnumerationSkipsHiddenFiles
                                                       error:NULL];
  for (NSURL* file in files) {
    if (keepPath != nil && [file.path isEqualToString:keepPath]) {
      continue;
    }
    [manager removeItemAtURL:file error:NULL];
  }
}

// 自动检查命中新版本后调用：按开关、流量提示确认与取消记录决定是否自动开始。
// autoStart=NO 用于手动检查：只准备展示态，不自动下载。只在 self.queue 上调用。
- (void)evaluatePrefetchGateAutoStart:(BOOL)autoStart {
#if XIAOTU_HAS_SPARKLE
  // 链接 Sparkle 时，下载、验签与替换都由安装流程统一完成；
  // 自研预下载只会造成重复流量，Sparkle 构建不再预热。
  (void)autoStart;
  if (self.downloadActive) {
    _engine->cancel_download();
  }
  [self resetPrefetchGate];
  return;
#endif
  nlohmann::json s = [self engineStatus];
  const std::string version = jstr(s, "version");
  const std::string state = jstr(s, "state");
  if (version.empty()) {
    [self resetPrefetchGate];
    return;
  }
  NSString* versionText = @(version.c_str());
  if (![self.prefetchVersion isEqualToString:versionText]) {
    [self resetPrefetchGate];
    self.prefetchVersion = versionText;
    [self removePrefetchFilesExceptPending];
  }
  if (state == "DONE") {
    self.prefetchState = kPrefetchIdle;
    return;
  }
  if (state == "DOWNLOADING" || self.downloadActive) {
    return;  // 下载进行中，等完成回调更新展示态
  }
  if (!prefetch_enabled()) {
    self.prefetchState = kPrefetchIdle;
    self.prefetchMessage = nil;
    return;
  }
  if (!autoStart) {
    if (!self.downloadActive &&
        ![self.prefetchState isEqualToString:kPrefetchNeedsConsent]) {
      self.prefetchState = kPrefetchIdle;
    }
    return;
  }
  if (![[NSUserDefaults standardUserDefaults] boolForKey:kPrefetchNoticeKey]) {
    self.prefetchState = kPrefetchNeedsConsent;
    self.prefetchMessage = @"首次后台下载前需要确认流量提示";
    return;
  }
  if ([self.canceledVersion isEqualToString:versionText]) {
    return;  // 取消过的版本不再自动重试；手动下载仍可。
  }
  if (self.prefetchAttempts >= kMaxPrefetchAttempts) {
    return;  // 展示为 failed（引擎 FAILED 态），不再自动重试。
  }
  [self startEngineDownloadManual:NO];
}

// 真正发起下载：引擎 download_pending() 在后台线程执行，完成回调回 self.queue。
// manual=YES 表示用户显式点了「下载并校验」，不受取消记录与开关限制。只在 self.queue 上调用。
- (void)startEngineDownloadManual:(BOOL)manual {
  if (self.downloadActive) {
    return;
  }
  const std::string version = jstr([self engineStatus], "version");
  if (version.empty()) {
    return;
  }
  self.downloadActive = YES;
  self.downloadManual = manual;
  if (manual) {
    self.prefetchAttempts = 0;
    self.prefetchRetryToken += 1;
    self.canceledVersion = nil;
    // 手动下载是显式动作，视为已知悉流量提示，不再单独弹一次性提示。
    [[NSUserDefaults standardUserDefaults] setBool:YES forKey:kPrefetchNoticeKey];
  }
  self.prefetchVersion = @(version.c_str());
  self.prefetchState = kPrefetchRunning;
  self.prefetchMessage = @"正在后台下载安装包";
  dispatch_queue_t queue = self.queue;
  __weak XiaotuUpdater* weakSelf = self;
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
    XiaotuUpdater* strongSelf = weakSelf;
    if (strongSelf == nil) {
      return;
    }
    const bool ok = strongSelf->_engine->download_pending();
    dispatch_async(queue, ^{
      XiaotuUpdater* inner = weakSelf;
      if (inner == nil) {
        return;
      }
      [inner handleEngineDownloadFinished:ok];
    });
  });
}

// 下载结束：更新展示态，自动失败按 1/10 分钟退避重试（最多 3 次）。只在 self.queue 上调用。
- (void)handleEngineDownloadFinished:(BOOL)ok {
  self.downloadActive = NO;
  const BOOL manual = self.downloadManual;
  self.downloadManual = NO;
  nlohmann::json s = [self engineStatus];
  const std::string version = jstr(s, "version");
  if (version.empty()) {
    // 下载期间版本被跳过/覆盖：展示态作废即可。
    [self resetPrefetchGate];
    return;
  }
  const std::string lastResult = jstr(s, "last_result");
  const std::string lastError = jstr(s, "last_error");
  NSString* versionText = @(version.c_str());
  if (ok) {
    self.prefetchAttempts = 0;
    self.prefetchState = kPrefetchIdle;
    self.prefetchMessage = @"下载并校验通过";
    xiaotu::log_info("更新：后台预下载完成 " + version);
    return;
  }
  if (lastResult == "CANCELED") {
    self.prefetchState = kPrefetchIdle;
    self.prefetchMessage = @"已取消本次后台下载";
    return;
  }
  self.prefetchAttempts += 1;
  NSString* message = [NSString stringWithFormat:@"下载失败：%@",
                                                 @(lastError.c_str())];
  xiaotu::log_warn("更新：后台预下载失败（第 " + std::to_string(self.prefetchAttempts) +
                   " 次）：" + (message.UTF8String ?: ""));
  // 手动下载失败直接结束；自动预下载最多 3 次（首次 + 两次退避）。
  const BOOL retry = !manual && prefetch_enabled() &&
                     self.prefetchAttempts < kMaxPrefetchAttempts;
  if (!retry) {
    self.prefetchState = kPrefetchIdle;
    self.prefetchMessage = message;
    return;
  }
  const NSTimeInterval delay =
      self.prefetchAttempts == 1 ? kPrefetchRetryFirstDelay : kPrefetchRetrySecondDelay;
  self.prefetchState = kPrefetchRetrying;
  self.prefetchMessage = [NSString stringWithFormat:@"%@，稍后自动重试（第 %ld/%d 次）",
                                                     message,
                                                     static_cast<long>(self.prefetchAttempts) + 1,
                                                     kMaxPrefetchAttempts];
  self.prefetchRetryToken += 1;
  const NSUInteger token = self.prefetchRetryToken;
  dispatch_queue_t queue = self.queue;
  __weak XiaotuUpdater* weakSelf = self;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(delay * NSEC_PER_SEC)),
                 queue, ^{
                   XiaotuUpdater* strongSelf = weakSelf;
                   if (strongSelf == nil || token != strongSelf.prefetchRetryToken) {
                     return;
                   }
                   if (!prefetch_enabled() || strongSelf.downloadActive) {
                     return;
                   }
                   if ([strongSelf.canceledVersion isEqualToString:versionText]) {
                     return;
                   }
                   if (![strongSelf.prefetchVersion isEqualToString:versionText]) {
                     return;
                   }
                   [strongSelf startEngineDownloadManual:NO];
                 });
}

// 开关与「首次流量提示」确认：auto_check / prefetch / ack_prefetch_notice 都是可选布尔。
- (void)setOptions:(id)options
        completion:(void (^)(NSDictionary<NSString*, id>* result))completion {
  void (^respond)(NSDictionary*) = ^(NSDictionary* result) {
    dispatch_async(dispatch_get_main_queue(), ^{
      completion(result);
    });
  };
  NSDictionary* body = [options isKindOfClass:[NSDictionary class]] ? options : @{};
  id autoValue = body[@"auto_check"];
  id prefetchValue = body[@"prefetch"];
  id ackValue = body[@"ack_prefetch_notice"];
  const BOOL hasAuto = autoValue != nil;
  const BOOL hasPrefetch = prefetchValue != nil;
  const BOOL ackNotice = [ackValue isKindOfClass:[NSNumber class]] && [ackValue boolValue];
  if ((hasAuto && ![autoValue isKindOfClass:[NSNumber class]]) ||
      (hasPrefetch && ![prefetchValue isKindOfClass:[NSNumber class]]) ||
      (ackValue != nil && ![ackValue isKindOfClass:[NSNumber class]])) {
    respond(@{
      @"ok" : @NO,
      @"code" : @"INVALID_ARGUMENT",
      @"status" : [self status],
    });
    return;
  }
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  if (hasAuto) {
    [defaults setBool:[autoValue boolValue] forKey:kAutoCheckKey];
  }
  if (hasPrefetch) {
    [defaults setBool:[prefetchValue boolValue] forKey:kPrefetchKey];
  }
  if (ackNotice) {
    [defaults setBool:YES forKey:kPrefetchNoticeKey];
  }
  _engine->set_options(true, auto_check_enabled(), prefetch_enabled());
  dispatch_async(self.queue, ^{
    if (hasAuto) {
      if (auto_check_enabled()) {
        [self checkIfDue];
      } else {
        // 递增令牌作废已武装的定时器；手动检查不受影响。
        self.timerToken += 1;
      }
    }
    if (hasPrefetch || ackNotice) {
      if (!prefetch_enabled() && !ackNotice) {
        if (self.downloadActive) {
          _engine->cancel_download();
        }
        self.prefetchRetryToken += 1;
        self.prefetchState = kPrefetchIdle;
        self.prefetchMessage = nil;
      } else if ([defaults boolForKey:kPrefetchNoticeKey]) {
        [self evaluatePrefetchGateAutoStart:YES];
      }
    }
    respond(@{
      @"ok" : @YES,
      @"code" : @"UPDATED",
      @"status" : [self status],
    });
  });
}

// 手动下载：开关关闭或同版本已取消都可以由用户显式重新发起。
- (void)downloadUpdateWithCompletion:(void (^)(NSDictionary<NSString*, id>* result))completion {
  void (^respond)(NSDictionary*) = ^(NSDictionary* result) {
    dispatch_async(dispatch_get_main_queue(), ^{
      completion(result);
    });
  };
#if XIAOTU_HAS_SPARKLE
  // Sparkle 构建下「下载」不再是独立步骤：交给 Sparkle 下载、验签并安装，
  // 避免自研预下载与 Sparkle 各下一遍。
  dispatch_async(dispatch_get_main_queue(), ^{
    [[[self sparkleController] updater] checkForUpdates];
  });
  respond(@{@"ok" : @YES, @"code" : @"STARTED", @"status" : [self status]});
  return;
#endif
  dispatch_async(self.queue, ^{
    nlohmann::json s = [self engineStatus];
    const std::string version = jstr(s, "version");
    const std::string state = jstr(s, "state");
    if (version.empty()) {
      respond(@{
        @"ok" : @NO,
        @"code" : @"NO_PENDING_UPDATE",
        @"status" : [self status],
      });
      return;
    }
    if (state == "DOWNLOADING" || self.downloadActive) {
      respond(@{
        @"ok" : @YES,
        @"code" : @"RUNNING",
        @"status" : [self status],
      });
      return;
    }
    if (state == "DONE") {
      respond(@{
        @"ok" : @YES,
        @"code" : @"DONE",
        @"status" : [self status],
      });
      return;
    }
    [self startEngineDownloadManual:YES];
    const BOOL started = self.downloadActive;
    respond(@{
      @"ok" : @(started),
      @"code" : started ? @"STARTED" : @"FAILED",
      @"status" : [self status],
    });
  });
}

- (void)cancelPrefetchWithCompletion:(void (^)(NSDictionary<NSString*, id>* result))completion {
  void (^respond)(NSDictionary*) = ^(NSDictionary* result) {
    dispatch_async(dispatch_get_main_queue(), ^{
      completion(result);
    });
  };
  dispatch_async(self.queue, ^{
    nlohmann::json s = [self engineStatus];
    const BOOL active =
        self.downloadActive || jstr(s, "state") == "DOWNLOADING";
    self.prefetchRetryToken += 1;
    if (active) {
      // 取消后本次版本不再自动重试，但不清版本记录：手动下载仍可。
      self.canceledVersion = @(jstr(s, "version").c_str());
      self.prefetchMessage = @"已取消本次后台下载";
      _engine->cancel_download();
    }
    respond(@{
      @"ok" : @YES,
      @"code" : active ? @"CANCELED" : @"NOT_RUNNING",
      @"status" : [self status],
    });
  });
}

- (void)skipVersion:(id)version
               days:(id)days
         completion:(void (^)(NSDictionary<NSString*, id>* result))completion {
  void (^respond)(NSDictionary*) = ^(NSDictionary* result) {
    dispatch_async(dispatch_get_main_queue(), ^{
      completion(result);
    });
  };
  NSDictionary* invalid = @{
    @"ok" : @NO,
    @"code" : @"INVALID_ARGUMENT",
    @"status" : [self status],
  };
  if (![version isKindOfClass:[NSString class]]) {
    respond(invalid);
    return;
  }
  NSString* text = [(NSString*)version
      stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
  NSInteger dayCount = 7;
  if (days != nil) {
    if (![days isKindOfClass:[NSNumber class]]) {
      respond(invalid);
      return;
    }
    const double raw = [(NSNumber*)days doubleValue];
    const NSInteger rounded = static_cast<NSInteger>(raw);
    if (raw != static_cast<double>(rounded)) {
      respond(invalid);
      return;
    }
    dayCount = rounded;
  }
  if (text.length == 0 || !xiaotu::valid_skip_days(static_cast<int>(dayCount))) {
    respond(invalid);
    return;
  }

  dispatch_async(self.queue, ^{
    nlohmann::json s = [self engineStatus];
    if (jbool(s, "mandatory")) {
      const std::string pendingVer = jstr(s, "version");
      if (pendingVer.empty() || pendingVer == text.UTF8String) {
        respond(@{
          @"ok" : @NO,
          @"code" : @"MANDATORY_UPDATE",
          @"status" : [self status],
        });
        return;
      }
    }
    if (self.downloadActive) {
      _engine->cancel_download();  // 跳过前中断下载，防止完成后复活 pending
    }
    if (!_engine->skip_version(text.UTF8String, static_cast<int>(dayCount))) {
      respond(invalid);
      return;
    }
    [self resetPrefetchGate];
    respond(@{
      @"ok" : @YES,
      @"code" : @"SKIPPED",
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
  // 安装前保留当前版本副本，供新版本连续启动失败时一键回退；失败不阻断安装。
  NSString* backupError = nil;
  if (![self preparePreviousBundleWithTargetVersion:@(pendingVer.c_str()) error:&backupError]) {
    xiaotu::log_warn("更新：未能保留旧版本副本，回退将不可用：" +
                     std::string(backupError.UTF8String ?: "未知原因"));
  }
  // 脚本消息在主线程派发；控制器也只在主线程创建。
  // Sparkle 负责下载、appcast EdDSA 验签、原子替换 .app 并自动重启；
  // 自研层只做「有没有新版本 / 灰度 / 强制」判定，不再打开 dmg 让用户拖拽。
  dispatch_async(dispatch_get_main_queue(), ^{
    [[[self sparkleController] updater] checkForUpdates];
  });
  respond(@{@"ok" : @YES, @"code" : @"STARTED"});
#else
  dispatch_async(self.queue, ^{
    const xiaotu::InstallResult res = _engine->try_install();
    if (res.started) {
      respond(@{
        @"ok" : @YES,
        @"code" : @"OPENED",
        @"message" : @"已打开安装包，请退出本程序后按提示完成安装。",
      });
      return;
    }
    NSString* code = @"FAILED";
    if (res.code == "UNAVAILABLE") {
      code = @"UNAVAILABLE";
    } else if (res.code == "NO_PENDING_UPDATE") {
      code = @"NO_PENDING_UPDATE";
    }
    respond(@{
      @"ok" : @NO,
      @"code" : code,
      @"message" : res.error.empty()
                       ? @"当前构建没有可用的安装包，请手动下载安装"
                       : @(res.error.c_str()),
    });
  });
#endif
}

- (void)resumeWithCompletion:(void (^)(NSDictionary<NSString*, id>* result))completion {
  void (^respond)(NSDictionary*) = ^(NSDictionary* result) {
    dispatch_async(dispatch_get_main_queue(), ^{
      completion(result);
    });
  };
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  const BOOL paused = [defaults boolForKey:kPausedKey];
  // 无论是否处于暂停，都清掉失败计数与恢复标记，避免恢复后因旧状态立刻再次暂停。
  [defaults setBool:NO forKey:kPausedKey];
  [defaults setInteger:0 forKey:kLaunchAttemptsKey];
  [defaults setBool:NO forKey:kRecoveryRequiredKey];

  if (xiaotu::decide_resume(paused) == xiaotu::ResumeDecision::kNotPaused) {
    respond(@{
      @"ok" : @YES,
      @"code" : @"NOT_PAUSED",
      @"status" : [self status],
    });
    return;
  }

  // 立即做一次自动检查，并重新安排下一周期；未配置清单或自动检查关闭时只清除暂停标记。
  _engine->invalidate_schedule();
  if (self.running && self.manifestURL.length > 0 && auto_check_enabled()) {
    dispatch_async(self.queue, ^{
      [self checkIfDue];
    });
  }
  xiaotu::log_info("更新：已恢复自动更新");
  respond(@{
    @"ok" : @YES,
    @"code" : @"RESUMED",
    @"status" : [self status],
  });
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
  if ([[NSUserDefaults standardUserDefaults] boolForKey:kPausedKey]) {
    respond(@{
      @"ok" : @NO,
      @"code" : kResultPaused,
      @"update_available" : @NO,
      @"status" : [self status],
    });
    return;
  }
  dispatch_async(self.queue, ^{
    const bool judged = _engine->check(true);
    nlohmann::json s = [self engineStatus];
    const std::string rawResult = jstr(s, "last_result");
    const bool hasPending = !jstr(s, "version").empty();
    if (!judged && rawResult == "COOLDOWN") {
      respond(@{
        @"ok" : @NO,
        @"code" : @"COOLDOWN",
        @"retry_after" : @(ji64(s, "retry_after")),
        @"update_available" : @NO,
        @"status" : [self status],
      });
      return;
    }
    if (judged) {
      [self recordHealthMinSupported];
      [self evaluatePrefetchGateAutoStart:NO];  // 手动检查只准备状态，不自动下载
    }
    [self armTimerOnQueue];
    NSString* lastResult = map_engine_last_result(
        rawResult.empty() ? nil : @(rawResult.c_str()), hasPending);
    const BOOL ok = judged && ![lastResult isEqualToString:kResultCheckFailed];
    respond(@{
      @"ok" : @(ok),
      @"code" : ok ? @"CHECKED" : kResultCheckFailed,
      @"last_result" : lastResult ?: [NSNull null],
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
    [[NSUserDefaults standardUserDefaults] setObject:minimum forKey:kMinSupportedKey];
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

// 检查成功后顺带记录服务端要求的最低客户端版本（只记录与告警，
// 启动时的阻塞页由 webview 在加载前单独检查）。只在 self.queue 上调用。
- (void)recordHealthMinSupported {
  NSString* base = self.healthBase;
  if (base.length == 0) {
    return;
  }
  NSString* healthError = nil;
  NSDictionary* health = fetch_json([NSURL URLWithString:[base stringByAppendingString:@"/api/health"]],
                                    &healthError);
  NSString* minimum = json_string(health, @"min_supported_version");
  if (minimum.length == 0) {
    return;
  }
  [[NSUserDefaults standardUserDefaults] setObject:minimum forKey:kMinSupportedKey];
  xiaotu::ProductVersion currentVersion{};
  xiaotu::ProductVersion requiredVersion{};
  if (xiaotu::parse_product_version(xiaotu::current_product_version(), currentVersion) &&
      xiaotu::parse_product_version(minimum.UTF8String, requiredVersion) &&
      xiaotu::compare_product_version(currentVersion, requiredVersion) < 0) {
    xiaotu::log_warn("更新：当前版本 " + std::string(xiaotu::current_product_version()) +
                     " 低于服务端最低支持版本 " + std::string(minimum.UTF8String) +
                     "，应阻断业务入口并提示升级");
  }
}

- (NSDictionary<NSString*, id>*)status {
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  // P3：以引擎统一 status_json 为底（方案 §2.4），壳只补 mac 扩展键；
  // 旧键名重映射（pending_version / prefetch_bytes / next_check_at 等）已删除，
  // 前端 normalizeUpdateStatus 负责归一成内部形状。
  nlohmann::json s = [self engineStatus];
  NSMutableDictionary<NSString*, id>* out = [NSMutableDictionary dictionary];
  const std::string text = s.dump();
  NSData* data = [NSData dataWithBytes:text.data() length:text.size()];
  id base = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
  if ([base isKindOfClass:[NSDictionary class]]) {
    [out addEntriesFromDictionary:base];
  }
  const std::string engineState = jstr(s, "state");
  const std::string rawResult = jstr(s, "last_result");
  // enabled 维持旧语义：配置了清单地址即启用（引擎的 enabled 是总开关位）。
  out[@"enabled"] = @(self.manifestURL.length > 0);
  out[@"channel"] = self.channel ?: @"stable";
#if XIAOTU_HAS_SPARKLE
  // 安装引擎：sparkle 表示下载与替换都由 Sparkle 完成，前端隐藏自研下载入口。
  out[@"install_engine"] = @"sparkle";
#else
  out[@"install_engine"] = @"package";
#endif
  out[@"prefetch_notice_acked"] = @([defaults boolForKey:kPrefetchNoticeKey]);
  // prefetch_state：引擎状态为主，叠加壳侧 needs_consent / retrying 门控细节。
  NSString* prefetchState;
  if (engineState == "DOWNLOADING") {
    prefetchState = kPrefetchRunning;
  } else if (engineState == "DONE") {
    prefetchState = kPrefetchDone;
  } else if (engineState == "FAILED") {
    if ([self.prefetchState isEqualToString:kPrefetchRetrying]) {
      prefetchState = kPrefetchRetrying;
    } else {
      prefetchState = rawResult == "CANCELED" ? kPrefetchCanceled : kPrefetchFailed;
    }
  } else {
    prefetchState = [self.prefetchState isEqualToString:kPrefetchNeedsConsent]
                        ? kPrefetchNeedsConsent
                        : kPrefetchIdle;
  }
  out[@"prefetch_state"] = prefetchState;
  out[@"prefetch_version"] = self.prefetchVersion ?: [NSNull null];
  out[@"prefetch_attempts"] = @(self.prefetchAttempts);
  out[@"prefetch_message"] =
      self.prefetchMessage.length > 0 ? self.prefetchMessage : [NSNull null];
  NSString* intermediateVersion = [defaults stringForKey:kIntermediateVersionKey];
  id intermediateValue = [NSNull null];
  if (intermediateVersion.length > 0) {
    intermediateValue = @{
      @"version" : intermediateVersion,
      @"build" : @([defaults integerForKey:kIntermediateBuildKey]),
      @"schema" : @([defaults integerForKey:kIntermediateSchemaKey]),
    };
  }
  out[@"min_supported_version"] = [defaults stringForKey:kMinSupportedKey] ?: [NSNull null];
  out[@"launch_attempts"] = @([defaults integerForKey:kLaunchAttemptsKey]);
  out[@"paused"] = @([defaults boolForKey:kPausedKey]);
  // 恢复模式：连续启动失败后引导回退；previous 为安装前保留的旧版本副本。
  out[@"recovery_required"] = @([defaults boolForKey:kRecoveryRequiredKey]);
  out[@"previous"] = [self previousBundleStatus];
  // 本地 schema 低于 schema_min 时由清单 upgrade_path 选出的中间版本；其余情况为 null。
  out[@"intermediate"] = intermediateValue;
  return out;
}

// 按引擎 next_check_epoch 武装一次性定时器；必须在 self.queue 上调用。
// 暂停或未配置时直接返回，不在后台空转；恢复时由 resume / 稳定运行逻辑重新武装。
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
  if ([[NSUserDefaults standardUserDefaults] boolForKey:kPausedKey]) {
    return;
  }
  nlohmann::json s = [self engineStatus];
  double delay = static_cast<double>(ji64(s, "next_check_epoch")) - now_epoch();
  const std::string state = jstr(s, "state");
  if (delay <= 0 && (state == "DOWNLOADING" || state == "INSTALLING")) {
    delay = 30;  // 下载/安装期间不空转，稍后再看
  }
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
// 必须在 self.queue 上调用。
- (void)checkIfDue {
  if (!self.running || self.manifestURL.length == 0 || !auto_check_enabled()) {
    return;
  }
  if ([[NSUserDefaults standardUserDefaults] boolForKey:kPausedKey]) {
    return;
  }
  const bool judged = _engine->check(false);
  if (!judged) {
    return;
  }
  [self recordHealthMinSupported];
  nlohmann::json s = [self engineStatus];
  const std::string state = jstr(s, "state");
  if (state == "UPDATE_AVAILABLE") {
    xiaotu::log_info("更新：发现新版本 " + jstr(s, "version"));
  }
#if XIAOTU_HAS_SPARKLE
  // 只触发检查。Q25 现行口径下 Info.plist 的 SUAutomaticallyUpdate 与
  // SUAllowsAutomaticUpdates 都是 false，Sparkle 不会自动下载或安装；
  // 下载与安装只能由用户在关于页显式点「下载并校验」「安装更新」触发。
  if (state == "UPDATE_AVAILABLE" || state == "DONE") {
    dispatch_async(dispatch_get_main_queue(), ^{
      [[[self sparkleController] updater] checkForUpdatesInBackground];
    });
  }
#else
  if (state == "UPDATE_AVAILABLE") {
    xiaotu::log_info("更新：待下载版本 " + jstr(s, "version"));
  }
#endif
  [self evaluatePrefetchGateAutoStart:YES];
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
