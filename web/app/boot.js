/* app/boot.js：启动流程：登录门禁、修改口令与首屏引导。
 * 由原 web/app.js 按职责拆出，供本模块与入口共同加载。
 */
import { API_BASE, api, can, clearRememberToken, editionAllows, escapeHtml, flushActiveDraft, hasUnsaved, keychainAvailable, keychainCall, loadScript, main, nav, notice, saveRememberToken, showNotice, state, updateNetworkBanner } from "./core.js";
import { FIXED_PAGES, buildNav, expandGroup, goToPage, handleHashChange, hashPageKey, mountPage, navHtml, readNavState, replaceHash, writeNavState } from "./shell.js";
import { blockMandatoryUpdate, maybeReportInstallResultOnce, requestNativeUpdateStatus } from "./update.js";

  export function clearBootstrapHash() {
    if (location.hash) {
      history.replaceState(null, "", location.pathname + location.search);
    }
  }

  // 单机版本机口令门禁：只收口令，内部仍按内置 admin 登录；不带用户名输入框。
  // 登录成功后按「30 天免输 / 每次都要输」决定是否把服务端签发的凭据写进钥匙串。
  export async function localPasswordLogin(password) {
    const gate = state.localGate || {};
    const remember = gate.remember_enabled !== false && keychainAvailable();
    const result = await api("POST", "/api/login", {
      username: "admin",
      password,
      remember,
    });
    state.csrf = result.csrf || "";
    if (result.remember_token) {
      await saveRememberToken(result.remember_token);
    } else {
      await clearRememberToken();
    }
    return result;
  }

  // 钥匙串里有凭据就先换会话；过期、被清除或时钟回拨时清掉本地凭据并回口令页。
  export async function tryRememberedLogin() {
    if (!keychainAvailable()) return { ok: false, message: "" };
    const stored = await keychainCall("get");
    const token = stored && stored.ok ? stored.token : "";
    if (!token) return { ok: false, message: "" };
    try {
      const result = await api("POST", "/api/local/remember", { token });
      state.csrf = result.csrf || "";
      return { ok: true };
    } catch (error) {
      if (error.status === 401 || error.code === "REMEMBER_TOKEN_INVALID") {
        await clearRememberToken();
        return { ok: false, message: error.message || "本机免输已失效，请重新输入口令" };
      }
      return { ok: false, message: "" };
    }
  }
  export function renderLocalGate(message) {
    state.current = null;
    nav.innerHTML = "";
    document.getElementById("mode-label").textContent = "单机版";
    document.getElementById("health").textContent = "";
    notice.hidden = true;
    main.querySelectorAll(":scope > *").forEach((node) => {
      if (node !== notice && node.id !== "data-warning" && node.id !== "network-warning") {
        node.remove();
      }
    });
    const card = document.createElement("div");
    card.className = "login-card";
    card.innerHTML = `
        <h2>小兔进销存</h2>
        <p class="hint">${escapeHtml(message || "请输入本机口令")}</p>
        <form data-role="local-gate-form">
          <label>本机口令<input name="password" type="password" autocomplete="current-password" required /></label>
          <button class="btn" type="submit">进入</button>
        </form>`;
    main.appendChild(card);
    updateNetworkBanner();
    const form = card.querySelector("form");
    form.addEventListener("submit", async (event) => {
      event.preventDefault();
      try {
        await localPasswordLogin(form.password.value);
        clearBootstrapHash();
        await bootstrap();
      } catch (error) {
        showNotice(error.message, "error");
      }
    });
  }

  // 老库第一次升级：已有数据但没有本机口令，问一次；跳过就保持 token 自动登录。
  export function renderLocalPasswordSetup() {
    state.current = null;
    nav.innerHTML = "";
    document.getElementById("mode-label").textContent = "单机版";
    document.getElementById("health").textContent = "";
    notice.hidden = true;
    main.querySelectorAll(":scope > *").forEach((node) => {
      if (node !== notice && node.id !== "data-warning" && node.id !== "network-warning") {
        node.remove();
      }
    });
    const card = document.createElement("div");
    card.className = "login-card";
    card.innerHTML = `
        <h2>设置本机口令</h2>
        <p class="hint">这台机器已有数据。设置口令后，下次启动需要先输入口令才能进入；也可以跳过，跳过则保持双击直接进入。</p>
        <form data-role="local-password-setup">
          <label>口令<input name="password" type="password" autocomplete="new-password" minlength="8" required /></label>
          <label>确认口令<input name="confirm" type="password" autocomplete="new-password" minlength="8" required /></label>
          <p class="hint">口令长度 8–128 个字符</p>
          <div class="form-actions">
            <button class="btn primary" type="submit">设置口令</button>
            <button class="btn" type="button" data-role="skip">跳过</button>
          </div>
        </form>`;
    main.appendChild(card);
    updateNetworkBanner();
    const form = card.querySelector("form");
    form.addEventListener("submit", async (event) => {
      event.preventDefault();
      if (form.password.value !== form.confirm.value) {
        showNotice("两次输入的口令不一致", "error");
        return;
      }
      try {
        await api("POST", "/api/local/password", { password: form.password.value });
        await localPasswordLogin(form.password.value);
        clearBootstrapHash();
        await bootstrap();
      } catch (error) {
        showNotice(error.message, "error");
      }
    });
    card.querySelector('[data-role="skip"]').addEventListener("click", async () => {
      try {
        await api("POST", "/api/local/password/skip");
        await bootstrap();
      } catch (error) {
        showNotice(error.message, "error");
      }
    });
  }
  export function renderLogin(message) {
    state.current = null;
    nav.innerHTML = "";
    document.getElementById("mode-label").textContent = "请登录";
    document.getElementById("health").textContent = "";
    notice.hidden = true;
    // 保留 #notice 与两条顶部提示，避免重新登录后 showNotice 写到已脱离文档的节点上。
    main.querySelectorAll(":scope > *").forEach((node) => {
      if (node !== notice && node.id !== "data-warning" && node.id !== "network-warning") {
        node.remove();
      }
    });
    const card = document.createElement("div");
    card.className = "login-card";
    card.innerHTML = `
        <h2>登录 小兔进销存</h2>
        <p class="hint">${escapeHtml(message || "请输入用户名与口令")}</p>
        <form data-role="login-form">
          <label>用户名<input name="username" autocomplete="username" required /></label>
          <label>口令<input name="password" type="password" autocomplete="current-password" required /></label>
          <button class="btn" type="submit">登录</button>
        </form>`;
    main.appendChild(card);
    updateNetworkBanner();
    const form = card.querySelector("form");
    form.addEventListener("submit", async (event) => {
      event.preventDefault();
      try {
        const result = await api("POST", "/api/login", {
          username: form.username.value.trim(),
          password: form.password.value,
        });
        state.csrf = result.csrf || "";
        if (result.must_change_password) {
          renderChangePassword("管理员已重置口令，请设置新口令");
          return;
        }
        await bootstrap();
      } catch (error) {
        showNotice(error.message, "error");
      }
    });
  }

  // 强制改密：登录后才进入；除改密与退出外的接口都会被服务端拒绝。
  export function renderChangePassword(message) {
    state.current = null;
    nav.innerHTML = "";
    document.getElementById("mode-label").textContent = "请修改口令";
    document.getElementById("health").textContent = "";
    notice.hidden = true;
    main.querySelectorAll(":scope > *").forEach((node) => {
      if (node !== notice && node.id !== "data-warning" && node.id !== "network-warning") {
        node.remove();
      }
    });
    const card = document.createElement("div");
    card.className = "login-card";
    card.innerHTML = `
        <h2>修改口令</h2>
        <p class="hint">${escapeHtml(message || "首次登录或口令被重置，请先修改口令")}</p>
        <form data-role="change-password-form">
          <label>原口令<input name="old_password" type="password" autocomplete="current-password" required /></label>
          <label>新口令<input name="new_password" type="password" autocomplete="new-password" minlength="8" required /></label>
          <label>确认新口令<input name="confirm_password" type="password" autocomplete="new-password" minlength="8" required /></label>
          <p class="hint">口令长度 8–128 个字符</p>
          <button class="btn" type="submit">保存并继续</button>
        </form>`;
    main.appendChild(card);
    updateNetworkBanner();
    const form = card.querySelector("form");
    form.addEventListener("submit", async (event) => {
      event.preventDefault();
      if (form.new_password.value !== form.confirm_password.value) {
        showNotice("两次输入的新口令不一致", "error");
        return;
      }
      try {
        const result = await api("POST", "/api/password/change", {
          old_password: form.old_password.value,
          new_password: form.new_password.value,
        });
        state.csrf = result.csrf || state.csrf;
        await bootstrap();
      } catch (error) {
        showNotice(error.message, "error");
      }
    });
  }
  export async function bootstrap() {
    // 启动耗时打点（方案：启动速度优化 P0-1）：相对 bootstrap 入口的各阶段毫秒数。
    const bootT0 = performance.now();
    const bootMark = (stage) =>
      console.debug("[xiaotu] 启动打点 " + stage + " +" + Math.round(performance.now() - bootT0) + "ms");
    try {
      // 启动并行化（方案：启动速度优化 P0-2）：health 与 me 同时发；
      // pages.json 不依赖登录态，提前取，脚本加载仍在登录成功后。
      // 走登录/门禁分支时 pagesPromise 可能无人 await，挂空 catch 防 unhandled rejection。
      const pagesPromise = fetch(API_BASE + "/gen/pages.json").then((r) => r.json());
      pagesPromise.catch(() => {});
      // health 免登录，先拿单机版门禁状态；网络版这些字段恒为 false。
      const [health, me0] = await Promise.all([
        api("GET", "/api/health"),
        api("GET", "/api/me").catch((error) => {
          if (error.status !== 401) throw error;
          return null;
        }),
      ]);
      bootMark("health与me就绪");
      state.edition = health.edition || "";
      const isLocal = health.mode === "local";
      const localGate = health.local_gate || {};
      state.mode = health.mode || "";
      state.localGate = localGate;
      document.getElementById("mode-label").textContent = isLocal ? "单机版" : "网络版";
      document.getElementById("health").textContent =
        "v" + health.version + " · schema v" + health.schema_version;

      let me = me0;
      if (!me) {
        // 已设本机口令：先试钥匙串里的长期凭据，失败再出门禁页；
        // 启动链接里的一次性 token 不参与自动登录。
        if (isLocal && localGate.password_required) {
          clearBootstrapHash();
          const remembered = await tryRememberedLogin();
          if (!remembered.ok) {
            renderLocalGate(remembered.message);
            return;
          }
          me = await api("GET", "/api/me");
        } else if (isLocal && localGate.prompt_set_password) {
          // 有数据但从未设过口令：问一次；新装无数据不打扰。
          renderLocalPasswordSetup();
          return;
        } else {
          const hash = location.hash || "";
          const match = /(?:^#|&)t=([0-9a-f]+)/.exec(hash);
          if (match) {
            try {
              const session = await api("POST", "/api/session", { token: match[1] });
              state.csrf = session.csrf || "";
            } catch (error) {
              // health 读取后口令被另一个页面设置：同样回到门禁页。
              if (error.code === "LOCAL_PASSWORD_REQUIRED") {
                clearBootstrapHash();
                renderLocalGate();
                return;
              }
              throw error;
            }
            clearBootstrapHash();
            me = await api("GET", "/api/me");
          } else {
            renderLogin("请使用用户名与口令登录");
            return;
          }
        }
      }
      state.csrf = me.csrf || state.csrf;
      state.permissions = me.permissions || [];
      if (me.must_change_password) {
        renderChangePassword("管理员已重置口令，请设置新口令");
        return;
      }

      // 强制升级拦截：壳内主动取一次状态；浏览器里没有桥，静默跳过。
      requestNativeUpdateStatus(blockMandatoryUpdate);
      // Windows 更新重启后的一次性安装结果提示；浏览器/mac 静默跳过。
      maybeReportInstallResultOnce();

      // 数据目录位于同步盘时由服务端给出提示，登录后常驻在页面顶部。
      let warningNode = document.getElementById("data-warning");
      if (!warningNode) {
        warningNode = document.createElement("div");
        warningNode.id = "data-warning";
        warningNode.className = "notice warn";
        main.prepend(warningNode);
      }
      warningNode.textContent = health.data_dir_warning || "";
      warningNode.hidden = !health.data_dir_warning;

      const pages = await pagesPromise;
      bootMark("pages.json就绪");
      state.pages = pages;
      // 各 gen 页脚本相互独立（分别注册 window.XiaotuPages[key]），并行加载。
      await Promise.all(pages.map((page) => loadScript(page.script)));
      bootMark("页面脚本加载完成");

      const all = [
        ...pages.map((page) => ({
          key: page.key,
          title: page.title,
          group: page.group || "",
          permission: page.permission || "",
          render: (mount, ui) => window.XiaotuPages[page.key].render(mount, ui),
        })),
        ...FIXED_PAGES,
      ].filter(
        (page) =>
          (!page.permission || can(page.permission)) &&
          (!page.capability || editionAllows(page.capability))
      );

      nav.innerHTML = navHtml(buildNav(all));
      nav.addEventListener("click", (event) => {
        const toggle = event.target.closest("[data-group-toggle]");
        if (toggle) {
          const group = toggle.closest(".nav-group");
          const key = group.classList.contains("open") ? "" : group.dataset.group;
          expandGroup(key);
          writeNavState("xiaotu.nav.group", key);
          return;
        }
        const button = event.target.closest("button[data-key]");
        if (!button) return;
        const page = all.find((item) => item.key === button.dataset.key);
        if (page) goToPage(page);
      });

      // 路由优先：hash > 上次页面 > 默认页；无权或未知 key 回退到默认页。
      state.allPages = all;
      const hashKey = hashPageKey();
      const savedPage = readNavState("xiaotu.nav.page");
      const initial =
        all.find((page) => page.key === hashKey) ||
        all.find((page) => page.key === savedPage) ||
        all[0];
      if (initial) {
        if (hashKey !== initial.key) replaceHash(initial.key);
        mountPage(initial);
        bootMark("首屏挂载");
      }
    } catch (error) {
      if (error.status === 401) {
        renderLogin("请使用用户名与口令登录");
      } else {
        showNotice("加载失败: " + error.message, "error");
      }
    }
  }

  window.addEventListener("hashchange", handleHashChange);
  // goToPage 改用 pushState 之后，浏览器前进/后退不再触发 hashchange，改由 popstate 驱动。
  window.addEventListener("popstate", handleHashChange);
  // 关闭窗口或刷新时的浏览器原生提示。只对 --server 的普通浏览器有效；
  // 单机版 WKWebView 不执行 beforeunload，关窗不会弹确认，需在文档中注明。
  window.addEventListener("beforeunload", (event) => {
    flushActiveDraft();
    if (!hasUnsaved()) return;
    event.preventDefault();
    event.returnValue = "";
  });
  bootstrap();

