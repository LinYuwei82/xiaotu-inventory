/* app/core_dialog.js：页内对话框（确认 / 提示 / 输入）。
 * 由 core.js 拆出；WKWebView 没有实现原生 confirm，统一走页内确认层。
 */

  // WKWebView 没有实现 WKUIDelegate 的 confirm 面板，window.confirm 会静默返回
  // false；离开确认改用页内确认层，不依赖浏览器原生对话框。
  export let confirmDialogOpen = false;

  // options: message / okText / cancelText（传 null 表示只有确认按钮）/ withInput / defaultValue
  //          blocking（强制升级用：屏蔽 Esc 与点击遮罩，必须点确认）
  export function askDialog(options) {
    const okText = options.okText || "确定";
    const cancelText = options.cancelText === undefined ? "取消" : options.cancelText;
    const withInput = Boolean(options.withInput);
    const blocking = options.blocking === true;
    if (confirmDialogOpen) return Promise.resolve({ ok: false, value: null });
    confirmDialogOpen = true;
    return new Promise((resolve) => {
      const mask = document.createElement("div");
      mask.className = "confirm-mask";
      const box = document.createElement("div");
      box.className = "confirm-box";
      box.setAttribute("role", "dialog");
      box.setAttribute("aria-modal", "true");
      const text = document.createElement("p");
      text.className = "confirm-text";
      text.textContent = options.message;
      let field = null;
      if (withInput) {
        field = document.createElement("input");
        field.type = "text";
        field.className = "confirm-input";
        field.value =
          options.defaultValue === undefined ? "" : String(options.defaultValue);
      }
      const actions = document.createElement("div");
      actions.className = "confirm-actions";
      const cancel = document.createElement("button");
      cancel.type = "button";
      cancel.className = "btn";
      cancel.textContent = cancelText;
      const ok = document.createElement("button");
      ok.type = "button";
      ok.className = "btn primary";
      ok.textContent = okText;
      if (cancelText !== null) actions.append(cancel);
      actions.append(ok);
      if (field) box.append(text, field, actions);
      else box.append(text, actions);
      mask.append(box);

      const previous = document.activeElement;
      function close(confirmed) {
        document.removeEventListener("keydown", onKey, true);
        mask.remove();
        confirmDialogOpen = false;
        if (previous && previous.focus) previous.focus();
        resolve({ ok: confirmed, value: field ? field.value : null });
      }
      function onKey(event) {
        if (event.key === "Escape") {
          event.preventDefault();
          if (!blocking) close(false);
        } else if (event.key === "Enter") {
          event.preventDefault();
          close(true);
        }
      }
      cancel.addEventListener("click", () => close(false));
      ok.addEventListener("click", () => close(true));
      mask.addEventListener("click", (event) => {
        if (event.target === mask && !blocking) close(false);
      });
      document.addEventListener("keydown", onKey, true);
      document.body.appendChild(mask);
      if (field) {
        field.focus();
        field.select();
      } else {
        ok.focus();
      }
    });
  }
  export async function askConfirm(message, options) {
    const result = await askDialog({
      message,
      okText: options && options.okText,
      cancelText: options && options.cancelText,
    });
    return result.ok;
  }
  export async function askAlert(message) {
    await askDialog({ message, okText: "知道了", cancelText: null });
  }
  export async function askPrompt(message, defaultValue) {
    const result = await askDialog({ message, defaultValue, withInput: true });
    return result.ok ? result.value : null;
  }
