#import <Cocoa/Cocoa.h>
#import <Security/Security.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <WebKit/WebKit.h>

#include <string>

#include "print_paper.h"
#include "updater.h"
#include "webview.h"

// 退出确认：桌面壳只在「有未保存内容」或「提交在途」时拦截，见下方实现。
static BOOL XiaotuConfirmExitIfDirty(void);

@interface XiaotuAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation XiaotuAppDelegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
  return YES;
}

// Cmd+Q 与窗口关闭是两条独立路径，必须都走同一个判断，只拦一条等于没拦。
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
  return XiaotuConfirmExitIfDirty() ? NSTerminateNow : NSTerminateCancel;
}
@end

// 前端通过 xiaotuState 推送「是否有未保存内容 / 提交是否在途」。
// windowShouldClose 与 applicationShouldTerminate 都必须同步返回，不能在这里等
// evaluateJavaScript 完成（那会死锁在主线程），所以只读前端最近一次推来的值。
static BOOL g_xiaotuDirty = NO;

// 有未保存内容才拦：平时点红叉或 Cmd+Q 直接退出，不弹无谓的确认。
// 按钮顺序固定为「取消 / 退出」：先加的是默认按钮，回车即取消。
static BOOL XiaotuConfirmExitIfDirty(void) {
  if (!g_xiaotuDirty) {
    return YES;
  }
  NSAlert* alert = [[NSAlert alloc] init];
  alert.alertStyle = NSAlertStyleWarning;
  alert.messageText = @"还有未保存的内容";
  alert.informativeText = @"现在退出会丢失当前录入或尚未提交的修改。";
  [alert addButtonWithTitle:@"取消"];
  [alert addButtonWithTitle:@"退出"];
  const BOOL quit = [alert runModal] == NSAlertSecondButtonReturn;
  if (quit) {
    // 已在退出路径上确认过：清标记，避免随后 applicationShouldTerminate 再弹一次。
    g_xiaotuDirty = NO;
  }
  return quit;
}

@interface XiaotuOpenPanelDelegate : NSObject <WKUIDelegate>
@end

// WKWebView 不会自己处理 <input type="file"> 的 click；必须由 UIDelegate
// 弹 NSOpenPanel，否则前端调用 fileInput.click() 完全没有反应。
// 主数据导入只接受服务端能解析的 .xlsx，这里也不放开其他类型。
@implementation XiaotuOpenPanelDelegate
- (void)webView:(WKWebView*)webView
    runOpenPanelWithParameters:(WKOpenPanelParameters*)parameters
             initiatedByFrame:(WKFrameInfo*)frame
            completionHandler:(void (^)(NSArray<NSURL*>* _Nullable URLs))completionHandler {
  (void)frame;
  NSOpenPanel* panel = [NSOpenPanel openPanel];
  panel.canChooseFiles = YES;
  panel.canChooseDirectories = NO;
  panel.allowsMultipleSelection = parameters.allowsMultipleSelection;
  UTType* xlsx = [UTType typeWithIdentifier:@"org.openxmlformats.spreadsheetml.sheet"];
  if (xlsx != nil) {
    panel.allowedContentTypes = @[ xlsx ];
  }
  panel.allowsOtherFileTypes = NO;
  NSURL* downloads = [[NSFileManager defaultManager] URLForDirectory:NSDownloadsDirectory
                                                            inDomain:NSUserDomainMask
                                                   appropriateForURL:nil
                                                              create:NO
                                                               error:NULL];
  if (downloads == nil) {
    downloads = [[NSFileManager defaultManager] URLForDirectory:NSDocumentDirectory
                                                       inDomain:NSUserDomainMask
                                              appropriateForURL:nil
                                                         create:NO
                                                          error:NULL];
  }
  if (downloads != nil) {
    panel.directoryURL = downloads;
  }
  void (^done)(NSModalResponse) = ^(NSModalResponse result) {
    if (result == NSModalResponseOK && panel.URLs.count > 0) {
      completionHandler(panel.URLs);
    } else {
      // 取消：把空结果交回 WebKit，前端不会触发 change，也就不会发导入请求。
      completionHandler(nil);
    }
  };
  if (webView.window != nil) {
    [panel beginSheetModalForWindow:webView.window completionHandler:done];
  } else {
    [panel beginWithCompletionHandler:done];
  }
}
@end

