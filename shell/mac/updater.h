#pragma once

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// 自研更新预检查：负责 24h±2h 调度、清单获取、跳过与失败计数；
// 下载与安装交给 Sparkle（可用时）。无 UI，状态通过 status 暴露给后续界面。
@interface XiaotuUpdater : NSObject

+ (instancetype)shared;

// manifest_url 为空时更新功能关闭。health_base 用于读取本机 schema 版本。
- (void)startWithManifestURL:(nullable NSString*)manifestURL
                   healthBase:(nullable NSString*)healthBase
                      channel:(NSString*)channel;

// 启动时调用：累计连续启动失败次数；成功运行 30 秒后清零。
- (void)noteLaunch;

// 以下四个桥方法的结果字典字段见 docs/前端交接-更新模块.md 第 2、3 节。
// completion 一律在主线程异步回调，不存在同步返回值。

// 立即检查一次（绕过 24h 定时，遵守暂停与跳过）；10 分钟冷却内返回 COOLDOWN。
- (void)checkNowWithCompletion:(void (^)(NSDictionary<NSString*, id>* result))completion;

// version 必须是字符串且非空；days 可为 nil（默认 7）或 1–30 的整数。
// 参数非法返回 INVALID_ARGUMENT，强制更新返回 MANDATORY_UPDATE。
- (void)skipVersion:(nullable id)version
               days:(nullable id)days
         completion:(void (^)(NSDictionary<NSString*, id>* result))completion;

// 无 Sparkle 返回 UNAVAILABLE，无待安装版本返回 NO_PENDING_UPDATE。
- (void)installUpdateWithCompletion:(void (^)(NSDictionary<NSString*, id>* result))completion;

// 清除“连续启动失败”的暂停标记与失败计数，并立即恢复一次自动检查。
// 未暂停时返回 NOT_PAUSED（幂等，不视为错误）。
- (void)resumeWithCompletion:(void (^)(NSDictionary<NSString*, id>* result))completion;

- (NSDictionary<NSString*, id>*)status;

@end

NS_ASSUME_NONNULL_END
