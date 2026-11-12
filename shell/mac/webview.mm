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
