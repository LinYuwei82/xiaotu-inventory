#pragma once

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// macOS 更新桥（mote 模式薄端口，参考 mote/shell/mac/updater.mm）：
// 清单拉取、验签、判定、跳过/冷却/退避全在 core/update_engine；本类只提供
// NSURLSession 传输、NSUserDefaults 单键存储、24h±2h 定时器与 Sparkle 旁路
// （下载、appcast EdDSA 验签、.app 原子替换与重启全部交给 Sparkle）。
// 无回滚/恢复模式、无自研预下载（与 mote 对齐）。无 UI（更新卡片在 web 前端）。
@interface XiaotuUpdater : NSObject

+ (instancetype)shared;

// manifest_url 为空时更新功能关闭。health_base 用于 min_supported 握手。
- (void)startWithManifestURL:(nullable NSString*)manifestURL
                  healthBase:(nullable NSString*)healthBase
                     channel:(NSString*)channel;

// 以下桥方法结果字典见 docs/前端交接-更新模块.md；completion 主线程回调。

// 立即检查一次（绕过 24h 定时，遵守跳过）；手动检查不设冷却。
- (void)checkNowWithCompletion:(void (^)(NSDictionary<NSString*, id>* result))completion;

// 轻量握手：读取服务端 /api/health 的 min_supported_version 并与当前壳版本比较。
// 返回 {blocked: bool, minimum: string, current: string}；未配置时报 blocked=false。
// 供启动时展示“必须升级”阻塞页，不依赖 24h 定时检查。
- (void)checkMinSupportedVersionWithCompletion:
    (void (^)(NSDictionary<NSString*, id>* result))completion;

// 无待安装版本返回 NO_PENDING_UPDATE；从只读位置运行返回 TRANSLOCATED；
// 无 Sparkle（dev 构建）返回 UNAVAILABLE。成功返回 STARTED（Sparkle 接管）。
- (void)installUpdateWithCompletion:(void (^)(NSDictionary<NSString*, id>* result))completion;

// options 支持可选布尔 auto_check（关闭后手动检查仍可用）；其余键忽略。
// 结果 code 为 UPDATED；参数类型不对返回 INVALID_ARGUMENT。
- (void)setOptions:(nullable id)options
        completion:(void (^)(NSDictionary<NSString*, id>* result))completion;

- (NSDictionary<NSString*, id>*)status;

@end

NS_ASSUME_NONNULL_END
