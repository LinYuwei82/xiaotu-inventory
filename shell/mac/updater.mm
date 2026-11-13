#import "updater.h"

#import <Foundation/Foundation.h>

#include <cstdlib>
#include <string>

#include "xiaotu/log.h"
#include "xiaotu/update.h"

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
NSString* const kPendingNotesKey = @"XiaotuPendingNotes";
NSString* const kMandatoryKey = @"XiaotuPendingMandatory";
NSString* const kPausedKey = @"XiaotuUpdatePaused";
NSString* const kLastCheckKey = @"XiaotuLastCheckAt";
NSString* const kLastManualCheckKey = @"XiaotuLastManualCheckAt";
NSString* const kLastResultKey = @"XiaotuLastCheckResult";

constexpr NSTimeInterval kCheckInterval = 86400;  // 24h
constexpr NSTimeInterval kJitter = 7200;          // ±2h
constexpr NSTimeInterval kRequestTimeout = 10;
constexpr NSTimeInterval kManualCooldown = 600;  // 10min
constexpr int kMaxLaunchFailures = 3;

// status.last_result 与 checkNow 的 code 取值，和交接单第 2、3 节保持一致。
NSString* const kResultCheckFailed = @"CHECK_FAILED";
NSString* const kResultNotConfigured = @"NOT_CONFIGURED";
NSString* const kResultPaused = @"PAUSED";
NSString* const kResultNoUpdate = @"NO_UPDATE";
NSString* const kResultUpdateAvailable = @"UPDATE_AVAILABLE";
NSString* const kResultSkipped = @"SKIPPED";
NSString* const kResultIncompatible = @"INCOMPATIBLE_SCHEMA";

NSDictionary* fetch_json(NSURL* url) {
  __block NSDictionary* result = nil;
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
                if (error == nil && data != nil) {
                  NSDictionary* parsed = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
                  if ([parsed isKindOfClass:[NSDictionary class]]) {
                    result = parsed;
                  }
                }
                dispatch_semaphore_signal(done);
              }] resume];
  dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW,
                                              static_cast<int64_t>((kRequestTimeout + 1) * NSEC_PER_SEC)));
  [session invalidateAndCancel];
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

std::int64_t pending_build(NSUserDefaults* defaults) {
  return [defaults objectForKey:kPendingBuildKey] != nil
             ? static_cast<std::int64_t>([defaults integerForKey:kPendingBuildKey])
             : 0;
}
}  // namespace

@interface XiaotuUpdater ()
@property(nonatomic, strong) dispatch_queue_t queue;
@property(nonatomic, copy, nullable) NSString* manifestURL;
@property(nonatomic, copy, nullable) NSString* healthBase;
@property(nonatomic, copy) NSString* channel;
@property(nonatomic, assign) BOOL running;
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
  }
  return self;
}

- (void)startWithManifestURL:(NSString*)manifestURL
                  healthBase:(NSString*)healthBase
                     channel:(NSString*)channel {
  if (manifestURL.length == 0) {
    xiaotu::log_info("更新：未配置清单地址，自动检查关闭");
    return;
  }
  @synchronized(self) {
    if (self.running) {
      return;
    }
    self.running = YES;
    self.manifestURL = manifestURL;
    self.healthBase = healthBase;
    self.channel = channel.length > 0 ? channel : @"stable";
  }
  dispatch_async(self.queue, ^{
    [self scheduleNextCheckIfNeeded];
    [self checkIfDue];
  });
  // Info.plist 中 SUEnableAutomaticChecks=false：定时检查只由本类负责，
  // 避免 Sparkle 与本类形成双份检查/提示（见 docs/更新模块技术方案.md 第 9 节）。
}

- (void)noteLaunch {
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  NSInteger attempts = [defaults integerForKey:kLaunchAttemptsKey] + 1;
  [defaults setInteger:attempts forKey:kLaunchAttemptsKey];
  const std::int64_t pending = pending_build(defaults);
  if (attempts >= kMaxLaunchFailures && pending != 0 && pending != current_build()) {
    [defaults setBool:YES forKey:kPausedKey];
    xiaotu::log_error("更新：连续 " + std::to_string(attempts) +
                      " 次启动未进入稳定状态，已暂停自动更新，建议回退上一版本");
  }
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC), self.queue, ^{
    [[NSUserDefaults standardUserDefaults] setInteger:0 forKey:kLaunchAttemptsKey];
    [[NSUserDefaults standardUserDefaults] setBool:NO forKey:kPausedKey];
  });
}

