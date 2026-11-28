/* app/core_state.js：全局状态、版本能力表、页面根节点引用、通知与权限判断。
 * 由 core.js 拆出；不依赖其他 app 模块，是全站公共层的底座。
 */

  export const params = new URLSearchParams(location.search);
  // 开发态：WebView 指向 dev server 时，用 ?api=http://127.0.0.1:<port> 指定后端地址。
  export const API_BASE = params.get("api") || "";
  export const state = {
    current: null,
    pages: [],
    allPages: [],
    csrf: "",
    permissions: [],
    edition: "",
    mode: "",
    localGate: {},
  };

  // 版本能力表由 tools/codegen 从 editions.json 生成，与后端 edition_caps.h 同源；
  // 这里只负责隐藏入口，服务端仍会按同一张表二次拒绝。
  export const EDITION_CAPS = window.XIAOTU_EDITION_CAPS || null;
  export function editionCaps() {
    if (!EDITION_CAPS) return null;
    const code = state.edition || EDITION_CAPS.default;
    return EDITION_CAPS.editions[code] || EDITION_CAPS.editions[EDITION_CAPS.default] || null;
  }
  export function editionAllows(feature) {
    const caps = editionCaps();
    if (!caps || !caps.features) return false;
    const value = caps.features[feature];
    return value === "full" || value === "limited";
  }
  export const main = document.getElementById("main");
  export const nav = document.getElementById("nav");
  export const notice = document.getElementById("notice");

  export function showNotice(message, kind) {
    notice.textContent = message;
    notice.className = "notice" + (kind ? " " + kind : "");
    notice.hidden = !message;
  }

  // 前端只负责隐藏入口，真正的拦截在各路由的 guard 里。
  export function can(permission) {
    return (
      state.permissions.indexOf("*") >= 0 || state.permissions.indexOf(permission) >= 0
    );
  }