@interface XiaotuWindowDelegate : NSObject <NSWindowDelegate>
@end

// 单机版「本机免输」：钥匙串里只放服务端签发的随机 token，不存口令或口令派生值。
// token 的 30 天到期、时钟回拨与作废都在服务端判断，壳只负责存取。
static NSString* const kXiaotuKeychainService = @"com.simem.xiaotu.local-remember";
static NSString* const kXiaotuKeychainAccount = @"admin";

static NSMutableDictionary* XiaotuKeychainQuery(void) {
  return [@{
    (__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword,
    (__bridge id)kSecAttrService : kXiaotuKeychainService,
    (__bridge id)kSecAttrAccount : kXiaotuKeychainAccount,
  } mutableCopy];
}

static NSDictionary* XiaotuKeychainGet(void) {
  NSMutableDictionary* query = XiaotuKeychainQuery();
  query[(__bridge id)kSecReturnData] = @YES;
  query[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;
  CFTypeRef result = NULL;
  const OSStatus status = SecItemCopyMatching((__bridge CFDictionaryRef)query, &result);
  if (status == errSecItemNotFound) {
    return @{@"ok" : @YES, @"token" : [NSNull null]};
  }
  if (status != errSecSuccess) {
    return @{@"ok" : @NO,
             @"error" : [NSString stringWithFormat:@"钥匙串读取失败（%d）", (int)status]};
  }
  NSData* data = CFBridgingRelease(result);
  NSString* token = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
  if (token.length == 0) {
    return @{@"ok" : @YES, @"token" : [NSNull null]};
  }
  return @{@"ok" : @YES, @"token" : token};
}

static NSDictionary* XiaotuKeychainSet(NSString* token) {
  if (token.length == 0) {
    return @{@"ok" : @NO, @"error" : @"凭据为空"};
  }
  NSData* data = [token dataUsingEncoding:NSUTF8StringEncoding];
  NSMutableDictionary* query = XiaotuKeychainQuery();
  NSDictionary* update = @{(__bridge id)kSecValueData : data};
  OSStatus status =
      SecItemUpdate((__bridge CFDictionaryRef)query, (__bridge CFDictionaryRef)update);
  if (status == errSecItemNotFound) {
    NSMutableDictionary* add = XiaotuKeychainQuery();
    add[(__bridge id)kSecValueData] = data;
    status = SecItemAdd((__bridge CFDictionaryRef)add, NULL);
  }
  if (status != errSecSuccess) {
    return @{@"ok" : @NO,
             @"error" : [NSString stringWithFormat:@"钥匙串写入失败（%d）", (int)status]};
  }
  return @{@"ok" : @YES};
}

static NSDictionary* XiaotuKeychainDelete(void) {
  const OSStatus status = SecItemDelete((__bridge CFDictionaryRef)XiaotuKeychainQuery());
  if (status == errSecSuccess || status == errSecItemNotFound) {
    return @{@"ok" : @YES};
  }
  return @{@"ok" : @NO,
           @"error" : [NSString stringWithFormat:@"钥匙串删除失败（%d）", (int)status]};
}

// 使用 WKScriptMessageHandlerWithReply：JS 侧 postMessage 直接拿到 Promise 结果。
@interface XiaotuKeychainHandler : NSObject <WKScriptMessageHandlerWithReply>
@end

@implementation XiaotuKeychainHandler
- (void)userContentController:(WKUserContentController*)controller
      didReceiveScriptMessage:(WKScriptMessage*)message
                 replyHandler:(void (^)(id _Nullable, NSString* _Nullable))replyHandler {
  NSDictionary* body = [message.body isKindOfClass:[NSDictionary class]] ? message.body : @{};
  NSString* action = [body[@"action"] isKindOfClass:[NSString class]] ? body[@"action"] : @"";
  if ([action isEqualToString:@"get"]) {
    replyHandler(XiaotuKeychainGet(), nil);
    return;
  }
  if ([action isEqualToString:@"set"]) {
    NSString* token = [body[@"token"] isKindOfClass:[NSString class]] ? body[@"token"] : @"";
    replyHandler(XiaotuKeychainSet(token), nil);
    return;
  }
  if ([action isEqualToString:@"delete"]) {
    replyHandler(XiaotuKeychainDelete(), nil);
    return;
  }
  replyHandler(nil, @"未知的钥匙串操作");
}
@end

@implementation XiaotuWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
  return XiaotuConfirmExitIfDirty();
}
@end

// 前端导出与打印的桥：WKWebView 不处理 a[download] 与 window.print()，
// 这里用 NSSavePanel 保存 CSV、用 WKWebView 的打印操作走系统打印面板。
// 把模板纸张与边距落到 NSPrintInfo：PDF 的 MediaBox 与真实打印纸一致。
// 连续纸（针式）同样是精确尺寸 + 模板边距，不做额外缩放，保证套打位置不漂。
//
// paper == nil 只出现在 Cmd+P（没有模板参数）：沿用系统共享打印设置。
// paper 非 nil 但缺字段或数值非法时返回 NO，由调用方给出可见错误，不静默用 A4。
static BOOL XiaotuApplyPaper(NSPrintInfo* info, NSDictionary* paper, NSString** failure) {
  if (info == nil) {
    if (failure != NULL) *failure = @"打印初始化失败（NSPrintInfo 不可用）。";
    return NO;
  }
  if (paper == nil) {
    return YES;
  }
  if (![paper isKindOfClass:[NSDictionary class]]) {
    if (failure != NULL) *failure = @"打印参数格式错误，已取消打印。";
    return NO;
  }
  id width = paper[@"width_mm"];
  id height = paper[@"height_mm"];
  id margin = paper[@"margin_mm"];
  if (![width isKindOfClass:[NSNumber class]] || ![height isKindOfClass:[NSNumber class]] ||
      ![margin isKindOfClass:[NSNumber class]]) {
    if (failure != NULL) *failure = @"缺少纸型参数（宽 / 高 / 页边距），已取消打印。";
    return NO;
  }

  // 模板方向只从 layout.orientation 读，不从宽高推断；它随后由 plan_print_layout
  // 折算进最终宽高，这里只负责把方向原样带进纸型计算。
  const bool landscape = [paper[@"orientation"] isKindOfClass:[NSString class]] &&
                         [paper[@"orientation"] isEqualToString:@"LANDSCAPE"];

  xiaotu::mac_print::PaperSpec spec;
  xiaotu::mac_print::PaperFailure error;
  if (!xiaotu::mac_print::make_paper_spec([width doubleValue], [height doubleValue],
                                          [margin doubleValue], landscape, spec, error)) {
    if (failure != NULL) {
      *failure = [NSString stringWithUTF8String:error.message.c_str()];
    }
    return NO;
  }

  // plan_print_layout 已把模板方向算进最终宽高；NSPrintInfo 的 paperSize 按纵向记，
  // 页面方向另由 orientation 决定。这里只按最终页面翻译页框，不再回头读模板方向，
  // 否则 241×140 配 PORTRAIT 会被 AppKit 规范成 140×241（内容按 241 宽排版后被裁）。
  const bool wide_page = spec.width_pt > spec.height_pt;
  [info setPaperSize:wide_page ? NSMakeSize(spec.height_pt, spec.width_pt)
                               : NSMakeSize(spec.width_pt, spec.height_pt)];
  [info setLeftMargin:spec.margin_pt];
  [info setRightMargin:spec.margin_pt];
  [info setTopMargin:spec.margin_pt];
  [info setBottomMargin:spec.margin_pt];
  [info setOrientation:wide_page ? NSPaperOrientationLandscape
                                 : NSPaperOrientationPortrait];
  // 系统打印默认在纸张页眉打印日期与网页标题（document.title = 「小兔进销存」），
  // 与模板里的「打印时间」重复，且会挤占第一页顶部空间。显式关闭页眉页脚，
  // 让输出只包含模板内容；系统打印与「导出 PDF」共用这份 NSPrintInfo。
  // 模板自己的 company_name / header_text 属于业务抬头，不受此开关影响。
  [[info dictionary] setObject:@NO forKey:NSPrintHeaderAndFooter];
  // WKWebView 的打印排版按 90 dpi 换算 CSS 像素（1 CSS px = 0.8pt），而 CSS 规范
  // 是 96 dpi（1 px = 0.75pt），整页因此凭空大 96/90 ≈ 1.067 倍；这是排版 DPI
  // 的偏差，不是内容缩放，改字号或乘反向系数都只是盖住现象。
  // scalingFactor 是 AppKit 在这层排版结果上再乘一次（实测 0.5 → 字号减半），
  // 所以用它把 90 dpi 的排版压回 96 dpi 的真实物理尺寸。
  constexpr double kWebKitPrintLayoutPxPt = 0.8;  // 1 CSS px → 0.8pt（90 dpi）
  constexpr double kCssPxPt = 0.75;               // 1 CSS px → 0.75pt（96 dpi）
  [info setScalingFactor:kCssPxPt / kWebKitPrintLayoutPxPt];
  // 套打与自定义纸不接受系统居中，否则位置会随纸宽漂移。
  [info setHorizontallyCentered:NO];
  [info setVerticallyCentered:NO];
  return YES;
}

@interface XiaotuScriptHandler : NSObject <WKScriptMessageHandler, NSMenuItemValidation>
@property(nonatomic, weak) WKWebView* webview;
@property(nonatomic, weak) NSWindow* window;
- (void)printDocumentWithPaper:(NSDictionary*)paper;
- (void)savePdfWithFilename:(NSString*)filename paper:(NSDictionary*)paper;
- (void)presentPrintFailure:(NSString*)message;
@end

@implementation XiaotuScriptHandler
- (void)userContentController:(WKUserContentController*)controller
      didReceiveScriptMessage:(WKScriptMessage*)message {
  if ([message.name isEqualToString:@"xiaotuPrint"]) {
    NSDictionary* body = [message.body isKindOfClass:[NSDictionary class]] ? message.body : nil;
    NSDictionary* paper =
        [body[@"paper"] isKindOfClass:[NSDictionary class]] ? body[@"paper"] : nil;
    [self printDocumentWithPaper:paper];
    return;
  }
  if ([message.name isEqualToString:@"xiaotuPrintPdf"]) {
    NSDictionary* body = message.body;
    NSString* filename =
        [body isKindOfClass:[NSDictionary class]] && [body[@"filename"] isKindOfClass:[NSString class]]
            ? body[@"filename"]
            : @"export.pdf";
    NSDictionary* paper =
        [body isKindOfClass:[NSDictionary class]] && [body[@"paper"] isKindOfClass:[NSDictionary class]]
            ? body[@"paper"]
            : nil;
    [self savePdfWithFilename:filename paper:paper];
    return;
  }
  if ([message.name isEqualToString:@"xiaotuState"]) {
    NSDictionary* body = [message.body isKindOfClass:[NSDictionary class]] ? message.body : nil;
    g_xiaotuDirty = [body[@"dirty"] boolValue];
    return;
  }
  if ([message.name hasPrefix:@"xiaotuUpdate"]) {
    [self handleUpdateMessage:message];
    return;
  }
  if (![message.name isEqualToString:@"xiaotuExport"]) {
    return;
  }
  NSDictionary* body = message.body;
  if (![body isKindOfClass:[NSDictionary class]]) {
    return;
  }
  NSString* filename = [body[@"filename"] isKindOfClass:[NSString class]] ? body[@"filename"]
                                                                        : @"report.csv";
  NSString* content = [body[@"content"] isKindOfClass:[NSString class]] ? body[@"content"] : @"";
  NSString* content_base64 =
      [body[@"contentBase64"] isKindOfClass:[NSString class]] ? body[@"contentBase64"] : nil;
  NSData* binary = content_base64 != nil
                       ? [[NSData alloc] initWithBase64EncodedString:content_base64 options:0]
                       : nil;
  NSSavePanel* panel = [NSSavePanel savePanel];
  panel.nameFieldStringValue = filename;
  panel.canCreateDirectories = YES;
  [panel beginSheetModalForWindow:self.window
                completionHandler:^(NSModalResponse result) {
                  if (result != NSModalResponseOK || panel.URL == nil) {
                    return;
                  }
                  if (binary != nil) {
                    [binary writeToURL:panel.URL atomically:YES];
                    return;
                  }
                  [content writeToURL:panel.URL
                           atomically:YES
                             encoding:NSUTF8StringEncoding
                                error:NULL];
                }];
}

// Cmd+P：没有模板参数时用系统共享打印设置走同一条打印链路，不弹自定义错误。
- (void)printDocument:(id)sender {
  [self printDocumentWithPaper:nil];
}

- (BOOL)validateMenuItem:(NSMenuItem*)menuItem {
  if (menuItem.action == @selector(printDocument:)) {
    return self.webview != nil;
  }
  return YES;
}

- (void)presentPrintFailure:(NSString*)message {
  NSAlert* alert = [[NSAlert alloc] init];
  alert.messageText = @"打印无法开始";
  alert.informativeText = message.length > 0 ? message : @"打印参数无效。";
  [alert addButtonWithTitle:@"好"];
  if (self.window != nil) {
    [alert beginSheetModalForWindow:self.window completionHandler:nil];
  } else {
    [alert runModal];
  }
}

// 更新桥：四个方法都通过 evaluateJavaScript 异步回调，前端封装为 Promise。
// 契约见 docs/前端交接-更新模块.md 第 2 节；原生侧不生成任何界面。
- (void)callUpdateCallback:(NSString*)callback payload:(NSDictionary*)payload {
  NSData* data = [NSJSONSerialization dataWithJSONObject:payload ?: @{} options:0 error:nil];
  NSString* json = data != nil ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding]
                               : @"null";
  NSString* script =
      [NSString stringWithFormat:@"window.%@ && window.%@(%@);", callback, callback, json];
  dispatch_async(dispatch_get_main_queue(), ^{
    [self.webview evaluateJavaScript:script completionHandler:nil];
  });
}

