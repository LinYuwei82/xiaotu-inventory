/* app/admin_settings.js：系统参数页（公司抬头、负库存、会话超时、成本口径与本机免输）。
 * 由 admin.js 拆出，供 pages.js 与入口共同加载。
 */
import { can, showNotice, state } from "./core_state.js";
import { askConfirm } from "./core_dialog.js";
import { api } from "./core_api.js";
import { clearRememberToken, keychainAvailable, saveRememberToken } from "./core.js";

  // ---------- 系统参数 ----------

  // 只开放公司抬头三项；编号规则、日期与金额格式仍是固定口径，只读展示。
  export async function settingPage(mount) {
    const writable = can("setting.write");
    const showLocalRemember =
      state.mode === "local" &&
      (state.localGate || {}).password_required === true &&
      writable &&
      keychainAvailable();
    mount.innerHTML = `
      <div class="page-head">
        <div>
          <h2>系统参数</h2>
          <p class="page-sub">公司抬头用于打印占位符与默认抬头；其余展示口径当前固定</p>
        </div>
      </div>
      <div class="card">
        <form class="grid-form" data-role="form">
          <label class="field"><span>公司名称</span><input type="text" maxlength="100" data-field="name" ${writable ? "" : "disabled"} /></label>
          <label class="field"><span>联系电话</span><input type="text" maxlength="50" data-field="phone" ${writable ? "" : "disabled"} /></label>
          <label class="field"><span>公司地址</span><input type="text" maxlength="200" data-field="address" ${writable ? "" : "disabled"} /></label>
          <div class="field">
            <span>允许负库存出库</span>
            <label class="inline-check"><input type="checkbox" data-field="allow_negative_stock" ${writable ? "" : "disabled"} /> <span>开启后出库可把结存做成负数</span></label>
            <span class="page-sub" data-role="negative-state"></span>
          </div>
          <label class="field"><span>会话空闲超时（分钟）</span><input type="number" min="1" max="1440" step="1" data-field="session_idle_minutes" ${writable ? "" : "disabled"} /></label>
          <label class="field"><span>会话绝对上限（小时）</span><input type="number" min="1" max="720" step="1" data-field="session_absolute_hours" ${writable ? "" : "disabled"} /></label>
          <label class="field"><span>库存成本口径</span>
            <select data-field="cost_basis" ${writable ? "" : "disabled"}>
              <option value="INCLUSIVE">含税价</option>
              <option value="NET">不含税净额</option>
            </select>
            <span class="page-sub">一般纳税人可切「不含税」；小规模纳税人保持「含税」。切换只影响此后审核的单据，历史结存与毛利不重算。</span>
            <span class="page-sub" data-role="basis-state"></span>
          </label>
          <span class="page-sub">空闲超时按最近访问滑动续期；绝对上限从登录时刻起算，超过后需重新登录。</span>
          <div class="form-actions">
            ${writable ? '<button class="btn primary" type="submit">保存</button>' : '<span class="page-sub">当前角色只读</span>'}
          </div>
        </form>
      </div>
      <div class="card">
        <h3>固定口径（只读）</h3>
        <p class="page-sub">单据编号：前缀 + 日期 YYYYMMDD + 4 位序号，例如 XSCK20261113-0001；前缀按单据类型固定，暂不可改。</p>
        <p class="page-sub">日期显示固定 YYYY-MM-DD；金额为人民币元、2 位小数；数量小数位跟随商品单位设置（0–4 位）。</p>
        <p class="page-sub">打印占位符：{{company_name}}、{{company_phone}}、{{company_address}}；模板抬头为空时默认打印公司名称。</p>
      </div>
      ${
        showLocalRemember
          ? `<div class="card" data-role="local-remember">
        <h3>本机免输</h3>
        <p class="page-sub">钥匙串只保存服务端签发的随机凭据，不保存本机口令；凭据 30 天后失效。</p>
        <label class="inline-check"><input type="radio" name="local-remember" value="on" /> <span>30 天内免输本机口令</span></label>
        <label class="inline-check"><input type="radio" name="local-remember" value="off" /> <span>每次启动都要输入本机口令</span></label>
        <p class="page-sub" data-role="local-remember-state"></p>
        <div class="form-actions">
          <button class="btn" type="button" data-role="local-remember-save">保存本机免输设置</button>
          <button class="btn" type="button" data-role="local-remember-clear">清除本机记忆</button>
        </div>
      </div>`
          : ""
      }`;

    const form = mount.querySelector('[data-role="form"]');
    const input = (name) => form.querySelector('[data-field="' + name + '"]');
    const negativeState = mount.querySelector('[data-role="negative-state"]');
    const basisState = mount.querySelector('[data-role="basis-state"]');

    function renderNegativeState() {
      negativeState.textContent = input("allow_negative_stock").checked
        ? "当前：开启（出库超库存会形成负结存）"
        : "当前：关闭（出库超库存会被拒绝）";
    }

    function renderBasisState(settings) {
      const label = settings.cost_basis === "NET" ? "不含税净额" : "含税价";
      basisState.textContent = settings.cost_basis_changed_at
        ? "当前：" + label + "；切换于 " + settings.cost_basis_changed_at + "，只影响新审核单据"
        : "当前：" + label + "；尚未切换过口径，只影响新审核单据";
    }

    try {
      const data = await api("GET", "/api/settings");
      const company = data.company || {};
      input("name").value = company.name || "";
      input("phone").value = company.phone || "";
      input("address").value = company.address || "";
      input("allow_negative_stock").checked = data.allow_negative_stock === true;
      input("session_idle_minutes").value = data.session_idle_minutes || 30;
      input("session_absolute_hours").value = data.session_absolute_hours || 12;
      input("cost_basis").value = data.cost_basis === "NET" ? "NET" : "INCLUSIVE";
      renderNegativeState();
      renderBasisState(data);
    } catch (error) {
      showNotice(error.message, "error");
      return;
    }

    input("allow_negative_stock").addEventListener("change", renderNegativeState);

    form.addEventListener("submit", async (event) => {
      event.preventDefault();
      if (!writable) return;
      try {
        await api("PUT", "/api/settings", {
          company: {
            name: input("name").value.trim(),
            phone: input("phone").value.trim(),
            address: input("address").value.trim(),
          },
          allow_negative_stock: input("allow_negative_stock").checked,
          session_idle_minutes: Number(input("session_idle_minutes").value),
          session_absolute_hours: Number(input("session_absolute_hours").value),
          cost_basis: input("cost_basis").value,
        });
        renderNegativeState();
        renderBasisState(await api("GET", "/api/settings"));
        showNotice("系统参数已保存", "ok");
      } catch (error) {
        showNotice(error.message, "error");
      }
    });

    // 单机版「本机免输」：开关改服务端策略，凭据本体在 macOS 钥匙串里。
    const rememberCard = mount.querySelector('[data-role="local-remember"]');
    if (rememberCard) {
      const rememberState = rememberCard.querySelector('[data-role="local-remember-state"]');
      const onInput = rememberCard.querySelector('input[value="on"]');
      const offInput = rememberCard.querySelector('input[value="off"]');
      async function refreshRemember() {
        const data = await api("GET", "/api/local/remember/setting");
        const enabled = data.enabled !== false;
        onInput.checked = enabled;
        offInput.checked = !enabled;
        rememberState.textContent = enabled
          ? data.has_token
            ? "当前：30 天内免输；系统钥匙串中已有凭据。"
            : "当前：30 天内免输；下次输入口令后写入系统钥匙串。"
          : "当前：每次启动都要输入本机口令。";
      }
      try {
        await refreshRemember();
      } catch (error) {
        showNotice(error.message, "error");
      }
      rememberCard
        .querySelector('[data-role="local-remember-save"]')
        .addEventListener("click", async () => {
          const enabled = onInput.checked;
          try {
            const result = await api("PUT", "/api/local/remember/setting", { enabled });
            if (enabled && result.remember_token) {
              if (!(await saveRememberToken(result.remember_token))) {
                showNotice("设置已保存，但未能写入系统钥匙串；下次启动仍需输入口令", "error");
                await refreshRemember();
                return;
              }
            } else if (!enabled) {
              await clearRememberToken();
            }
            await refreshRemember();
            showNotice("本机免输设置已保存", "ok");
          } catch (error) {
            showNotice(error.message, "error");
          }
        });
      rememberCard
        .querySelector('[data-role="local-remember-clear"]')
        .addEventListener("click", async () => {
          if (
            !(await askConfirm("清除后，下次启动需要重新输入本机口令。", {
              okText: "清除",
              cancelText: "取消",
            }))
          ) {
            return;
          }
          try {
            await api("POST", "/api/local/remember/clear");
            await clearRememberToken();
            await refreshRemember();
            showNotice("本机记忆已清除", "ok");
          } catch (error) {
            showNotice(error.message, "error");
          }
        });
    }
  }
