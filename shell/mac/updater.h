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

// 供未来设置页使用：跳过指定版本若干天。
- (void)skipVersion:(NSString*)version days:(NSInteger)days;

- (NSDictionary<NSString*, id>*)status;

@end

NS_ASSUME_NONNULL_END
