#import "updater.h"

#import <Foundation/Foundation.h>

#include <cstdlib>
#include <string>

#include "xiaotu/log.h"

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
NSString* const kPausedKey = @"XiaotuUpdatePaused";

constexpr NSTimeInterval kCheckInterval = 86400;  // 24h
constexpr NSTimeInterval kJitter = 7200;          // ±2h
constexpr NSTimeInterval kRequestTimeout = 10;
constexpr int kMaxLaunchFailures = 3;

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
}  // namespace

@interface XiaotuUpdater ()
@property(nonatomic, strong) dispatch_queue_t queue;
@property(nonatomic, copy, nullable) NSString* manifestURL;
@property(nonatomic, copy, nullable) NSString* healthBase;
@property(nonatomic, copy) NSString* channel;
@property(nonatomic, assign) BOOL running;
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
  // 注意：Info.plist 中 SUEnableAutomaticChecks=true 时 Sparkle 也会按
  // SUScheduledCheckInterval 自行检查，与本类形成双份检查/提示；当前未改行为，
  // 建议发布前只保留一侧（见 docs/更新模块技术方案.md 第 9 节）。
}

- (void)noteLaunch {
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  NSInteger attempts = [defaults integerForKey:kLaunchAttemptsKey] + 1;
  [defaults setInteger:attempts forKey:kLaunchAttemptsKey];
  long long pending = [defaults objectForKey:kPendingBuildKey] != nil
                          ? [defaults integerForKey:kPendingBuildKey]
                          : 0;
  const long long current = static_cast<long long>(XIAOTU_BUILD);
  if (attempts >= kMaxLaunchFailures && pending != 0 && pending != current) {
    [defaults setBool:YES forKey:kPausedKey];
    xiaotu::log_error("更新：连续 " + std::to_string(attempts) +
                      " 次启动未进入稳定状态，已暂停自动更新，建议回退上一版本");
  }
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC), self.queue, ^{
    [[NSUserDefaults standardUserDefaults] setInteger:0 forKey:kLaunchAttemptsKey];
    [[NSUserDefaults standardUserDefaults] setBool:NO forKey:kPausedKey];
  });
}

- (void)skipVersion:(NSString*)version days:(NSInteger)days {
  if (version.length == 0) {
    return;
  }
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  [defaults setObject:version forKey:kSkippedVersionKey];
  [defaults setDouble:[[NSDate date] timeIntervalSince1970] + days * 86400 forKey:kSkipUntilKey];
}

- (NSDictionary<NSString*, id>*)status {
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  return @{
    @"enabled" : @(self.manifestURL.length > 0),
    @"channel" : self.channel ?: @"stable",
    @"next_check_at" : @([defaults doubleForKey:kNextCheckKey]),
    @"launch_attempts" : @([defaults integerForKey:kLaunchAttemptsKey]),
    @"paused" : @([defaults boolForKey:kPausedKey]),
    @"skipped_version" : [defaults stringForKey:kSkippedVersionKey] ?: [NSNull null],
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
  const double next = [[NSDate date] timeIntervalSince1970] + kCheckInterval + jitter;
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
  const double now = [[NSDate date] timeIntervalSince1970];
  if (now < [defaults doubleForKey:kNextCheckKey]) {
    return;
  }
  [self scheduleNextCheck];

  NSDictionary* manifest = fetch_json([NSURL URLWithString:self.manifestURL]);
  if (manifest == nil) {
    xiaotu::log_warn("更新：清单获取失败，按下一周期重试");
    return;
  }
  NSString* platform = manifest[@"platform"];
  NSString* channel = manifest[@"channel"];
  if (![platform isEqualToString:@"macos"] || ![channel isEqualToString:self.channel]) {
    return;
  }
  const long long remoteBuild = json_long(manifest, @"build");
  if (remoteBuild <= static_cast<long long>(XIAOTU_BUILD)) {
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
    xiaotu::log_warn("更新：无法读取本地 schema，暂不提示更新");
    return;
  }
  if (localSchema < schemaMin || localSchema > schemaMax) {
    xiaotu::log_warn("更新：目标版本与当前数据版本不兼容，已跳过");
    return;
  }

  const bool mandatory = json_bool(manifest, @"mandatory");
  NSString* version = manifest[@"version"];
  NSString* skipped = [defaults stringForKey:kSkippedVersionKey];
  const double skipUntil = [defaults doubleForKey:kSkipUntilKey];
  if (!mandatory && [skipped isEqualToString:version] && now < skipUntil) {
    return;
  }

  [defaults setInteger:remoteBuild forKey:kPendingBuildKey];
  xiaotu::log_info("更新：发现新版本 " + std::string(version.UTF8String) +
                   "（build " + std::to_string(remoteBuild) + "）");
#if XIAOTU_HAS_SPARKLE
  dispatch_async(dispatch_get_main_queue(), ^{
    SPUStandardUpdaterController* controller =
        [[SPUStandardUpdaterController alloc] initWithStartingUpdater:YES
                                                    updaterDelegate:nil
                                                 userDriverDelegate:nil];
    [controller checkForUpdatesInBackground];
    (void)controller;
  });
#else
  xiaotu::log_warn("更新：当前构建未链接 Sparkle，已记录待更新版本，需手动下载安装");
#endif
}

@end