- (void)handleUpdateMessage:(WKScriptMessage*)message {
  NSDictionary* body = [message.body isKindOfClass:[NSDictionary class]] ? message.body : @{};
  if ([message.name isEqualToString:@"xiaotuUpdateCheckNow"]) {
    [[XiaotuUpdater shared] checkNowWithCompletion:^(NSDictionary* result) {
      [self callUpdateCallback:@"xiaotuUpdateCheckDone" payload:result];
    }];
    return;
  }
  if ([message.name isEqualToString:@"xiaotuUpdateStatus"]) {
    [self callUpdateCallback:@"xiaotuUpdateStatusDone" payload:[[XiaotuUpdater shared] status]];
    return;
  }
  if ([message.name isEqualToString:@"xiaotuUpdateSkip"]) {
    [[XiaotuUpdater shared] skipVersion:body[@"version"]
                                   days:body[@"days"]
                             completion:^(NSDictionary* result) {
                               [self callUpdateCallback:@"xiaotuUpdateSkipDone" payload:result];
                             }];
    return;
  }
  if ([message.name isEqualToString:@"xiaotuUpdateInstall"]) {
    [[XiaotuUpdater shared] installUpdateWithCompletion:^(NSDictionary* result) {
      [self callUpdateCallback:@"xiaotuUpdateInstallDone" payload:result];
    }];
    return;
  }
  if ([message.name isEqualToString:@"xiaotuUpdateResume"]) {
    [[XiaotuUpdater shared] resumeWithCompletion:^(NSDictionary* result) {
      [self callUpdateCallback:@"xiaotuUpdateResumeDone" payload:result];
    }];
    return;
  }
  if ([message.name isEqualToString:@"xiaotuUpdateOptions"]) {
    [[XiaotuUpdater shared] setOptions:body
                            completion:^(NSDictionary* result) {
                              [self callUpdateCallback:@"xiaotuUpdateOptionsDone" payload:result];
                            }];
    return;
  }
  if ([message.name isEqualToString:@"xiaotuUpdateDownload"]) {
    [[XiaotuUpdater shared] downloadUpdateWithCompletion:^(NSDictionary* result) {
      [self callUpdateCallback:@"xiaotuUpdateDownloadDone" payload:result];
    }];
    return;
  }
  if ([message.name isEqualToString:@"xiaotuUpdateCancelPrefetch"]) {
    [[XiaotuUpdater shared] cancelPrefetchWithCompletion:^(NSDictionary* result) {
      [self callUpdateCallback:@"xiaotuUpdateCancelDone" payload:result];
    }];
    return;
  }
}

