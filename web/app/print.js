/* app/print.js：打印视图组装、页面打印与 PDF 导出。
 * 由原 web/app.js 按职责拆出，供本模块与入口共同加载。
 */
import { api, askConfirm, escapeHtml, isWinWebview, money, showNotice } from "./core.js";

  export function printPage(layout) {
    const handlers = window.webkit && window.webkit.messageHandlers;
    if (handlers && handlers.xiaotuPrint) {
      handlers.xiaotuPrint.postMessage({ paper: layout || null });
    } else {
      window.print();
    }
  }

  // ---------- 自定义打印模板 ----------
  export function printAlign(align) {
    return align === "RIGHT" ? "right" : align === "CENTER" ? "center" : "left";
  }
  export function printCellText(value, type) {
    if (value === null || value === undefined) return "";
    // 「—」等占位文本按原样输出，避免被金额格式化吞掉。
    if (type === "MONEY" && typeof value === "number") return money(value);
    return String(value);
  }
  export function printTotalsText(totals) {
    if (!totals) return "";
    if (totals.text !== undefined) return String(totals.text);
    if (totals.total_amount_text !== undefined) {
      let text = "合计数量 " + totals.total_qty + "，合计金额 " + totals.total_amount_text + " 元";
      if (totals.total_tax_text !== undefined) {
        text += "\n税额 " + totals.total_tax_text + " 元";
        text += "\n价税合计 " + totals.total_with_tax_text + " 元";
      }
      return text;
    }
    if (totals.closing_text !== undefined) {
      let text = "期初余额 " + totals.opening_text + " 元，期末余额 " + totals.closing_text + " 元";
      if (totals.reverse_text !== undefined) {
        text += "；本期撤销收付款 " + totals.reverse_text;
      }
      if (totals.settle_net_text !== undefined) {
        text += "，期间核销净额 " + totals.settle_net_text;
      }
      return text;
    }
    return "";
  }

  // WKWebView 打印排版按 90dpi 换算 CSS 像素（1 CSS px = 0.8pt），比 CSS 规范的
  // 96dpi（1 px = 0.75pt）整体大 1.067 倍。补偿必须放在 CSS 层：若改用
  // NSPrintInfo.scalingFactor，内容正好占满可打印宽度时会先触发 WebKit 的
  // shrink-to-fit，再被 scalingFactor 压一次，净效果反而小 6.25%。
  export const MAC_PRINT_ZOOM = 0.9375;  // 0.75 / 0.8 = 96 / 90
  export function macPrintZoom() {
    const handlers = window.webkit && window.webkit.messageHandlers;
    // 只有 macOS WKWebView 壳走 90dpi 排版；浏览器与 Windows WebView2 按 96dpi。
    return handlers && (handlers.xiaotuPrint || handlers.xiaotuPrintPdf) ? MAC_PRINT_ZOOM : 1;
  }

  // 模板只提供展示配置，所有数据都按纯文本转义后拼进打印视图。
  export function printViewHtml(data) {
    const tmpl = data.template || {};
    const layout = data.layout || {};
    const columns = data.columns || [];
    const scale = Math.max(10, Math.min(100, Number(layout.scale_percent) || 100));
    const zoom = (scale / 100) * macPrintZoom();
    const tableWidth = Number(layout.table_width_mm) || 0;
    const colgroup = columns
      .map((column) =>
        column.width_mm > 0 ? `<col style="width:${Number(column.width_mm)}mm" />` : "<col />"
      )
      .join("");
    const head = columns
      .map(
        (column) =>
          `<th style="text-align:${printAlign(column.align)}">${escapeHtml(column.label)}</th>`
      )
      .join("");
    const body = (data.rows || []).length
      ? data.rows
          .map(
            (row) =>
              `<tr>${columns
                .map((column) => {
                  const styles = ["text-align:" + printAlign(column.align)];
                  if (column.font_size_pt) styles.push("font-size:" + Number(column.font_size_pt) + "pt");
                  if (column.bold) styles.push("font-weight:600");
                  return `<td style="${styles.join(";")}">${escapeHtml(
                    printCellText(row[column.key], column.type)
                  )}</td>`;
                })
                .join("")}</tr>`
          )
          .join("")
      : `<tr><td colspan="${columns.length || 1}" style="text-align:center">暂无数据</td></tr>`;
    const fields = (data.header_fields || [])
      .map(
        (field) =>
          `<div><strong>${escapeHtml(field.label)}：</strong>${escapeHtml(field.value)}</div>`
      )
      .join("");
    const totals = printTotalsText(data.totals);
    const headerText = data.header_text
      ? `<div class="print-header">${escapeHtml(data.header_text)}</div>`
      : "";
    const footerText = data.footer_text
      ? `<div class="print-footer">${escapeHtml(data.footer_text)}</div>`
      : "";
    return `
      <div class="print-sheet" style="width:${
        tableWidth > 0 ? tableWidth + "mm" : "auto"
      };zoom:${zoom};font-size:${Number(tmpl.font_size_pt) || 10}pt">
        ${headerText}
        <h1 class="print-title">${escapeHtml(data.title || "")}</h1>
        <div class="print-subtitle">${escapeHtml(data.subtitle || "")}</div>
        ${fields ? `<div class="print-fields">${fields}</div>` : ""}
        <div class="print-table-wrap">
          <table class="print-table">
            <colgroup>${colgroup}</colgroup>
            <thead><tr>${head}</tr></thead>
            <tbody>${body}</tbody>
          </table>
        </div>
        ${totals ? `<div class="print-totals">${escapeHtml(totals)}</div>` : ""}
        <div class="print-meta">打印时间：${escapeHtml(data.printed_at || "")}</div>
        ${footerText}
      </div>`;
  }
  export function printPageCss(tmpl, layout) {
    const size =
      layout && layout.width_mm && layout.height_mm
        ? `${Number(layout.width_mm)}mm ${Number(layout.height_mm)}mm`
        : tmpl.page_size === "CUSTOM"
          ? `${Number(tmpl.paper_width_mm)}mm ${Number(tmpl.paper_height_mm)}mm`
          : tmpl.orientation === "LANDSCAPE"
            ? "297mm 210mm"
            : "210mm 297mm";
    // WKWebView 打印时以 @page margin 为准，NSPrintInfo 的 margin 会被它盖掉；
    // 原生模式若把这里归零，纸边距就只剩 0。三端统一用模板边距。
    const margin = Number(tmpl.margin_mm) || 0;
    return `@page { size: ${size}; margin: ${margin}mm; }`;
  }
  export function mountPrintView(data) {
    for (const node of document.querySelectorAll("[data-print-view]")) node.remove();
    const view = document.createElement("section");
    view.className = "print-view";
    view.dataset.printView = "1";
    view.innerHTML = printViewHtml(data);
    // macOS 壳：把视图宽度限制在可打印宽度（同样按 90dpi 折算），超宽内容只裁切，
    // 不触发 WebKit 的整页 shrink-to-fit；连续纸原始尺寸与套打位置因此保持不缩放。
    const printable = Number((data.layout || {}).printable_width_mm) || 0;
    const nativeZoom = macPrintZoom();
    if (nativeZoom < 1 && printable > 0) {
      view.style.width = `${(printable * nativeZoom).toFixed(3)}mm`;
    }
    document.body.appendChild(view);
    const style = document.createElement("style");
    style.dataset.printView = "1";
    style.textContent = printPageCss(data.template || {}, data.layout || {});
    document.head.appendChild(style);
    document.body.classList.add("print-mode");
    return () => {
      document.body.classList.remove("print-mode");
      view.remove();
      style.remove();
    };
  }
  export async function confirmPrintLayout(layout) {
    if (!layout || !layout.warn || layout.warn === "paper_unknown") return true;
    let message = layout.warn_text || "打印内容超出纸张，可能被裁切。";
    if (layout.suggest_landscape) message += "\n\n可把纸张方向改为横向后再试。";
    return askConfirm(message + "\n\n仍要继续吗？", {
      okText: "继续",
      cancelText: "返回调整",
    });
  }
  export async function printWithTemplate(data) {
    if (!(await confirmPrintLayout(data.layout))) return;
    const cleanup = mountPrintView(data);
    // 浏览器打印结束（含取消）后清理；单机版桥接不触发时，下次打印会先清掉旧视图。
    window.addEventListener("afterprint", cleanup, { once: true });
    printPage(data.layout);
  }
  export function pdfBridge() {
    const handlers = window.webkit && window.webkit.messageHandlers;
    if (handlers && handlers.xiaotuPrintPdf) return handlers.xiaotuPrintPdf;
    // Windows WebView2：mock 一个有 postMessage 的桥，内部用 chrome.webview.postMessage 发到 C++ 壳
    if (isWinWebview()) {
      return {
        postMessage: (payload) => {
          // 必须发字符串：壳端优先按字符串读取，对象消息在部分 WebView2 版本读不到
          window.chrome.webview.postMessage(
            JSON.stringify({ kind: "printPdf", filename: payload && payload.filename })
          );
        },
      };
    }
    return null;
  }
  export function pdfFileName(name) {
    const base = String(name || "导出").replace(/[\\/:*?"<>|]/g, "_").trim() || "导出";
    return base + ".pdf";
  }

  // 直接导出 PDF：复用打印视图，交给原生保存面板写文件，不弹系统打印对话框。
  export async function exportPdfTarget(path, filename) {
    const bridge = pdfBridge();
    if (!bridge) {
      showNotice("直接导出 PDF 仅在桌面版可用；浏览器请用系统打印另存为 PDF", "error");
      return;
    }
    try {
      const data = await api("GET", path);
      if (!(await confirmPrintLayout(data.layout))) return;
      const cleanup = mountPrintView(data);
      window.xiaotuPdfDone = (result) => {
        cleanup();
        delete window.xiaotuPdfDone;
        if (result === "ok") showNotice("PDF 已导出", "ok");
        else if (result === "error") showNotice("PDF 导出失败", "error");
      };
      bridge.postMessage({ filename: pdfFileName(filename || data.title), paper: data.layout });
    } catch (error) {
      showNotice(error.message, "error");
    }
  }
  export async function printTarget(path) {
    try {
      await printWithTemplate(await api("GET", path));
    } catch (error) {
      showNotice(error.message, "error");
    }
  }

  // 新建单据的幂等键：进入新建页生成一个，提交成功后作废，重试复用同一个。
  // 服务端按 request_id 唯一索引返回首次创建的单据，避免双击/断线重试重复建单。
