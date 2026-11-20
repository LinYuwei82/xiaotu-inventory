#include "updater.h"

#import <AppKit/AppKit.h>
#import <CommonCrypto/CommonDigest.h>
#import <Foundation/Foundation.h>

#include <cstdlib>
#include <optional>
#include <string>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "xiaotu/log.h"
#include "xiaotu/update.h"
#include "xiaotu/update_signature.h"

#if __has_include(<Sparkle/Sparkle.h>)
#import <Sparkle/Sparkle.h>
#define XIAOTU_HAS_SPARKLE 1
#else
#define XIAOTU_HAS_SPARKLE 0
#endif

namespace {
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
// 清单检查失败计数：成功一次即清零；失败后按 1/10/60 分钟退避。
NSString* const kCheckFailuresKey = @"XiaotuCheckFailures";
// 服务端轻量握手要求的最低客户端版本；未配置或从未握手为 nil。
NSString* const kMinSupportedKey = @"XiaotuMinSupportedVersion";
// 灰度分桶用的安装标识：首次检查时生成 UUID，之后保持不变。
NSString* const kInstallIDKey = @"XiaotuInstallID";
NSString* const kAutoCheckKey = @"XiaotuAutoCheckEnabled";
NSString* const kPrefetchKey = @"XiaotuPrefetchEnabled";
NSString* const kPrefetchNoticeKey = @"XiaotuPrefetchNoticeAcked";
NSString* const kPrefetchVersionKey = @"XiaotuPrefetchVersion";
NSString* const kPrefetchAttemptsKey = @"XiaotuPrefetchAttempts";
NSString* const kPrefetchStateKey = @"XiaotuPrefetchState";
NSString* const kPrefetchFileKey = @"XiaotuPrefetchFile";
NSString* const kPrefetchBytesKey = @"XiaotuPrefetchBytes";
NSString* const kPrefetchTotalKey = @"XiaotuPrefetchTotal";
NSString* const kPrefetchMessageKey = @"XiaotuPrefetchMessage";
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

// 后台预下载状态。running / retrying 只在本次进程内有效，重启后回到 idle。
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

constexpr NSTimeInterval kCheckInterval = 86400;  // 24h
constexpr NSTimeInterval kJitter = 7200;          // ±2h
constexpr NSTimeInterval kRequestTimeout = 10;
constexpr NSTimeInterval kManualCooldown = 600;  // 10min
constexpr NSTimeInterval kFirstCheckDelay = 30;  // 首次安装后的首轮检查延迟
constexpr int kMaxLaunchFailures = 3;
// 新版本连续稳定运行 3 次后，删除安装前保留的旧版本副本。
constexpr int kStableLaunchesBeforeCleanup = 3;

// status.last_result 与 checkNow 的 code 取值，和交接单第 2、3 节保持一致。
NSString* const kResultCheckFailed = @"CHECK_FAILED";
NSString* const kResultNotConfigured = @"NOT_CONFIGURED";
NSString* const kResultPaused = @"PAUSED";
NSString* const kResultNoUpdate = @"NO_UPDATE";
NSString* const kResultUpdateAvailable = @"UPDATE_AVAILABLE";
NSString* const kResultSkipped = @"SKIPPED";
NSString* const kResultIncompatible = @"INCOMPATIBLE_SCHEMA";
NSString* const kResultIntermediate = @"INTERMEDIATE_REQUIRED";
NSString* const kResultOsUnsupported = @"OS_UNSUPPORTED";
NSString* const kResultIncompatibleApi = @"INCOMPATIBLE_API";
NSString* const kResultNotInRollout = @"NOT_IN_ROLLOUT";

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

long long json_long(NSDictionary* body, NSString* key) {
  id value = body[key];
  return [value isKindOfClass:[NSNumber class]] ? [value longLongValue] : 0;
}

bool json_bool(NSDictionary* body, NSString* key) {
  id value = body[key];
  return [value isKindOfClass:[NSNumber class]] && [value boolValue];
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

// 本机 macOS 版本，形如 "14.1.2"，用于清单 min_os 判定。
NSString* system_version_string() {
  NSOperatingSystemVersion version = [[NSProcessInfo processInfo] operatingSystemVersion];
  return [NSString stringWithFormat:@"%ld.%ld.%ld", static_cast<long>(version.majorVersion),
                                    static_cast<long>(version.minorVersion),
                                    static_cast<long>(version.patchVersion)];
}

// v2 清单的 macOS 段 build 固定为 0：待安装状态一律用版本号判定，
// 否则 pending_build(0) 永远小于本地日期构建号，界面会一直显示“无更新”。
std::string pending_version(NSUserDefaults* defaults) {
  NSString* value = [defaults stringForKey:kPendingVersionKey];
  return value.length > 0 ? std::string(value.UTF8String) : std::string();
}

bool auto_check_enabled() {
  return [[NSUserDefaults standardUserDefaults] boolForKey:kAutoCheckKey];
}

bool prefetch_enabled() {
  return [[NSUserDefaults standardUserDefaults] boolForKey:kPrefetchKey];
}

// 预下载缓存目录：Application Support 下按 bundle id 分开，卸载时随应用数据一起清理。
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

NSString* safe_file_component(NSString* text) {
  NSMutableString* out = [NSMutableString string];
  for (NSUInteger i = 0; i < text.length; ++i) {
    const unichar ch = [text characterAtIndex:i];
    const BOOL allowed = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                         (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_';
    [out appendString:allowed ? [NSString stringWithFormat:@"%C", ch] : @"_"];
  }
  if (out.length == 0 || [out isEqualToString:@"."] || [out isEqualToString:@".."]) {
    return @"update-package";
  }
  return out;
}

NSString* prefetch_file_name(NSDictionary* manifest) {
  NSString* urlText = json_string(manifest, @"url") ?: @"";
  NSString* last = [[NSURL URLWithString:urlText] lastPathComponent] ?: @"update-package";
  last = [last stringByRemovingPercentEncoding] ?: last;
  NSString* version = json_string(manifest, @"version") ?: @"0.0.0";
  return [NSString stringWithFormat:@"%@-%lld-%@", safe_file_component(version),
                                    json_long(manifest, @"build"), safe_file_component(last)];
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

}  // namespace

// 后台预下载：URLSession 的下载任务在完成回调里文件只在本次回调内有效，
// 所以这里先同步搬到目标路径，再把结果异步交给 updater 的串行队列处理校验。
@interface XiaotuPrefetchDownload : NSObject <NSURLSessionDownloadDelegate>
@property(nonatomic, strong) NSURLSession* session;
@property(nonatomic, strong) NSURLSessionDownloadTask* task;
@property(nonatomic, copy) NSURL* destination;
// 下面三个会被取消方与 URLSession 回调线程同时访问，保持原子属性。
@property(atomic, assign) BOOL cancelled;
@property(atomic, copy, nullable) void (^onProgress)(int64_t received, int64_t total);
@property(atomic, copy, nullable) void (^onFinish)(NSURL* location, NSError* error);
@end

@implementation XiaotuPrefetchDownload

- (void)startWithURL:(NSURL*)url destination:(NSURL*)destination {
  self.destination = destination;
  NSURLSessionConfiguration* config = [NSURLSessionConfiguration ephemeralSessionConfiguration];
  // 安装包可能较大，单次请求超时留足时间；连接失败仍由系统尽快返回。
  config.timeoutIntervalForRequest = 30;
  config.timeoutIntervalForResource = 1800;
  self.session = [NSURLSession sessionWithConfiguration:config delegate:self delegateQueue:nil];
  self.task = [self.session downloadTaskWithURL:url];
  [self.task resume];
}

- (void)cancel {
  self.cancelled = YES;
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
  if (self.onProgress != nil) {
    self.onProgress(totalBytesWritten, totalBytesExpectedToWrite);
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
  [manager removeItemAtURL:self.destination error:NULL];
  if (![manager moveItemAtURL:location toURL:self.destination error:&error]) {
    [self report:nil error:error];
    return;
  }
  [self report:self.destination error:nil];
}

- (void)URLSession:(NSURLSession*)session
              task:(NSURLSessionTask*)task
didCompleteWithError:(NSError*)error {
  if (self.cancelled || error == nil) {
    return;
  }
  [self report:nil error:error];
}

- (void)report:(nullable NSURL*)location error:(nullable NSError*)error {
  void (^finish)(NSURL*, NSError*) = self.onFinish;
  self.onFinish = nil;
  // 取消路径已经 invalidate 过，重复调用会抛异常。
  if (!self.cancelled && self.session != nil) {
    [self.session finishTasksAndInvalidate];
  }
  if (finish != nil) {
    finish(location, error);
  }
}

@end

#if XIAOTU_HAS_SPARKLE
@interface XiaotuUpdater () <SPUUpdaterDelegate>
#else
@interface XiaotuUpdater ()
#endif
@property(nonatomic, strong) dispatch_queue_t queue;
@property(nonatomic, copy, nullable) NSString* manifestURL;
@property(nonatomic, copy, nullable) NSString* healthBase;
@property(nonatomic, copy) NSString* channel;
@property(nonatomic, assign) BOOL running;
// 递增令牌用于作废旧定时器；只在 self.queue 上读写。
@property(nonatomic, assign) NSUInteger timerToken;
// 后台预下载：状态与进度只在 self.queue 上写，status 在主线程读。
@property(nonatomic, strong, nullable) XiaotuPrefetchDownload* prefetchDownload;
@property(nonatomic, copy) NSString* prefetchState;
@property(nonatomic, copy, nullable) NSString* prefetchMessage;
@property(nonatomic, copy, nullable) NSString* prefetchFile;
@property(nonatomic, assign) long long prefetchReceived;
@property(nonatomic, assign) long long prefetchTotal;
@property(nonatomic, assign) NSUInteger prefetchGeneration;
@property(nonatomic, assign) NSUInteger prefetchRetryToken;
// 本次下载是否由用户手动发起：手动失败不进入自动退避重试。
@property(nonatomic, assign) BOOL prefetchManual;
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
    _prefetchState = [defaults stringForKey:kPrefetchStateKey] ?: kPrefetchIdle;
    // running / retrying 只在本次进程内有效，重启后下载已经不存在，回到 idle。
    if ([_prefetchState isEqualToString:kPrefetchRunning] ||
        [_prefetchState isEqualToString:kPrefetchRetrying]) {
      _prefetchState = kPrefetchIdle;
      [defaults setObject:kPrefetchIdle forKey:kPrefetchStateKey];
    }
    _prefetchFile = [defaults stringForKey:kPrefetchFileKey];
    _prefetchMessage = [defaults stringForKey:kPrefetchMessageKey];
    _prefetchTotal = static_cast<long long>([defaults integerForKey:kPrefetchTotalKey]);
    _prefetchReceived =
        static_cast<long long>([defaults integerForKey:kPrefetchBytesKey]);
    // 上一次安装成功后旧包仍会留在缓存目录：done 且待装版本不高于当前版本时，
    // 说明该版本已装上（或已被更新版本覆盖），删除旧包并回到 idle，避免长期占磁盘。
    // 同时：pending_version 若已被安装覆盖也必须清掉，否则前端 status 里
    // is_newer_product_version(pending, current) 虽然动态返回 false，
    // 但 last_result 还留着 UPDATE_AVAILABLE 会让"结果文案"一直显示"发现新版本"。
    // 2026-11-19 用户从 1.0.3 在线更新到 1.0.4 后仍显示"待安装版本 1.0.4"即因此。
    const bool pendingStale =
        !xiaotu::is_newer_product_version(pending_version(defaults),
                                          xiaotu::current_product_version());
    if (pendingStale && [defaults stringForKey:kPendingVersionKey].length > 0) {
      [self clearPending];
    }
    if ([_prefetchState isEqualToString:kPrefetchDone] && pendingStale) {
      [self removePrefetchFilesExcept:nil];
      _prefetchState = kPrefetchIdle;
      _prefetchFile = nil;
      _prefetchMessage = nil;
      _prefetchReceived = 0;
      _prefetchTotal = 0;
      [defaults setObject:kPrefetchIdle forKey:kPrefetchStateKey];
      [defaults removeObjectForKey:kPrefetchFileKey];
      [defaults removeObjectForKey:kPrefetchVersionKey];
      [defaults removeObjectForKey:kPrefetchAttemptsKey];
      [defaults removeObjectForKey:kPrefetchBytesKey];
      [defaults removeObjectForKey:kPrefetchTotalKey];
      [defaults removeObjectForKey:kPrefetchMessageKey];
    }
    // 缓存目录可能被系统清理过：done 但文件不在时按 idle 处理，界面不会给一个打不开的包。
    if ([_prefetchState isEqualToString:kPrefetchDone] &&
        (_prefetchFile.length == 0 ||
         ![[NSFileManager defaultManager] fileExistsAtPath:_prefetchFile])) {
      _prefetchState = kPrefetchIdle;
      _prefetchFile = nil;
      [defaults setObject:kPrefetchIdle forKey:kPrefetchStateKey];
      [defaults removeObjectForKey:kPrefetchFileKey];
    }
  }
  return self;
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
  }
  dispatch_async(self.queue, ^{
    [self scheduleNextCheckIfNeeded];
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
  // 两种情况都算“更新后不稳定”：新版本仍在验证期，或更高版本已下载但尚未装上。
  // v2 的 macOS 没有构建号，这里按版本号判定（与清单的更新判定同一口径）。
  if (xiaotu::should_enter_recovery_version(
          static_cast<int>(attempts), kMaxLaunchFailures,
          validatingVersion.UTF8String ?: "", pending_version(defaults),
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
    NSString* validatingVersion = [currentDefaults stringForKey:kValidatingVersionKey];
    if (!recovery &&
        xiaotu::is_same_product_version(validatingVersion.UTF8String ?: "",
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

// 清除待更新与中间版本记录（两者互斥）；同时作废该版本的后台预下载。
// 跳过版本与定时不受影响。
- (void)clearPending {
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  [defaults removeObjectForKey:kPendingBuildKey];
  [defaults removeObjectForKey:kPendingVersionKey];
  [defaults removeObjectForKey:kPendingReasonKey];
  [defaults removeObjectForKey:kPendingDeadlineKey];
  [defaults removeObjectForKey:kPendingNotesKey];
  [defaults removeObjectForKey:kPendingURLKey];
  [defaults removeObjectForKey:kPendingSHAKey];
  [defaults removeObjectForKey:kPendingSizeKey];
  [defaults setBool:NO forKey:kMandatoryKey];
  [defaults removeObjectForKey:kIntermediateVersionKey];
  [defaults removeObjectForKey:kIntermediateBuildKey];
  [defaults removeObjectForKey:kIntermediateSchemaKey];
  // 同步把 last_result 置为 NO_UPDATE，否则前端 resultText 还会显示"发现新版本"——
  // 即使 update_available 已经变成 false、pending_version 已清空，文案会误导用户。
  // 2026-11-19 在线更新 1.0.3→1.0.4 后仍显示"发现新版本"即因此。
  [defaults setObject:@"NO_UPDATE" forKey:kLastResultKey];
  // 调用方可能来自主线程（跳过版本），统一回到串行队列里改预下载状态。
  dispatch_async(self.queue, ^{
    [self resetPrefetchOnQueue];
  });
}

- (void)setPrefetchState:(NSString*)state message:(nullable NSString*)message {
  self.prefetchState = state;
  self.prefetchMessage = message;
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  [defaults setObject:state forKey:kPrefetchStateKey];
  if (message.length > 0) {
    [defaults setObject:message forKey:kPrefetchMessageKey];
  } else {
    [defaults removeObjectForKey:kPrefetchMessageKey];
  }
}

// 取消进行中的下载并作废回调用令牌；只在 self.queue 上调用。
- (void)cancelActiveDownload {
  XiaotuPrefetchDownload* download = self.prefetchDownload;
  self.prefetchDownload = nil;
  self.prefetchGeneration += 1;
  self.prefetchManual = NO;
  if (download != nil) {
    [download cancel];
  }
  NSURL* directory = prefetch_directory();
  if (directory == nil) {
    return;
  }
  NSArray<NSURL*>* files =
      [[NSFileManager defaultManager] contentsOfDirectoryAtURL:directory
                                    includingPropertiesForKeys:nil
                                                       options:NSDirectoryEnumerationSkipsHiddenFiles
                                                         error:NULL];
  for (NSURL* file in files) {
    if ([file.pathExtension isEqualToString:@"part"]) {
      [[NSFileManager defaultManager] removeItemAtURL:file error:NULL];
    }
  }
}

// 同一时间只保留一个待装包：新版本开始下载前清掉旧包与旧进度。
- (void)removePrefetchFilesExcept:(nullable NSString*)keepName {
  NSURL* directory = prefetch_directory();
  if (directory == nil) {
    return;
  }
  NSArray<NSURL*>* files =
      [[NSFileManager defaultManager] contentsOfDirectoryAtURL:directory
                                    includingPropertiesForKeys:nil
                                                       options:NSDirectoryEnumerationSkipsHiddenFiles
                                                         error:NULL];
  for (NSURL* file in files) {
    if (keepName.length > 0 &&
        ([file.lastPathComponent isEqualToString:keepName] ||
         [file.lastPathComponent isEqualToString:[keepName stringByAppendingString:@".part"]])) {
      continue;
    }
    [[NSFileManager defaultManager] removeItemAtURL:file error:NULL];
  }
}

// 清理该版本的预下载记录；只在 self.queue 上调用。
- (void)resetPrefetchOnQueue {
  self.prefetchRetryToken += 1;
  [self cancelActiveDownload];
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  [defaults removeObjectForKey:kPrefetchVersionKey];
  [defaults removeObjectForKey:kPrefetchAttemptsKey];
  [defaults removeObjectForKey:kPrefetchFileKey];
  [defaults removeObjectForKey:kPrefetchBytesKey];
  [defaults removeObjectForKey:kPrefetchTotalKey];
  [defaults removeObjectForKey:kPrefetchMessageKey];
  [defaults setObject:kPrefetchIdle forKey:kPrefetchStateKey];
  self.prefetchState = kPrefetchIdle;
  self.prefetchMessage = nil;
  self.prefetchFile = nil;
  self.prefetchReceived = 0;
  self.prefetchTotal = 0;
  [self removePrefetchFilesExcept:nil];
}

// 待更新版本的安装包信息；用 status/ack/手动下载时不必重新拉清单。
- (nullable NSDictionary*)pendingPackageManifest {
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  NSString* version = [defaults stringForKey:kPendingVersionKey];
  NSString* url = [defaults stringForKey:kPendingURLKey];
  if (version.length == 0 || url.length == 0) {
    return nil;
  }
  return @{
    @"version" : version,
    @"build" : @([defaults integerForKey:kPendingBuildKey]),
    @"url" : url,
    @"sha256" : [defaults stringForKey:kPendingSHAKey] ?: @"",
    @"size" : @([defaults integerForKey:kPendingSizeKey]),
  };
}

// 自动检查命中新版本后调用：记录版本、按开关与提示确认情况决定是否自动开始。
// autoStart=NO 用于手动检查：只准备状态，不自动下载。
- (void)preparePrefetchForManifest:(NSDictionary*)manifest autoStart:(BOOL)autoStart {
  NSString* version = json_string(manifest, @"version");
  if (version.length == 0) {
    return;
  }
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  NSString* tracked = [defaults stringForKey:kPrefetchVersionKey];
  if (![tracked isEqualToString:version]) {
    [self resetPrefetchOnQueue];
    [defaults setObject:version forKey:kPrefetchVersionKey];
  }
  if (!prefetch_enabled()) {
    if ([self.prefetchState isEqualToString:kPrefetchRunning] ||
        [self.prefetchState isEqualToString:kPrefetchRetrying] ||
        [self.prefetchState isEqualToString:kPrefetchNeedsConsent]) {
      [self cancelActiveDownload];
      [self setPrefetchState:kPrefetchIdle message:nil];
    }
    return;
  }
  if ([self.prefetchState isEqualToString:kPrefetchDone]) {
    if (self.prefetchFile.length > 0 &&
        [[NSFileManager defaultManager] fileExistsAtPath:self.prefetchFile]) {
      return;
    }
    [self setPrefetchState:kPrefetchIdle message:nil];
  }
  // 取消或失败过的版本不再自动重试；手动下载仍可。
  if ([self.prefetchState isEqualToString:kPrefetchCanceled] ||
      [self.prefetchState isEqualToString:kPrefetchFailed]) {
    return;
  }
  if (![defaults boolForKey:kPrefetchNoticeKey]) {
    [self setPrefetchState:kPrefetchNeedsConsent
                   message:@"首次后台下载前需要确认流量提示"];
    return;
  }
  if (!autoStart) {
    if (![self.prefetchState isEqualToString:kPrefetchRunning] &&
        ![self.prefetchState isEqualToString:kPrefetchRetrying]) {
      [self setPrefetchState:kPrefetchIdle message:nil];
    }
    return;
  }
  [self startPrefetchWithManifest:manifest manual:NO];
}

// 真正发起下载；manual=YES 表示用户显式点了「下载并校验」，不受取消记录限制。
- (void)startPrefetchWithManifest:(NSDictionary*)manifest manual:(BOOL)manual {
  if (!manual && !prefetch_enabled()) {
    return;
  }
  if (self.prefetchDownload != nil) {
    return;
  }
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  NSString* version = json_string(manifest, @"version");
  if (version.length == 0) {
    return;
  }
  if (![[defaults stringForKey:kPrefetchVersionKey] isEqualToString:version]) {
    [self resetPrefetchOnQueue];
    [defaults setObject:version forKey:kPrefetchVersionKey];
  }
  if (!manual && [self.prefetchState isEqualToString:kPrefetchCanceled]) {
    return;
  }
  if (![defaults boolForKey:kPrefetchNoticeKey]) {
    if (!manual) {
      [self setPrefetchState:kPrefetchNeedsConsent
                     message:@"首次后台下载前需要确认流量提示"];
      return;
    }
    // 手动下载是显式动作，视为已知悉流量提示，不再单独弹一次性提示。
    [defaults setBool:YES forKey:kPrefetchNoticeKey];
  }
  if (manual) {
    [defaults setInteger:0 forKey:kPrefetchAttemptsKey];
    self.prefetchRetryToken += 1;
  }
  NSURL* directory = prefetch_directory();
  NSString* name = prefetch_file_name(manifest);
  NSURL* url = [NSURL URLWithString:json_string(manifest, @"url") ?: @""];
  if (directory == nil || name.length == 0 || url == nil) {
    [self setPrefetchState:kPrefetchFailed message:@"无法创建更新缓存目录或安装包地址无效"];
    return;
  }
  NSURL* partURL = [directory URLByAppendingPathComponent:[name stringByAppendingString:@".part"]];
  NSURL* finalURL = [directory URLByAppendingPathComponent:name];
  [self removePrefetchFilesExcept:name];
  [defaults removeObjectForKey:kPrefetchFileKey];
  self.prefetchFile = nil;
  const long long expected = json_long(manifest, @"size");
  self.prefetchReceived = 0;
  self.prefetchTotal = expected > 0 ? expected : 0;
  self.prefetchManual = manual;
  [self setPrefetchState:kPrefetchRunning message:@"正在后台下载安装包"];
  const NSUInteger generation = self.prefetchGeneration + 1;
  self.prefetchGeneration = generation;

  XiaotuPrefetchDownload* download = [[XiaotuPrefetchDownload alloc] init];
  self.prefetchDownload = download;
  dispatch_queue_t queue = self.queue;
  __weak XiaotuUpdater* weakSelf = self;
  download.onProgress = ^(int64_t received, int64_t total) {
    XiaotuUpdater* strongSelf = weakSelf;
    if (strongSelf == nil) {
      return;
    }
    strongSelf.prefetchReceived = received;
    if (total > 0) {
      strongSelf.prefetchTotal = total;
    }
  };
  download.onFinish = ^(NSURL* location, NSError* error) {
    dispatch_async(queue, ^{
      XiaotuUpdater* strongSelf = weakSelf;
      if (strongSelf == nil || generation != strongSelf.prefetchGeneration) {
        return;
      }
      [strongSelf finishPrefetchWithLocation:location
                                       error:error
                                     partURL:partURL
                                    finalURL:finalURL
                                    manifest:manifest];
    });
  };
  [download startWithURL:url destination:partURL];
}

- (void)finishPrefetchWithLocation:(nullable NSURL*)location
                             error:(nullable NSError*)error
                           partURL:(NSURL*)partURL
                          finalURL:(NSURL*)finalURL
                          manifest:(NSDictionary*)manifest {
  (void)location;  // 文件已由下载代理同步搬到 partURL，这里只需处理 partURL。
  self.prefetchDownload = nil;
  const BOOL manual = self.prefetchManual;
  self.prefetchManual = NO;
  if (error != nil) {
    [[NSFileManager defaultManager] removeItemAtURL:partURL error:NULL];
    [self handlePrefetchFailure:[NSString stringWithFormat:@"下载失败：%@",
                                                           error.localizedDescription ?: @"未知原因"]
                        manifest:manifest
                          manual:manual];
    return;
  }
  const long long expectedSize = json_long(manifest, @"size");
  NSDictionary* attributes =
      [[NSFileManager defaultManager] attributesOfItemAtPath:partURL.path error:NULL];
  const long long actualSize =
      attributes != nil ? [attributes[NSFileSize] longLongValue] : 0;
  if (expectedSize > 0 && actualSize != expectedSize) {
    [[NSFileManager defaultManager] removeItemAtURL:partURL error:NULL];
    [self handlePrefetchFailure:[NSString
                                    stringWithFormat:@"安装包大小不符（应为 %lld 字节，实际 %lld 字节）",
                                                     expectedSize, actualSize]
                        manifest:manifest
                          manual:manual];
    return;
  }
  NSString* expectedSHA = [json_string(manifest, @"sha256") lowercaseString];
  NSString* actualSHA = sha256_of_file(partURL);
  if (expectedSHA.length > 0 && ![expectedSHA isEqualToString:actualSHA ?: @""]) {
    [[NSFileManager defaultManager] removeItemAtURL:partURL error:NULL];
    [self handlePrefetchFailure:@"安装包 SHA-256 校验失败" manifest:manifest manual:manual];
    return;
  }
  [[NSFileManager defaultManager] removeItemAtURL:finalURL error:NULL];
  NSError* moveError = nil;
  if (![[NSFileManager defaultManager] moveItemAtURL:partURL toURL:finalURL error:&moveError]) {
    [self handlePrefetchFailure:[NSString stringWithFormat:@"无法保存安装包：%@",
                                                           moveError.localizedDescription ?: @"未知原因"]
                        manifest:manifest
                          manual:manual];
    return;
  }
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  [defaults setInteger:0 forKey:kPrefetchAttemptsKey];
  [defaults setObject:finalURL.path forKey:kPrefetchFileKey];
  [defaults setInteger:actualSize forKey:kPrefetchBytesKey];
  [defaults setInteger:(expectedSize > 0 ? expectedSize : actualSize) forKey:kPrefetchTotalKey];
  self.prefetchFile = finalURL.path;
  self.prefetchReceived = actualSize;
  self.prefetchTotal = expectedSize > 0 ? expectedSize : actualSize;
  [self setPrefetchState:kPrefetchDone message:@"下载并校验通过"];
  xiaotu::log_info("更新：后台预下载完成 " +
                   std::string(finalURL.lastPathComponent.UTF8String ?: ""));
}

- (void)handlePrefetchFailure:(NSString*)message
                     manifest:(NSDictionary*)manifest
                       manual:(BOOL)manual {
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  NSInteger attempts = [defaults integerForKey:kPrefetchAttemptsKey] + 1;
  [defaults setInteger:attempts forKey:kPrefetchAttemptsKey];
  xiaotu::log_warn("更新：后台预下载失败（第 " + std::to_string(attempts) + " 次）：" +
                   (message.UTF8String ?: ""));
  // 手动下载失败直接结束；自动预下载最多 3 次（首次 + 两次退避）。
  const BOOL retry = !manual && prefetch_enabled() && attempts < kMaxPrefetchAttempts;
  if (!retry) {
    [self setPrefetchState:kPrefetchFailed message:message];
    return;
  }
  const NSTimeInterval delay =
      attempts == 1 ? kPrefetchRetryFirstDelay : kPrefetchRetrySecondDelay;
  [self setPrefetchState:kPrefetchRetrying
                 message:[NSString stringWithFormat:@"%@，稍后自动重试（第 %ld/%d 次）", message,
                                                    static_cast<long>(attempts) + 1,
                                                    kMaxPrefetchAttempts]];
  self.prefetchRetryToken += 1;
  const NSUInteger token = self.prefetchRetryToken;
  NSString* version = json_string(manifest, @"version") ?: @"";
  dispatch_queue_t queue = self.queue;
  __weak XiaotuUpdater* weakSelf = self;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(delay * NSEC_PER_SEC)),
                 queue, ^{
                   XiaotuUpdater* strongSelf = weakSelf;
                   if (strongSelf == nil || token != strongSelf.prefetchRetryToken) {
                     return;
                   }
                   if (!prefetch_enabled()) {
                     return;
                   }
                   if (![[[NSUserDefaults standardUserDefaults]
                            stringForKey:kPrefetchVersionKey] isEqualToString:version]) {
                     return;
                   }
                   [strongSelf startPrefetchWithManifest:manifest manual:NO];
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
  dispatch_async(self.queue, ^{
    if (hasAuto) {
      if (auto_check_enabled()) {
        [self scheduleNextCheckIfNeeded];
        [self checkIfDue];
        [self armTimerOnQueue];
      } else {
        // 递增令牌作废已武装的定时器；手动检查不受影响。
        self.timerToken += 1;
      }
    }
    if (hasPrefetch || ackNotice) {
      if (!prefetch_enabled() && !ackNotice) {
        [self cancelActiveDownload];
        self.prefetchRetryToken += 1;
        if ([self.prefetchState isEqualToString:kPrefetchRunning] ||
            [self.prefetchState isEqualToString:kPrefetchRetrying] ||
            [self.prefetchState isEqualToString:kPrefetchNeedsConsent]) {
          [self setPrefetchState:kPrefetchIdle message:nil];
        }
      } else if ([defaults boolForKey:kPrefetchNoticeKey]) {
        NSDictionary* manifest = [self pendingPackageManifest];
        if (manifest != nil) {
          [self preparePrefetchForManifest:manifest autoStart:YES];
        }
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
  dispatch_async(self.queue, ^{
    NSDictionary* manifest = [self pendingPackageManifest];
    if (manifest == nil) {
      respond(@{
        @"ok" : @NO,
        @"code" : @"NO_PENDING_UPDATE",
        @"status" : [self status],
      });
      return;
    }
    if (self.prefetchDownload != nil) {
      respond(@{
        @"ok" : @YES,
        @"code" : @"RUNNING",
        @"status" : [self status],
      });
      return;
    }
    if ([self.prefetchState isEqualToString:kPrefetchDone] && self.prefetchFile.length > 0 &&
        [[NSFileManager defaultManager] fileExistsAtPath:self.prefetchFile]) {
      respond(@{
        @"ok" : @YES,
        @"code" : @"DONE",
        @"status" : [self status],
      });
      return;
    }
    [self startPrefetchWithManifest:manifest manual:YES];
    const BOOL started = [self.prefetchState isEqualToString:kPrefetchRunning];
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
    const BOOL active = self.prefetchDownload != nil ||
                        [self.prefetchState isEqualToString:kPrefetchRunning] ||
                        [self.prefetchState isEqualToString:kPrefetchRetrying];
    self.prefetchRetryToken += 1;
    [self cancelActiveDownload];
    if (active) {
      // 取消后本次版本不再自动重试，但不清版本记录：手动下载仍可。
      [self setPrefetchState:kPrefetchCanceled message:@"已取消本次后台下载"];
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

  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  if ([defaults boolForKey:kMandatoryKey]) {
    NSString* pendingVersion = [defaults stringForKey:kPendingVersionKey];
    if (pendingVersion.length == 0 || [pendingVersion isEqualToString:text]) {
      respond(@{
        @"ok" : @NO,
        @"code" : @"MANDATORY_UPDATE",
        @"status" : [self status],
      });
      return;
    }
  }

  [defaults setObject:text forKey:kSkippedVersionKey];
  [defaults setDouble:now_epoch() + dayCount * 86400 forKey:kSkipUntilKey];
  [defaults setObject:kResultSkipped forKey:kLastResultKey];
  [self clearPending];
  respond(@{
    @"ok" : @YES,
    @"code" : @"SKIPPED",
    @"status" : [self status],
  });
}

- (void)installUpdateWithCompletion:(void (^)(NSDictionary<NSString*, id>* result))completion {
  void (^respond)(NSDictionary*) = ^(NSDictionary* result) {
    dispatch_async(dispatch_get_main_queue(), ^{
      completion(result);
    });
  };
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
#if XIAOTU_HAS_SPARKLE
  const bool installerAvailable = true;
  NSURL* packageURL = nil;
#else
  // 没有 Sparkle 时，后台预下载并校验通过的安装包就是安装入口：由系统打开，
  // 替换与重启仍交给安装器，程序自身不静默替换正在运行的二进制。
  NSURL* packageURL = nil;
  if ([self.prefetchState isEqualToString:kPrefetchDone] && self.prefetchFile.length > 0 &&
      [[NSFileManager defaultManager] fileExistsAtPath:self.prefetchFile]) {
    packageURL = [NSURL fileURLWithPath:self.prefetchFile];
  }
  const bool installerAvailable = packageURL != nil;
#endif
  // v2 的 macOS 清单没有构建号（build 固定 0），待安装判定同样按版本号比较。
  switch (xiaotu::decide_install_version(installerAvailable, pending_version(defaults),
                                         xiaotu::current_product_version())) {
    case xiaotu::InstallDecision::kUnavailable:
      respond(@{
        @"ok" : @NO,
        @"code" : @"UNAVAILABLE",
        @"message" : @"当前构建没有可用的安装包，请手动下载安装",
      });
      return;
    case xiaotu::InstallDecision::kNoPendingUpdate:
      respond(@{
        @"ok" : @NO,
        @"code" : @"NO_PENDING_UPDATE",
        @"message" : @"没有待安装的版本",
      });
      return;
    case xiaotu::InstallDecision::kStarted:
      break;
  }
  // 安装前保留当前版本副本，供新版本连续启动失败时一键回退；失败不阻断安装。
  NSString* backupError = nil;
  if (![self preparePreviousBundleWithTargetVersion:[defaults stringForKey:kPendingVersionKey]
                                              error:&backupError]) {
    xiaotu::log_warn("更新：未能保留旧版本副本，回退将不可用：" +
                     std::string(backupError.UTF8String ?: "未知原因"));
  }
#if XIAOTU_HAS_SPARKLE
  // 脚本消息在主线程派发；控制器也只在主线程创建。
  [[[self sparkleController] updater] checkForUpdates];
  respond(@{@"ok" : @YES, @"code" : @"STARTED"});
#else
  if ([[NSWorkspace sharedWorkspace] openURL:packageURL]) {
    respond(@{
      @"ok" : @YES,
      @"code" : @"OPENED",
      @"message" : @"已打开安装包，请退出本程序后按提示完成安装。",
    });
  } else {
    respond(@{
      @"ok" : @NO,
      @"code" : @"UNAVAILABLE",
      @"message" : @"无法打开安装包，请手动下载安装",
    });
  }
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
  if (self.running && self.manifestURL.length > 0 && auto_check_enabled()) {
    dispatch_async(self.queue, ^{
      [self scheduleNextCheck];
      [self performCheckWithCompletion:nil];
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
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  if ([defaults boolForKey:kPausedKey]) {
    respond(@{
      @"ok" : @NO,
      @"code" : kResultPaused,
      @"update_available" : @NO,
      @"status" : [self status],
    });
    return;
  }
  const double now = now_epoch();
  const std::int64_t retryAfter = xiaotu::manual_check_retry_after(
      static_cast<std::int64_t>([defaults doubleForKey:kLastManualCheckKey]),
      static_cast<std::int64_t>(now), static_cast<std::int64_t>(kManualCooldown));
  if (retryAfter > 0) {
    respond(@{
      @"ok" : @NO,
      @"code" : @"COOLDOWN",
      @"retry_after" : @(retryAfter),
      @"update_available" : @NO,
      @"status" : [self status],
    });
    return;
  }
  [defaults setDouble:now forKey:kLastManualCheckKey];

  dispatch_async(self.queue, ^{
    [self performCheckWithCompletion:^(NSString* lastResult) {
      NSDictionary* status = [self status];
      const BOOL ok = ![lastResult isEqualToString:kResultCheckFailed];
      respond(@{
        @"ok" : @(ok),
        @"code" : ok ? @"CHECKED" : kResultCheckFailed,
        @"last_result" : lastResult,
        @"update_available" : status[@"update_available"],
        @"status" : status,
      });
    }];
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
    NSDictionary* health =
        fetch_json([NSURL URLWithString:[base stringByAppendingString:@"/api/health"]], nil);
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

- (NSDictionary<NSString*, id>*)status {
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  // 与检查链路同一口径：版本更高，或同版本但待安装构建号更大。
  const std::string statusPendingVersion = pending_version(defaults);
  const std::int64_t statusPendingBuild = [defaults integerForKey:kPendingBuildKey];
  xiaotu::ProductVersion statusRemote{};
  xiaotu::ProductVersion statusLocal{};
  BOOL updateAvailable = NO;
  if (!statusPendingVersion.empty() &&
      xiaotu::parse_product_version(statusPendingVersion, statusRemote) &&
      xiaotu::parse_product_version(xiaotu::current_product_version(), statusLocal)) {
    const int statusOrder = xiaotu::compare_product_version(statusRemote, statusLocal);
    updateAvailable = statusOrder > 0 || (statusOrder == 0 && statusPendingBuild > current_build());
  }
  NSString* pendingVersion = updateAvailable ? [defaults stringForKey:kPendingVersionKey] : nil;
  const BOOL mandatory = updateAvailable && [defaults boolForKey:kMandatoryKey];
  NSString* reason = mandatory ? [defaults stringForKey:kPendingReasonKey] : nil;
  NSString* notes = updateAvailable ? [defaults stringForKey:kPendingNotesKey] : nil;
  NSString* lastResult = [defaults stringForKey:kLastResultKey];
  id reasonValue = reason.length > 0 ? reason : [NSNull null];
  // 单机版无法直接升级到最新版且没有中间版本时，用 reason 明确表达不兼容。
  if (!mandatory && [lastResult isEqualToString:kResultIncompatible]) {
    reasonValue = kResultIncompatible;
  }
  id notesValue = notes.length > 0 ? notes : [NSNull null];
  NSString* intermediateVersion = [defaults stringForKey:kIntermediateVersionKey];
  id intermediateValue = [NSNull null];
  if (intermediateVersion.length > 0) {
    intermediateValue = @{
      @"version" : intermediateVersion,
      @"build" : @([defaults integerForKey:kIntermediateBuildKey]),
      @"schema" : @([defaults integerForKey:kIntermediateSchemaKey]),
    };
  }
  // 重启后内存里的进度会归零，已完成/上次中断的字节数从 defaults 兜底。
  long long prefetchBytes = self.prefetchReceived;
  if (prefetchBytes <= 0) {
    prefetchBytes = static_cast<long long>([defaults integerForKey:kPrefetchBytesKey]);
  }
  long long prefetchTotal = self.prefetchTotal;
  if (prefetchTotal <= 0) {
    prefetchTotal = static_cast<long long>([defaults integerForKey:kPrefetchTotalKey]);
  }
  return @{
    @"enabled" : @(self.manifestURL.length > 0),
    @"channel" : self.channel ?: @"stable",
    @"auto_check" : @(auto_check_enabled()),
    @"prefetch" : @(prefetch_enabled()),
    @"prefetch_notice_acked" : @([defaults boolForKey:kPrefetchNoticeKey]),
    @"prefetch_state" : self.prefetchState ?: kPrefetchIdle,
    @"prefetch_version" : [defaults stringForKey:kPrefetchVersionKey] ?: [NSNull null],
    @"prefetch_attempts" : @([defaults integerForKey:kPrefetchAttemptsKey]),
    @"prefetch_bytes" : @(prefetchBytes),
    @"prefetch_total" : @(prefetchTotal),
    @"prefetch_file" : self.prefetchFile.length > 0 ? [self.prefetchFile lastPathComponent]
                                                     : [NSNull null],
    @"prefetch_message" : self.prefetchMessage.length > 0 ? self.prefetchMessage
                                                          : [NSNull null],
    @"next_check_at" : @([defaults doubleForKey:kNextCheckKey]),
    // 失败退避的可观测字段：失败计数与下次重试时间（next_check_at 已按退避重排）。
    @"check_failures" : @([defaults integerForKey:kCheckFailuresKey]),
    @"min_supported_version" : [defaults stringForKey:kMinSupportedKey] ?: [NSNull null],
    @"launch_attempts" : @([defaults integerForKey:kLaunchAttemptsKey]),
    @"paused" : @([defaults boolForKey:kPausedKey]),
    // 恢复模式：连续启动失败后引导回退；previous 为安装前保留的旧版本副本。
    @"recovery_required" : @([defaults boolForKey:kRecoveryRequiredKey]),
    @"previous" : [self previousBundleStatus],
    @"skipped_version" : [defaults stringForKey:kSkippedVersionKey] ?: [NSNull null],
    @"skip_until" : @([defaults doubleForKey:kSkipUntilKey]),
    @"last_check_at" : @([defaults doubleForKey:kLastCheckKey]),
    @"last_result" : lastResult ?: [NSNull null],
    @"pending_version" : pendingVersion ?: [NSNull null],
    // macOS 没有构建号，固定给 0；界面按“无构建号”展示。
    @"pending_build" : @0,
    // 安装包大小：Sparkle 安装前会重新下载，界面据此提示“将重新下载约 X MB”。
    @"pending_size" : @(updateAvailable ? [defaults integerForKey:kPendingSizeKey] : 0),
    @"pending_deadline" : updateAvailable
        ? (id)([defaults stringForKey:kPendingDeadlineKey] ?: [NSNull null])
        : [NSNull null],
    @"update_available" : @(updateAvailable),
    @"mandatory" : @(mandatory),
    @"reason" : reasonValue,
    @"release_notes_url" : notesValue,
    // 本地 schema 低于 schema_min 时由清单 upgrade_path 选出的中间版本；其余情况为 null。
    @"intermediate" : intermediateValue,
  };
}

// 按 next_check_at 武装一次性定时器；必须在 self.queue 上调用。
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
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  if ([defaults boolForKey:kPausedKey]) {
    return;
  }
  if ([defaults doubleForKey:kNextCheckKey] <= 0) {
    [self scheduleNextCheck];
  }
  const double delay = [defaults doubleForKey:kNextCheckKey] - now_epoch();
  const int64_t nanoseconds = static_cast<int64_t>(MAX(1.0, delay) * NSEC_PER_SEC);
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, nanoseconds), self.queue, ^{
    if (token != self.timerToken) {
      return;  // 已被更新的排期取代
    }
    [self checkIfDue];
    [self armTimerOnQueue];
  });
}

- (void)scheduleNextCheckIfNeeded {
  if (!auto_check_enabled()) {
    return;
  }
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  if ([defaults doubleForKey:kNextCheckKey] <= 0) {
    // 首次安装：启动后 30 秒做首轮检查，避免新装用户要等满 24 小时；
    // 之后由 checkIfDue 重新按 24h±2h 排期。
    [defaults setDouble:now_epoch() + kFirstCheckDelay forKey:kNextCheckKey];
  }
}

- (void)scheduleNextCheck {
  const double jitter = (arc4random_uniform(static_cast<uint32_t>(kJitter * 2 + 1)) - kJitter);
  const double next = now_epoch() + kCheckInterval + jitter;
  [[NSUserDefaults standardUserDefaults] setDouble:next forKey:kNextCheckKey];
}

- (void)checkIfDue {
  if (!self.running || self.manifestURL.length == 0 || !auto_check_enabled()) {
    return;
  }
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  if ([defaults boolForKey:kPausedKey]) {
    return;
  }
  if (now_epoch() < [defaults doubleForKey:kNextCheckKey]) {
    return;
  }
  [self scheduleNextCheck];
  [self performCheckWithCompletion:nil];
}

// 在 self.queue 上执行一次清单检查；completion 带回 last_result 并在主线程调用。
- (void)performCheckWithCompletion:(nullable void (^)(NSString* lastResult))completion {
  NSString* manifestURL = self.manifestURL;
  __block BOOL finished = NO;
  void (^finish)(NSString*) = ^(NSString* code) {
    if (finished) {
      return;
    }
    finished = YES;
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    [defaults setDouble:now_epoch() forKey:kLastCheckKey];
    [defaults setObject:code forKey:kLastResultKey];
    // 检查失败按 1 分钟 / 10 分钟 / 1 小时退避，避免网络异常时整天不重试；
    // 任意一次成功都清零，回到正常的 24h±2h 节奏。
    const BOOL failed = [code isEqualToString:kResultCheckFailed];
    if (failed) {
      const NSInteger failures = [defaults integerForKey:kCheckFailuresKey] + 1;
      [defaults setInteger:failures forKey:kCheckFailuresKey];
      const std::int64_t delay = xiaotu::check_retry_delay(static_cast<int>(failures));
      [defaults setDouble:now_epoch() + static_cast<double>(delay) forKey:kNextCheckKey];
      xiaotu::log_warn("更新：清单检查失败（第 " + std::to_string(failures) + " 次），" +
                       std::to_string(delay) + " 秒后重试");
    } else if ([defaults integerForKey:kCheckFailuresKey] != 0) {
      [defaults setInteger:0 forKey:kCheckFailuresKey];
    }
    // 自动检查后重新武装；手动检查失败也按退避重排，避免继续等满 24 小时。
    if (completion == nil || failed) {
      [self armTimerOnQueue];
    }
    if (completion != nil) {
      dispatch_async(dispatch_get_main_queue(), ^{
        completion(code);
      });
    }
  };

  NSString* fetchError = nil;
  NSDictionary* manifest = fetch_json([NSURL URLWithString:manifestURL], &fetchError);
  if (manifest == nil) {
    // 首次启动常见冷 DNS / CDN 首包慢，重试一次再判失败，避免误判后等 24 小时
    [NSThread sleepForTimeInterval:2.0];
    fetchError = nil;
    manifest = fetch_json([NSURL URLWithString:manifestURL], &fetchError);
  }
  if (manifest == nil) {
    xiaotu::log_warn(std::string("更新：清单获取失败（") +
                     (fetchError.UTF8String ?: "未知原因") + "），按下一周期重试");
    finish(kResultCheckFailed);
    return;
  }

  // ---- Ed25519 清单签名校验（信任锚）----
  // 公钥从 Info.plist 的 SUPublicEDKey 读取（Sparkle 自己也会用）。
  // 验签失败 → 直接 abort，不做版本比较、不通知用户、不触发 Sparkle。
  // OpenSSL 不可用时（XIAOTU_HAS_OPENSSL 未定义）跳过 C++ 侧验签，
  // 由 Sparkle 在用户点"安装更新"时自行校验 appcast.xml EdDSA。
  NSString* pubKeyB64 = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"SUPublicEDKey"];
  if (pubKeyB64.length > 0) {
    @try {
      NSError* jserr = nil;
      NSData* jsonData = [NSJSONSerialization dataWithJSONObject:manifest
                                                        options:0
                                                          error:&jserr];
      if (jsonData != nil) {
        std::string jsonStr(reinterpret_cast<const char*>([jsonData bytes]),
                            [jsonData length]);
        nlohmann::json parsed = nlohmann::json::parse(jsonStr);
        if (!xiaotu::verify_manifest_signature(parsed, pubKeyB64.UTF8String)) {
          xiaotu::log_error("更新：清单 Ed25519 验签失败，本次检查中止");
          finish(kResultCheckFailed);
          return;
        }
        xiaotu::log_info("更新：清单 Ed25519 验签通过");
      }
    } @catch (NSException* ex) {
      xiaotu::log_warn(std::string("更新：清单转 JSON 异常：") +
                       (ex.reason.UTF8String ?: "unknown"));
      finish(kResultCheckFailed);
      return;
    }
  } else {
    xiaotu::log_warn("更新：Info.plist 未配置 SUPublicEDKey，跳过 C++ 侧验签");
  }

  // ---- v2 统一清单：取 platforms.macos 拍平成原有字段，后续判定与预下载不变 ----
  // 未知字段与未知 artifacts key 一律忽略，只取需要的键。
  id platforms = manifest[@"platforms"];
  id macPlatform = [platforms isKindOfClass:[NSDictionary class]] ? platforms[@"macos"] : nil;
  if (![macPlatform isKindOfClass:[NSDictionary class]]) {
    xiaotu::log_warn("更新：清单缺少 platforms.macos，本次不提示");
    [self clearPending];
    finish(kResultNoUpdate);
    return;
  }
  NSString* channel = json_string(manifest, @"channel");
  if (![channel isEqualToString:self.channel]) {
    xiaotu::log_warn("更新：清单的通道与当前构建不匹配，本次不提示");
    [self clearPending];
    finish(kResultNoUpdate);
    return;
  }
  id artifacts = macPlatform[@"artifacts"];
  id dmg = [artifacts isKindOfClass:[NSDictionary class]] ? artifacts[@"dmg"] : nil;
  NSString* dmgURL = json_string(dmg, @"url");
  NSString* dmgSHA256 = json_string(dmg, @"sha256");
  const long long dmgSize = json_long(dmg, @"size");
  if (dmgURL.length == 0 || dmgSHA256.length == 0 || dmgSize <= 0) {
    xiaotu::log_warn("更新：清单缺少可用的 dmg 产物，本次检查按失败处理");
    finish(kResultCheckFailed);
    return;
  }
  // 拍平（只取需要的键）：下游预下载与待安装状态继续按原字段消费；
  // notes_url 映射为原 release_notes_url 字段。
  manifest = @{
    @"version" : json_string(macPlatform, @"version") ?: @"",
    @"build" : @(json_long(macPlatform, @"build")),
    @"min_os" : json_string(macPlatform, @"min_os") ?: @"",
    @"release_notes_url" : json_string(macPlatform, @"notes_url") ?: @"",
    @"rollout_percent" : @(json_long(macPlatform, @"rollout_percent")),
    @"mandatory" : @(json_bool(macPlatform, @"mandatory")),
    @"mandatory_reason" : json_string(macPlatform, @"mandatory_reason") ?: @"",
    @"mandatory_deadline" : json_string(macPlatform, @"mandatory_deadline") ?: @"",
    @"url" : dmgURL,
    @"sha256" : dmgSHA256,
    @"size" : @(dmgSize),
  };
  // 清单声明的最低系统版本：与 core 的 evaluate_manifest 同一口径。
  NSString* minOS = json_string(manifest, @"min_os");
  if (minOS.length > 0) {
    NSString* systemVersion = system_version_string();
    if (xiaotu::compare_dotted_version(systemVersion.UTF8String, minOS.UTF8String) < 0) {
      xiaotu::log_warn("更新：当前系统 " + std::string(systemVersion.UTF8String) +
                       " 低于清单要求 " + std::string(minOS.UTF8String) + "，本次不提示");
      [self clearPending];
      finish(kResultOsUnsupported);
      return;
    }
  }
  // v2：macOS 已带构建号（与 Info.plist CFBundleVersion 同源），同版本重发靠
  // build 区分，与 Windows / Mote / DevCard 同一口径。
  NSString* remoteVersionText = json_string(manifest, @"version");
  xiaotu::ProductVersion remoteVersion{};
  xiaotu::ProductVersion localVersion{};
  if (remoteVersionText.length == 0 ||
      !xiaotu::parse_product_version(remoteVersionText.UTF8String, remoteVersion) ||
      !xiaotu::parse_product_version(xiaotu::current_product_version(), localVersion)) {
    xiaotu::log_warn("更新：清单 version 缺失或非法，本次检查按失败处理");
    finish(kResultCheckFailed);
    return;
  }
  const std::int64_t remoteBuild = json_long(manifest, @"build");
  const int versionOrder = xiaotu::compare_product_version(remoteVersion, localVersion);
  if (versionOrder < 0 ||
      (versionOrder == 0 && remoteBuild <= current_build())) {
    [self clearPending];
    finish(kResultNoUpdate);
    return;
  }

  if (self.healthBase.length > 0) {
    NSString* healthError = nil;
    NSDictionary* health = fetch_json([NSURL URLWithString:
                                           [self.healthBase stringByAppendingString:@"/api/health"]],
                                       &healthError);
    // 轻量握手：服务端可要求最低客户端版本。这里只记录与告警，
    // 启动时的阻塞页由 webview 在加载前单独检查，避免等待 30 秒后的首轮检查。
    NSString* minimum = json_string(health, @"min_supported_version");
    if (minimum.length > 0) {
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
  }
  // v2 清单不再携带 schema/api 区间与 upgrade_path：数据版本兼容性由安装器
  // 与服务端握手把关，这里不做 schema/api 拦截。

  const bool mandatory = json_bool(manifest, @"mandatory");
  NSString* version = json_string(manifest, @"version");
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  // 灰度：强制更新不受分桶限制；其余按安装 UUID + 目标 build 稳定分桶。
  if (!mandatory) {
    const long long rollout = json_long(manifest, @"rollout_percent");
    const int percent =
        (rollout >= 1 && rollout <= 100) ? static_cast<int>(rollout) : 100;
    const int bucket = xiaotu::rollout_bucket(installation_id().UTF8String, remoteBuild);
    if (!xiaotu::rollout_allows(percent, bucket)) {
      xiaotu::log_info("更新：版本 " + std::string(version.UTF8String ?: "") + " 灰度未命中（" +
                       std::to_string(bucket) + "/" + std::to_string(percent) + "），本次不提示");
      [self clearPending];
      finish(kResultNotInRollout);
      return;
    }
  }
  NSString* skipped = [defaults stringForKey:kSkippedVersionKey];
  const double skipUntil = [defaults doubleForKey:kSkipUntilKey];
  if (!mandatory && [skipped isEqualToString:version] && now_epoch() < skipUntil) {
    [self clearPending];
    finish(kResultSkipped);
    return;
  }

  [defaults setInteger:remoteBuild forKey:kPendingBuildKey];
  [defaults setObject:version ?: @"" forKey:kPendingVersionKey];
  [defaults setBool:mandatory forKey:kMandatoryKey];
  [defaults setObject:json_string(manifest, @"mandatory_reason") ?: @"" forKey:kPendingReasonKey];
  // 最晚安装时间仅用于界面提示；是否强制仍以 mandatory 为准（与 core 一致）。
  [defaults setObject:json_string(manifest, @"mandatory_deadline") ?: @""
               forKey:kPendingDeadlineKey];
  [defaults setObject:json_string(manifest, @"release_notes_url") ?: @"" forKey:kPendingNotesKey];
  // 预下载需要安装包地址、哈希与大小；持久化后用户确认流量提示时不必重新拉清单。
  [defaults setObject:json_string(manifest, @"url") ?: @"" forKey:kPendingURLKey];
  [defaults setObject:json_string(manifest, @"sha256") ?: @"" forKey:kPendingSHAKey];
  [defaults setInteger:json_long(manifest, @"size") forKey:kPendingSizeKey];
  xiaotu::log_info("更新：发现新版本 " + std::string(version.UTF8String ?: "") +
                   "（build " + std::to_string(remoteBuild) + "）");
  // 自动检查（completion == nil）命中新版本才自动预下载；手动检查只准备状态。
  [self preparePrefetchForManifest:manifest autoStart:(completion == nil)];
  finish(kResultUpdateAvailable);
#if XIAOTU_HAS_SPARKLE
  // 只触发检查。Q25 现行口径下 Info.plist 的 SUAutomaticallyUpdate 与
  // SUAllowsAutomaticUpdates 都是 false，Sparkle 不会自动下载或安装；
  // 下载与安装只能由用户在关于页显式点「下载并校验」「安装更新」触发。
  dispatch_async(dispatch_get_main_queue(), ^{
    [[[self sparkleController] updater] checkForUpdatesInBackground];
  });
#else
  xiaotu::log_warn("更新：当前构建未链接 Sparkle，已记录待更新版本，需手动下载安装");
#endif
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