- (void)printDocumentWithPaper:(NSDictionary*)paper {
  if (self.webview == nil) {
    [self presentPrintFailure:@"页面尚未加载完成，请稍后重试。"];
    return;
  }
  // 复制一份共享设置，避免本次模板的纸型 / 边距留在进程级单例里影响后续打印。
  NSPrintInfo* info = [[NSPrintInfo sharedPrintInfo] copy];
  NSString* failure = nil;
  if (!XiaotuApplyPaper(info, paper, &failure)) {
    [self presentPrintFailure:failure];
    return;
  }
  NSPrintOperation* operation = [self.webview printOperationWithPrintInfo:info];
  if (operation == nil) {
    [self presentPrintFailure:@"系统未能创建打印任务。"];
    return;
  }
  [operation runOperation];
}

// 不弹系统打印面板：用同一条打印链路把页面写成 PDF 文件。
// 前端在调用前已经挂好打印视图，完成或取消后由 JS 负责清理。
- (void)finishPdfWithResult:(NSString*)result {
  NSString* script = [NSString
      stringWithFormat:@"window.xiaotuPdfDone && window.xiaotuPdfDone('%@');", result];
  [self.webview evaluateJavaScript:script completionHandler:nil];
}

- (void)savePdfWithFilename:(NSString*)filename paper:(NSDictionary*)paper {
  if (self.webview == nil) {
    return;
  }
  NSSavePanel* panel = [NSSavePanel savePanel];
  panel.nameFieldStringValue = filename.length > 0 ? filename : @"export.pdf";
  panel.canCreateDirectories = YES;
  __weak XiaotuScriptHandler* weak_self = self;
  [panel beginSheetModalForWindow:self.window
                completionHandler:^(NSModalResponse result) {
                  XiaotuScriptHandler* strong_self = weak_self;
                  if (strong_self == nil) {
                    return;
                  }
                  if (result != NSModalResponseOK || panel.URL == nil) {
                    [strong_self finishPdfWithResult:@"cancel"];
                    return;
                  }
                  NSPrintInfo* info = [[NSPrintInfo sharedPrintInfo] copy];
                  NSString* failure = nil;
                  if (!XiaotuApplyPaper(info, paper, &failure)) {
                    // 保存面板已关闭，错误用弹窗告知；同时让前端恢复按钮状态。
                    [strong_self presentPrintFailure:failure];
                    [strong_self finishPdfWithResult:@"error"];
                    return;
                  }
                  [info setJobDisposition:NSPrintSaveJob];
                  [[info dictionary] setObject:panel.URL forKey:NSPrintJobSavingURL];
                  NSPrintOperation* operation =
                      [strong_self.webview printOperationWithPrintInfo:info];
                  if (operation == nil) {
                    [strong_self finishPdfWithResult:@"error"];
                    return;
                  }
                  [operation setShowsPrintPanel:NO];
                  [operation setShowsProgressPanel:NO];
                  // WKWebView 的分页是异步的：页数与每页内容要等 WebKit 进程回报。
                  // runOperation 起的是不依附窗口的模态会话，不会为打印视图泵这些
                  // 回调，分页没完成就开始出纸，产出的是一叠空白页（本机实测无限页）。
                  // 挂到窗口的模态会话后由窗口的 run loop 驱动 WebKit 回调，分页完成
                  // 再落盘（实测 1 页有效 PDF），所以这里必须要求有窗口。
                  [strong_self runPdfOperation:operation];
                }];
}

