#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

#include <string>

#include "webview.h"

@interface XiaotuAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation XiaotuAppDelegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
  return YES;
}
@end

// 前端导出与打印的桥：WKWebView 不处理 a[download] 与 window.print()，
// 这里用 NSSavePanel 保存 CSV、用 WKWebView 的打印操作走系统打印面板。
@interface XiaotuScriptHandler : NSObject <WKScriptMessageHandler>
@property(nonatomic, weak) WKWebView* webview;
@property(nonatomic, weak) NSWindow* window;
- (void)printDocument:(id)sender;
@end

@implementation XiaotuScriptHandler
- (void)userContentController:(WKUserContentController*)controller
      didReceiveScriptMessage:(WKScriptMessage*)message {
  if ([message.name isEqualToString:@"xiaotuPrint"]) {
    [self printDocument:nil];
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

- (void)printDocument:(id)sender {
  if (self.webview == nil) {
    return;
  }
  NSPrintOperation* operation =
      [self.webview printOperationWithPrintInfo:[NSPrintInfo sharedPrintInfo]];
  if (operation != nil) {
    [operation runOperation];
  }
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

    const NSRect frame = NSMakeRect(0, 0, 1280, 840);
    NSWindow* window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [window setTitle:app_name];
    [window setMinSize:NSMakeSize(960, 600)];

    WKWebViewConfiguration* configuration = [[WKWebViewConfiguration alloc] init];
    WKUserContentController* controller = [[WKUserContentController alloc] init];
    [controller addScriptMessageHandler:script_handler name:@"xiaotuExport"];
    [controller addScriptMessageHandler:script_handler name:@"xiaotuPrint"];
    configuration.userContentController = controller;
    WKWebView* webview = [[WKWebView alloc] initWithFrame:frame configuration:configuration];
    [webview setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];
    [window setContentView:webview];
    script_handler.webview = webview;
    script_handler.window = window;
    [window center];
    [window makeKeyAndOrderFront:nil];

    NSURL* nsurl = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
    [webview loadRequest:[NSURLRequest requestWithURL:nsurl]];

    [NSApp activateIgnoringOtherApps:YES];
    [NSApp run];
  }
  return 0;
}

}  // namespace xiaotu::platform
