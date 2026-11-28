/* app/core_export.js：导出下载（CSV / XLSX / 文本）与 WebView 壳交互。
 * 由 core.js 拆出；浏览器走 Blob，macOS 交 WKWebView，Windows WebView2 走原生对话框。
 */
import { API_BASE, showNotice } from "./core_state.js";

  // Windows WebView2 壳检测：chrome.webview + hostObjects 存在
  export function isWinWebview() {
    return !!(window.chrome && window.chrome.webview && window.chrome.webview.hostObjects);
  }
  // 用系统默认浏览器打开外链：Windows 走壳的 native_io.openUrl，其余用 window.open
  export function openExternalUrl(url) {
    if (!url) return;
    try {
      if (isWinWebview()) {
        window.chrome.webview.hostObjects.native_io.openUrl(url);
        return;
      }
      window.open(url, "_blank", "noopener");
    } catch (error) {
      showNotice("打开链接失败：" + (error && error.message ? error.message : url), "error");
    }
  }
  // Blob → base64（UTF-8 safe，替代 btoa）
  export function blobToBase64(blob) {
    return new Promise((resolve, reject) => {
      const reader = new FileReader();
      reader.onload = () => resolve(reader.result.split(",")[1]);
      reader.onerror = reject;
      reader.readAsDataURL(blob);
    });
  }

  // 报表 / 对账单共用：浏览器走 Blob 下载，macOS 原生交 WKWebView，Windows WebView2 走原生对话框。
  export async function downloadCsv(path) {
    const response = await fetch(API_BASE + path, { credentials: "include" });
    if (!response.ok) {
      let message = "导出失败（HTTP " + response.status + "）";
      try {
        const payload = await response.json();
        if (payload && payload.message) message = payload.message;
      } catch (error) {
        /* 非 JSON 响应，保留默认提示 */
      }
      throw new Error(message);
    }
    const disposition = response.headers.get("Content-Disposition") || "";
    const match = /filename="([^"]+)"/.exec(disposition);
    const filename = match ? match[1] : "report.csv";
    // response.text() 会丢掉 BOM，这里统一补回，保证 Excel 按 UTF-8 打开。
    const text = await response.text();
    const content = "\uFEFF" + text;
    const handlers = window.webkit && window.webkit.messageHandlers;
    if (handlers && handlers.xiaotuExport) {
      handlers.xiaotuExport.postMessage({ filename, content });
      return;
    }
    if (isWinWebview()) {
      const nativeIo = await window.chrome.webview.hostObjects.native_io;
      const b64 = await blobToBase64(new Blob([content], { type: "text/csv;charset=utf-8" }));
      const ok = await nativeIo.saveFile(filename, b64);
      if (ok === false) return;  // 用户取消保存：静默返回，不弹错误
      return;
    }
    const blob = new Blob([content], { type: "text/csv;charset=utf-8" });
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = url;
    link.download = filename;
    document.body.appendChild(link);
    link.click();
    link.remove();
    URL.revokeObjectURL(url);
  }

  // xlsx 是二进制，单机版桥接用 base64 传回原生保存，浏览器走 Blob 下载。
  export function toBase64(bytes) {
    let binary = "";
    const chunk = 0x8000;
    for (let i = 0; i < bytes.length; i += chunk) {
      binary += String.fromCharCode.apply(null, bytes.subarray(i, i + chunk));
    }
    return btoa(binary);
  }
  export async function downloadXlsx(path) {
    const response = await fetch(API_BASE + path, { credentials: "include" });
    if (!response.ok) {
      let message = "导出失败（HTTP " + response.status + "）";
      try {
        const payload = await response.json();
        if (payload && payload.message) message = payload.message;
      } catch (error) {
        /* 非 JSON 响应，保留默认提示 */
      }
      throw new Error(message);
    }
    const disposition = response.headers.get("Content-Disposition") || "";
    const match = /filename="([^"]+)"/.exec(disposition);
    const filename = match ? match[1] : "report.xlsx";
    const bytes = new Uint8Array(await response.arrayBuffer());
    const handlers = window.webkit && window.webkit.messageHandlers;
    if (handlers && handlers.xiaotuExport) {
      handlers.xiaotuExport.postMessage({ filename, contentBase64: toBase64(bytes) });
      return;
    }
    if (isWinWebview()) {
      const nativeIo = await window.chrome.webview.hostObjects.native_io;
      await nativeIo.saveFile(filename, toBase64(bytes));  // 取消保存时静默返回
      return;
    }
    const blob = new Blob([bytes], {
      type: "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
    });
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = url;
    link.download = filename;
    document.body.appendChild(link);
    link.click();
    link.remove();
    URL.revokeObjectURL(url);
  }

  // 文本类导出（错误清单等）：macOS 交原生，Windows WebView2 走原生对话框，浏览器走 Blob。
  export function saveText(filename, text) {
    const content = "\uFEFF" + text;
    const handlers = window.webkit && window.webkit.messageHandlers;
    if (handlers && handlers.xiaotuExport) {
      handlers.xiaotuExport.postMessage({ filename, content });
      return;
    }
    if (isWinWebview()) {
      // hostObjects 下的对象是异步代理，不保证有 .then，统一用 async 函数包一层
      (async () => {
        try {
          const nativeIo = await window.chrome.webview.hostObjects.native_io;
          const b64 = await blobToBase64(
            new Blob([content], { type: "text/plain;charset=utf-8" })
          );
          await nativeIo.saveFile(filename, b64);
        } catch (error) {
          showNotice(error.message, "error");
        }
      })();
      return;
    }
    const blob = new Blob([content], { type: "text/csv;charset=utf-8" });
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = url;
    link.download = filename;
    document.body.appendChild(link);
    link.click();
    link.remove();
    URL.revokeObjectURL(url);
  }
  export function csvField(value) {
    const text = String(value === null || value === undefined ? "" : value);
    return /[",\r\n]/.test(text) ? '"' + text.replace(/"/g, '""') + '"' : text;
  }