- (void)runPdfOperation:(NSPrintOperation*)operation {
  if (self.window == nil) {
    [self finishPdfWithResult:@"error"];
    return;
  }
  [operation runOperationModalForWindow:self.window
                               delegate:self
                         didRunSelector:@selector(pdfOperationDidRun:success:contextInfo:)
                            contextInfo:NULL];
}

- (void)pdfOperationDidRun:(NSPrintOperation*)operation
                   success:(BOOL)success
               contextInfo:(void*)context {
  [self finishPdfWithResult:success ? @"ok" : @"error"];
}
@end

namespace xiaotu::platform {

int run_webview(const std::string& url, const std::string& title) {
  @autoreleasepool {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    XiaotuAppDelegate* delegate = [[XiaotuAppDelegate alloc] init];
    [NSApp setDelegate:delegate];

    NSString* app_name = [NSString stringWithUTF8String:title.c_str()];
    NSMenu* menubar = [[NSMenu alloc] init];
    NSMenuItem* app_item = [[NSMenuItem alloc] init];
    [menubar addItem:app_item];
    [NSApp setMainMenu:menubar];

    NSMenu* app_menu = [[NSMenu alloc] init];
    [app_menu addItemWithTitle:[@"关于 " stringByAppendingString:app_name]
                        action:@selector(orderFrontStandardAboutPanel:)
                 keyEquivalent:@""];
    [app_menu addItem:[NSMenuItem separatorItem]];
    [app_menu addItemWithTitle:[@"隐藏 " stringByAppendingString:app_name]
                        action:@selector(hide:)
                 keyEquivalent:@"h"];
    [app_menu addItem:[NSMenuItem separatorItem]];
    [app_menu addItemWithTitle:[@"退出 " stringByAppendingString:app_name]
                        action:@selector(terminate:)
                 keyEquivalent:@"q"];
    [app_item setSubmenu:app_menu];

    XiaotuScriptHandler* script_handler = [[XiaotuScriptHandler alloc] init];

    NSMenuItem* file_item = [[NSMenuItem alloc] init];
    [menubar addItem:file_item];
    NSMenu* file_menu = [[NSMenu alloc] initWithTitle:@"文件"];
    NSMenuItem* print_item = [[NSMenuItem alloc] initWithTitle:@"打印…"
                                                       action:@selector(printDocument:)
                                                keyEquivalent:@"p"];
    [print_item setTarget:script_handler];
    [file_menu addItem:print_item];
    [file_item setSubmenu:file_menu];

    // 编辑菜单：target 留空，交给响应者链，焦点在 WKWebView 输入框时也能生效。
    NSMenuItem* edit_item = [[NSMenuItem alloc] init];
    [menubar addItem:edit_item];
    NSMenu* edit_menu = [[NSMenu alloc] initWithTitle:@"编辑"];
    [edit_menu addItemWithTitle:@"撤销" action:@selector(undo:) keyEquivalent:@"z"];
    [edit_menu addItemWithTitle:@"重做" action:@selector(redo:) keyEquivalent:@"Z"];
    [edit_menu addItem:[NSMenuItem separatorItem]];
    [edit_menu addItemWithTitle:@"剪切" action:@selector(cut:) keyEquivalent:@"x"];
    [edit_menu addItemWithTitle:@"复制" action:@selector(copy:) keyEquivalent:@"c"];
    [edit_menu addItemWithTitle:@"粘贴" action:@selector(paste:) keyEquivalent:@"v"];
    [edit_menu addItemWithTitle:@"全选" action:@selector(selectAll:) keyEquivalent:@"a"];
    [edit_item setSubmenu:edit_menu];

    NSMenuItem* window_item = [[NSMenuItem alloc] init];
    [menubar addItem:window_item];
    NSMenu* window_menu = [[NSMenu alloc] initWithTitle:@"窗口"];
    [window_menu addItemWithTitle:@"最小化"
                           action:@selector(performMiniaturize:)
                    keyEquivalent:@"m"];
    [window_menu addItemWithTitle:@"关闭"
                           action:@selector(performClose:)
                    keyEquivalent:@"w"];
    [window_item setSubmenu:window_menu];
    [NSApp setWindowsMenu:window_menu];

    const NSRect frame = NSMakeRect(0, 0, 1280, 840);
    NSWindow* window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [window setTitle:app_name];
    [window setMinSize:NSMakeSize(960, 600)];
    // NSWindow.delegate 是 weak，必须自己持有；窗口关闭前由它决定放行还是弹确认。
    static XiaotuWindowDelegate* g_windowDelegate = nil;
    if (g_windowDelegate == nil) {
      g_windowDelegate = [[XiaotuWindowDelegate alloc] init];
    }
    [window setDelegate:g_windowDelegate];

    WKWebViewConfiguration* configuration = [[WKWebViewConfiguration alloc] init];
    WKUserContentController* controller = [[WKUserContentController alloc] init];
    [controller addScriptMessageHandler:script_handler name:@"xiaotuState"];
    [controller addScriptMessageHandler:script_handler name:@"xiaotuExport"];
    [controller addScriptMessageHandler:script_handler name:@"xiaotuPrint"];
    [controller addScriptMessageHandler:script_handler name:@"xiaotuPrintPdf"];
    [controller addScriptMessageHandler:script_handler name:@"xiaotuUpdateCheckNow"];
    [controller addScriptMessageHandler:script_handler name:@"xiaotuUpdateStatus"];
    [controller addScriptMessageHandler:script_handler name:@"xiaotuUpdateSkip"];
    [controller addScriptMessageHandler:script_handler name:@"xiaotuUpdateInstall"];
    [controller addScriptMessageHandler:script_handler name:@"xiaotuUpdateResume"];
    [controller addScriptMessageHandler:script_handler name:@"xiaotuUpdateOptions"];
    [controller addScriptMessageHandler:script_handler name:@"xiaotuUpdateDownload"];
    [controller addScriptMessageHandler:script_handler name:@"xiaotuUpdateCancelPrefetch"];
    static XiaotuKeychainHandler* g_keychainHandler = nil;
    if (g_keychainHandler == nil) {
      g_keychainHandler = [[XiaotuKeychainHandler alloc] init];
    }
    [controller addScriptMessageHandlerWithReply:g_keychainHandler
                                    contentWorld:[WKContentWorld pageWorld]
                                            name:@"xiaotuKeychain"];
    configuration.userContentController = controller;
    WKWebView* webview = [[WKWebView alloc] initWithFrame:frame configuration:configuration];
    // UIDelegate 是 weak，静态变量负责持有；没有它，导入按钮的 fileInput.click() 不会弹面板。
    static XiaotuOpenPanelDelegate* g_openPanelDelegate = nil;
    if (g_openPanelDelegate == nil) {
      g_openPanelDelegate = [[XiaotuOpenPanelDelegate alloc] init];
    }
    [webview setUIDelegate:g_openPanelDelegate];
    [webview setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];
    [window setContentView:webview];
    script_handler.webview = webview;
    script_handler.window = window;
    [window center];
    [window makeKeyAndOrderFront:nil];

    NSURL* nsurl = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
    [webview loadRequest:[NSURLRequest requestWithURL:nsurl]];

    // 更新预检查：清单地址与通道来自 Info.plist；health 只用于读取本地 schema。
    [[XiaotuUpdater shared] noteLaunch];
    NSString* manifest_url =
        [[NSBundle mainBundle] objectForInfoDictionaryKey:@"XiaotuUpdateManifestURL"];
    NSString* update_channel =
        [[NSBundle mainBundle] objectForInfoDictionaryKey:@"XiaotuUpdateChannel"];
    NSString* health_base = nil;
    if (nsurl.scheme.length > 0 && nsurl.host.length > 0) {
      health_base = [NSString stringWithFormat:@"%@://%@%@", nsurl.scheme, nsurl.host,
                                               nsurl.port != nil
                                                   ? [@":" stringByAppendingString:
                                                                   nsurl.port.stringValue]
                                                   : @""];
    }
    [[XiaotuUpdater shared] startWithManifestURL:manifest_url
                                     healthBase:health_base
                                        channel:update_channel ?: @"stable"];

    [NSApp activateIgnoringOtherApps:YES];
    [NSApp run];
  }
  return 0;
}

}  // namespace xiaotu::platform