// 清除待更新记录；不触碰跳过版本与定时。
- (void)clearPending {
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  [defaults removeObjectForKey:kPendingBuildKey];
  [defaults removeObjectForKey:kPendingVersionKey];
  [defaults removeObjectForKey:kPendingReasonKey];
  [defaults removeObjectForKey:kPendingNotesKey];
  [defaults setBool:NO forKey:kMandatoryKey];
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
#else
  const bool installerAvailable = false;
#endif
  switch (xiaotu::decide_install(installerAvailable, pending_build(defaults), current_build())) {
    case xiaotu::InstallDecision::kUnavailable:
      respond(@{
        @"ok" : @NO,
        @"code" : @"UNAVAILABLE",
        @"message" : @"当前构建未链接 Sparkle，请手动下载安装",
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
#if XIAOTU_HAS_SPARKLE
  // 脚本消息在主线程派发；控制器也只在主线程创建。
  [[self sparkleController] checkForUpdates:nil];
  respond(@{@"ok" : @YES, @"code" : @"STARTED"});
#else
  respond(@{@"ok" : @NO, @"code" : @"UNAVAILABLE"});
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

- (NSDictionary<NSString*, id>*)status {
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  const std::int64_t pending = pending_build(defaults);
  const BOOL updateAvailable = pending > current_build();
  NSString* pendingVersion = updateAvailable ? [defaults stringForKey:kPendingVersionKey] : nil;
  const BOOL mandatory = updateAvailable && [defaults boolForKey:kMandatoryKey];
  NSString* reason = mandatory ? [defaults stringForKey:kPendingReasonKey] : nil;
  NSString* notes = updateAvailable ? [defaults stringForKey:kPendingNotesKey] : nil;
  id reasonValue = reason.length > 0 ? reason : [NSNull null];
  id notesValue = notes.length > 0 ? notes : [NSNull null];
  return @{
    @"enabled" : @(self.manifestURL.length > 0),
    @"channel" : self.channel ?: @"stable",
    @"next_check_at" : @([defaults doubleForKey:kNextCheckKey]),
    @"launch_attempts" : @([defaults integerForKey:kLaunchAttemptsKey]),
    @"paused" : @([defaults boolForKey:kPausedKey]),
    @"skipped_version" : [defaults stringForKey:kSkippedVersionKey] ?: [NSNull null],
    @"skip_until" : @([defaults doubleForKey:kSkipUntilKey]),
    @"last_check_at" : @([defaults doubleForKey:kLastCheckKey]),
    @"last_result" : [defaults stringForKey:kLastResultKey] ?: [NSNull null],
    @"pending_version" : pendingVersion ?: [NSNull null],
    @"pending_build" : @(updateAvailable ? pending : 0),
    @"update_available" : @(updateAvailable),
    @"mandatory" : @(mandatory),
    @"reason" : reasonValue,
    @"release_notes_url" : notesValue,
    // 原生只处理静态清单，不解析 upgrade_path；中间版本由网络版接口给出。
    @"intermediate" : [NSNull null],
  };
}

- (void)scheduleNextCheckIfNeeded {
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  if ([defaults doubleForKey:kNextCheckKey] <= 0) {
    [self scheduleNextCheck];
  }
}

- (void)scheduleNextCheck {
  const double jitter = (arc4random_uniform(static_cast<uint32_t>(kJitter * 2 + 1)) - kJitter);
  const double next = now_epoch() + kCheckInterval + jitter;
  [[NSUserDefaults standardUserDefaults] setDouble:next forKey:kNextCheckKey];
}

- (void)checkIfDue {
  if (!self.running || self.manifestURL.length == 0) {
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
    if (completion != nil) {
      dispatch_async(dispatch_get_main_queue(), ^{
        completion(code);
      });
    }
  };

  NSDictionary* manifest = fetch_json([NSURL URLWithString:manifestURL]);
  if (manifest == nil) {
    xiaotu::log_warn("更新：清单获取失败，按下一周期重试");
    finish(kResultCheckFailed);
    return;
  }
  NSString* platform = manifest[@"platform"];
  NSString* channel = manifest[@"channel"];
  if (![platform isEqualToString:@"macos"] || ![channel isEqualToString:self.channel]) {
    xiaotu::log_warn("更新：清单的平台或通道与当前构建不匹配，本次不提示");
    [self clearPending];
    finish(kResultNoUpdate);
    return;
  }
  const std::int64_t remoteBuild = json_long(manifest, @"build");
  if (remoteBuild <= current_build()) {
    [self clearPending];
    finish(kResultNoUpdate);
    return;
  }

  int localSchema = 0;
  if (self.healthBase.length > 0) {
    NSDictionary* health = fetch_json([NSURL URLWithString:
                                             [self.healthBase stringByAppendingString:@"/api/health"]]);
    localSchema = static_cast<int>(json_long(health, @"schema_version"));
  }
  const long long schemaMin = json_long(manifest, @"schema_min");
  const long long schemaMax = json_long(manifest, @"schema_max");
  if (localSchema <= 0) {
    xiaotu::log_warn("更新：无法读取本地 schema，本次检查按失败处理");
    finish(kResultCheckFailed);
    return;
  }
  if (localSchema < schemaMin || localSchema > schemaMax) {
    xiaotu::log_warn("更新：目标版本与当前数据版本不兼容，已跳过");
    [self clearPending];
    finish(kResultIncompatible);
    return;
  }

  const bool mandatory = json_bool(manifest, @"mandatory");
  NSString* version = json_string(manifest, @"version");
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
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
  [defaults setObject:json_string(manifest, @"release_notes_url") ?: @"" forKey:kPendingNotesKey];
  xiaotu::log_info("更新：发现新版本 " + std::string(version.UTF8String ?: "") +
                   "（build " + std::to_string(remoteBuild) + "）");
  finish(kResultUpdateAvailable);
#if XIAOTU_HAS_SPARKLE
  dispatch_async(dispatch_get_main_queue(), ^{
    [[self sparkleController] checkForUpdatesInBackground];
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
                                                    updaterDelegate:nil
                                                 userDriverDelegate:nil];
  }
  return _sparkleController;
}
#endif

@end
