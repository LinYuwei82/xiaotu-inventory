/* app/admin.js：用户、审计日志与系统设置页。
 * 由原 web/app.js 按职责拆出，供本模块与入口共同加载。
 */
import { api, askConfirm, bizTypeText, can, clearRememberToken, downloadCsv, downloadXlsx, escapeHtml, keychainAvailable, params, saveRememberToken, showNotice, state } from "./core.js";
import { USER_ROLES } from "./stock.js";

  export function roleLabel(code) {
    const role = USER_ROLES.find((item) => item.value === code);
    return role ? role.label : code;
  }

  // 内置管理员是单机版与命令行重置口令的兜底入口，服务端不允许改角色、停用或删除。
  // 这里把这些限制直接摆在界面上，而不是等提交后报错。
  export function isBuiltinAdmin(row) {
    return row.username === "admin";
  }
  export async function userPage(mount, ui) {
    const canWrite = ui.can("user.write");
    let rows = [];
    let editing = null;
    let mode = "create";

    mount.innerHTML = `
      <div class="page-head">
        <div>
          <h2>用户</h2>
          <p class="page-sub">账号、角色与口令。内置管理员 admin 不能改名、停用、删除或降级。</p>
        </div>
        ${canWrite ? '<button class="btn primary" data-role="new">新建用户</button>' : ""}
      </div>
      <div class="card" data-role="form-card" hidden>
        <form class="grid-form" data-role="form"></form>
        <div class="form-actions">
          <button class="btn primary" type="button" data-role="save">保存</button>
          <button class="btn" type="button" data-role="cancel">取消</button>
        </div>
      </div>
      <div class="card"><table class="table" data-role="table"></table></div>`;

    const formCard = mount.querySelector('[data-role="form-card"]');
    const form = mount.querySelector('[data-role="form"]');
    const table = mount.querySelector('[data-role="table"]');

    function roleOptions(selected) {
      return USER_ROLES.map(
        (role) =>
          `<option value="${role.value}" ${
            role.value === selected ? "selected" : ""
          }>${role.label}</option>`
      ).join("");
    }

    function openForm(row) {
      editing = row || null;
      mode = row ? "edit" : "create";
      const locked = row ? isBuiltinAdmin(row) : false;
      form.innerHTML = `
        <label class="field"><span>用户名 *</span>
          <input type="text" data-field="username" value="${escapeHtml(
            row ? row.username : ""
          )}" ${locked ? "disabled" : ""} required />
        </label>
        <label class="field"><span>显示名</span>
          <input type="text" data-field="display_name" value="${escapeHtml(
            row ? row.display_name : ""
          )}" />
        </label>
        <label class="field"><span>角色</span>
          <select data-field="role" ${locked ? "disabled" : ""}>${roleOptions(
        row ? row.role : "viewer"
      )}</select>
        </label>
        <label class="checkbox">
          <input type="checkbox" data-field="active" ${
            !row || row.active ? "checked" : ""
          } ${locked ? "disabled" : ""} /> 启用
        </label>
        ${
          row
            ? ""
            : `<label class="field"><span>初始口令 *</span>
                 <input type="password" data-field="password" autocomplete="new-password" required />
               </label>`
        }
        ${
          locked
            ? '<p class="page-sub">内置管理员只能修改显示名与口令：角色固定为管理员，账号不能停用或删除。</p>'
            : ""
        }`;
      formCard.hidden = false;
    }

    function openPassword(row) {
      editing = row;
      mode = "password";
      form.innerHTML = `
        <label class="field"><span>为 ${escapeHtml(row.username)} 设置新口令（8–128 个字符）</span>
          <input type="password" data-field="password" autocomplete="new-password" required />
        </label>`;
      formCard.hidden = false;
    }

    function renderTable() {
      if (!rows.length) {
        table.innerHTML = '<tbody><tr><td class="empty">暂无用户</td></tr></tbody>';
        return;
      }
      const body = rows
        .map((row) => {
          const builtin = isBuiltinAdmin(row);
          const lockTitle = builtin ? ' title="内置管理员不能停用、删除或降级"' : "";
          const actions = canWrite
            ? `<button class="link" data-role="edit">编辑</button>
               <button class="link" data-role="password">重置口令</button>
               ${
                 builtin
                   ? `<button class="link" disabled${lockTitle}>${row.active ? "停用" : "启用"}</button>
                      <button class="link danger" disabled${lockTitle}>删除</button>`
                   : `<button class="link" data-role="toggle">${row.active ? "停用" : "启用"}</button>
                      <button class="link danger" data-role="delete">删除</button>`
               }`
            : "";
          const status = builtin
            ? `${row.active ? "启用" : "停用"}<br /><span class="page-sub">内置账号，不可停用或删除</span>`
            : row.active
            ? "启用"
            : "停用";
          return `<tr data-id="${row.id}">
            <td>${escapeHtml(row.username)}</td>
            <td>${escapeHtml(row.display_name)}</td>
            <td>${escapeHtml(roleLabel(row.role))}</td>
            <td>${status}</td>
            <td>${escapeHtml(row.updated_at || row.created_at)}</td>
            <td class="actions">${actions}</td>
          </tr>`;
        })
        .join("");
      table.innerHTML = `<thead><tr><th>用户名</th><th>显示名</th><th>角色</th><th>状态</th><th>最后修改</th><th class="actions">操作</th></tr></thead><tbody>${body}</tbody>`;
    }

    async function reload() {
      rows = await api("GET", "/api/users");
      renderTable();
    }

    const newButton = mount.querySelector('[data-role="new"]');
    if (newButton) {
      newButton.addEventListener("click", () => openForm(null));
    }
    mount.querySelector('[data-role="cancel"]').addEventListener("click", () => {
      formCard.hidden = true;
    });

    mount.querySelector('[data-role="save"]').addEventListener("click", async () => {
      const field = (name) => form.querySelector('[data-field="' + name + '"]');
      try {
        if (mode === "password") {
          const password = field("password").value;
          if (!password) {
            showNotice("口令不能为空", "error");
            return;
          }
          await api("PUT", "/api/users/" + editing.id, {
            username: editing.username,
            display_name: editing.display_name,
            role: editing.role,
            active: editing.active,
            password: password,
          });
        } else {
          const payload = {
            username: field("username").value.trim(),
            display_name: field("display_name").value.trim(),
            role: field("role").value,
            active: field("active").checked,
          };
          if (!payload.username) {
            showNotice("用户名不能为空", "error");
            return;
          }
          if (mode === "create") {
            payload.password = field("password").value;
            if (!payload.password) {
              showNotice("请填写初始口令", "error");
              return;
            }
            await api("POST", "/api/users", payload);
          } else {
            await api("PUT", "/api/users/" + editing.id, payload);
          }
        }
        formCard.hidden = true;
        showNotice("");
        await reload();
      } catch (error) {
        showNotice(error.message, "error");
      }
    });

    table.addEventListener("click", async (event) => {
      const button = event.target.closest("button");
      if (!button || button.disabled) return;
      const row = rows.find(
        (item) => item.id === Number(event.target.closest("tr").dataset.id)
      );
      if (!row) return;
      const action = button.dataset.role;
      if (action === "edit") {
        openForm(row);
      } else if (action === "password") {
        openPassword(row);
      } else if (action === "toggle") {
        try {
          await api("PUT", "/api/users/" + row.id, {
            username: row.username,
            display_name: row.display_name,
            role: row.role,
            active: !row.active,
          });
          await reload();
        } catch (error) {
          showNotice(error.message, "error");
        }
      } else if (action === "delete") {
        if (!(await askConfirm("确认删除用户 " + row.username + "？该账号会立即退出登录。"))) return;
        try {
          await api("DELETE", "/api/users/" + row.id);
          await reload();
        } catch (error) {
          showNotice(error.message, "error");
        }
      }
    });

    await reload();
  }

  // ---------- 审计日志 ----------
  export const AUDIT_ENTITIES = [
    ["", "全部对象"],
    ["document", "单据"],
    ["doc_header", "单据（信用额度）"],
    ["item", "商品"],
    ["item_category", "商品分类"],
    ["unit", "计量单位"],
    ["warehouse", "仓库"],
    ["customer", "客户"],
    ["supplier", "供应商"],
    ["employee", "职员"],
    ["settlement", "核销单"],
    ["fund_account", "资金账户"],
    ["print_template", "打印模板"],
    ["user", "用户"],
    ["auth", "登录"],
    ["report", "报表"],
    ["database", "数据库"],
    ["setting", "系统参数"],
    ["opening_stock", "库存期初"],
    ["opening_receivable", "应收期初"],
    ["opening_payable", "应付期初"],
    ["opening_fund", "资金期初"],
  ];
  export const AUDIT_ACTIONS = {
    create: "新建",
    update: "修改",
    delete: "删除",
    save_draft: "存草稿",
    update_draft: "修改草稿",
    delete_draft: "删除草稿",
    approve: "审核",
    reverse: "反审核",
    convert_order: "订单转出库",
    close_order: "关闭订单",
    set_password: "重置口令",
    change_password: "修改口令",
    account_locked: "账号锁定",
    login: "登录",
    login_failed: "登录失败",
    export: "导出",
    backup: "备份",
    restore: "恢复",
    import: "导入",
    credit_over_limit: "超信用额度",
    role_change: "角色变更",
  };
  export const AUDIT_ROLES = { admin: "管理员", operator: "操作员", viewer: "只读" };
  export const AUDIT_SETTING_KEYS = {
    company_name: "公司名称",
    company_phone: "联系电话",
    company_address: "公司地址",
  };
  export function auditEntityLabel(code) {
    const found = AUDIT_ENTITIES.find(([value]) => value === code);
    return found ? found[1] : code;
  }
  export function auditActionLabel(row) {
    if (row.action === "reverse") {
      return row.entity === "settlement" ? "反核销" : "反审核";
    }
    return AUDIT_ACTIONS[row.action] || row.action;
  }
  export function auditRoleLabel(code) {
    return AUDIT_ROLES[code] || code || "—";
  }
  export function auditMoney(cents) {
    const value = Number(cents || 0) / 100;
    return (
      value.toLocaleString("zh-CN", { minimumFractionDigits: 2, maximumFractionDigits: 2 }) +
      " 元"
    );
  }
  export function auditHasValue(value) {
    return value !== undefined && value !== null && String(value).trim() !== "";
  }

  // 摘要只做展示层翻译：detail 仍按后端写入的 JSON/纯文本保存，
  // 未知结构或解析失败一律原样显示，不吞内容也不报错。
  export function auditDetailText(row, accountNames) {
    const raw = row.detail || "";
    if (!raw) {
      return row.action === "reverse" ? "红字回冲" : "—";
    }
    let data = null;
    try {
      data = JSON.parse(raw);
    } catch (error) {
      return raw;
    }
    if (!data || typeof data !== "object" || Array.isArray(data)) {
      return raw;
    }
    if (data.doc_type && ["approve", "save_draft", "update_draft"].includes(row.action)) {
      const parts = [bizTypeText(data.doc_type)];
      if (auditHasValue(data.qty)) parts.push("数量 " + data.qty);
      if (auditHasValue(data.amount_cents)) parts.push("金额 " + auditMoney(data.amount_cents));
      return parts.join(" · ");
    }
    if (row.action === "convert_order" && data.out_doc_no) {
      return (
        "转出库 " + data.out_doc_no + (auditHasValue(data.qty) ? " · 数量 " + data.qty : "")
      );
    }
    if (row.entity === "settlement" && row.action === "create") {
      const parts = [Number(data.direction) === -1 ? "付款" : "收款"];
      if (auditHasValue(data.amount_cents)) parts.push("金额 " + auditMoney(data.amount_cents));
      if (auditHasValue(data.account_id)) {
        const id = Number(data.account_id);
        const name = id > 0 ? accountNames.get(id) : "";
        parts.push("账户 " + (name || (id > 0 ? "#" + id : "未指定")));
      }
      return parts.join(" · ");
    }
    if (row.entity === "settlement" && row.action === "reverse") {
      return "原单 " + (data.reverses || "—") + "（红字冲回）";
    }
    if (row.entity === "user") {
      if (row.action === "role_change") {
        return "角色从 " + auditRoleLabel(data.from) + " 改为 " + auditRoleLabel(data.to);
      }
      if (row.action === "create" || row.action === "update") {
        const parts = [];
        if (auditHasValue(data.role)) parts.push("角色 " + auditRoleLabel(data.role));
        if (typeof data.active === "boolean") parts.push(data.active ? "启用" : "停用");
        return parts.length ? parts.join(" · ") : raw;
      }
      return raw;
    }
    if (row.entity === "setting" && row.action === "update") {
      const label = AUDIT_SETTING_KEYS[row.entity_no] || row.entity_no || "参数";
      const from = auditHasValue(data.from) ? data.from : "未设置";
      const to = auditHasValue(data.to) ? data.to : "未设置";
      return label + "从 " + from + " 改为 " + to;
    }
    return raw;
  }
  export async function auditPage(mount) {
    let page = 1;
    let pages = 1;
    const pageSize = 50;

    const entityOptions = AUDIT_ENTITIES.map(
      ([value, label]) => `<option value="${value}">${label}</option>`
    ).join("");

    mount.innerHTML = `
      <div class="page-head">
        <div>
          <h2>审计日志</h2>
          <p class="page-sub">只读记录：谁在何时对什么对象做了什么</p>
        </div>
      </div>
      <div class="card">
        <form class="grid-form" data-role="filters">
          <label class="field"><span>开始日期</span><input type="date" data-field="from" /></label>
          <label class="field"><span>结束日期</span><input type="date" data-field="to" /></label>
          <label class="field"><span>对象类型</span><select data-field="entity">${entityOptions}</select></label>
          <label class="field"><span>对象 id</span><input type="number" min="0" step="1" data-field="entity_id" /></label>
          <label class="field"><span>操作人</span><input type="text" data-field="actor" placeholder="用户名" /></label>
          <label class="field"><span>来源 IP</span><input type="text" data-field="client_ip" placeholder="如 192.168.1.20" /></label>
          <label class="field"><span>请求号</span><input type="text" data-field="request_id" placeholder="排障用请求标识" /></label>
          <div class="form-actions">
            <button class="btn primary" type="submit">查询</button>
            <button class="btn" type="button" data-role="reset">重置</button>
          </div>
        </form>
      </div>
      <div class="card">
        <table class="table" data-role="table"></table>
        <div class="form-actions">
          <button class="btn" type="button" data-role="prev">上一页</button>
          <span class="page-sub" data-role="page-info"></span>
          <button class="btn" type="button" data-role="next">下一页</button>
          <button class="btn" type="button" data-role="export-csv">导出 CSV</button>
          <button class="btn" type="button" data-role="export-xlsx">导出 Excel</button>
        </div>
      </div>`;

    const form = mount.querySelector('[data-role="filters"]');
    const table = mount.querySelector('[data-role="table"]');
    const info = mount.querySelector('[data-role="page-info"]');
    const prevButton = mount.querySelector('[data-role="prev"]');
    const nextButton = mount.querySelector('[data-role="next"]');

    const field = (name) => form.querySelector('[data-field="' + name + '"]');

    const filterNames = ["from", "to", "entity", "entity_id", "actor", "client_ip", "request_id"];
    const filterParams = () => {
      const params = new URLSearchParams();
      for (const name of filterNames) {
        const value = field(name).value.trim();
        if (value) params.set(name, value);
      }
      return params;
    };

    // 核销审计只存 account_id，页面尽量换成账户名；没有权限或取不到时退回 #id。
    const accountNames = new Map();
    api("GET", "/api/fund-accounts")
      .then((result) => {
        const rows = Array.isArray(result) ? result : result.rows || [];
        for (const account of rows) {
          accountNames.set(Number(account.id), account.name || account.code || "#" + account.id);
        }
      })
      .catch(() => {});

    async function reload() {
      const params = filterParams();
      params.set("page", String(page));
      params.set("page_size", String(pageSize));
      try {
        const result = await api("GET", "/api/audit-logs?" + params.toString());
        const total = Number(result.total || 0);
        pages = Math.max(1, Math.ceil(total / Number(result.page_size || pageSize)));
        info.textContent = "第 " + result.page + " / " + pages + " 页 · 共 " + total + " 条";
        prevButton.disabled = result.page <= 1;
        nextButton.disabled = result.page >= pages;
        if (!result.items.length) {
          table.innerHTML = '<tbody><tr><td class="empty">没有符合条件的记录</td></tr></tbody>';
          return;
        }
        table.innerHTML = `<thead><tr><th>时间</th><th>操作人</th><th>对象</th><th>对象 id</th><th>动作</th><th>来源 IP</th><th>请求号</th><th>摘要</th></tr></thead><tbody>${result.items
          .map(
            (row) => `<tr>
              <td>${escapeHtml(row.at)}</td>
              <td>${escapeHtml(row.actor)}</td>
              <td>${escapeHtml(auditEntityLabel(row.entity))}${
              row.entity_no ? " " + escapeHtml(row.entity_no) : ""
            }</td>
              <td>${row.entity_id || "—"}</td>
              <td>${escapeHtml(auditActionLabel(row))}</td>
              <td>${row.client_ip ? escapeHtml(row.client_ip) : "—"}</td>
              <td>${row.request_id ? escapeHtml(row.request_id) : "—"}</td>
              <td class="audit-detail"><span class="audit-detail-text">${escapeHtml(
                auditDetailText(row, accountNames)
              )}</span></td>
            </tr>`
          )
          .join("")}</tbody>`;
      } catch (error) {
        showNotice(error.message, "error");
      }
    }

    form.addEventListener("submit", (event) => {
      event.preventDefault();
      page = 1;
      reload();
    });
    mount.querySelector('[data-role="reset"]').addEventListener("click", () => {
      for (const name of ["from", "to", "entity_id", "actor", "client_ip", "request_id"]) {
        field(name).value = "";
      }
      field("entity").value = "";
      page = 1;
      reload();
    });
    mount.querySelector('[data-role="export-csv"]').addEventListener("click", async () => {
      const params = filterParams();
      params.set("format", "csv");
      try {
        await downloadCsv("/api/audit-logs/export?" + params.toString());
        showNotice("已导出 CSV", "ok");
      } catch (error) {
        showNotice(error.message, "error");
      }
    });
    mount.querySelector('[data-role="export-xlsx"]').addEventListener("click", async () => {
      const params = filterParams();
      params.set("format", "xlsx");
      try {
        await downloadXlsx("/api/audit-logs/export?" + params.toString());
        showNotice("已导出 Excel", "ok");
      } catch (error) {
        showNotice(error.message, "error");
      }
    });
    prevButton.addEventListener("click", () => {
      if (page > 1) {
        page -= 1;
        reload();
      }
    });
    nextButton.addEventListener("click", () => {
      if (page < pages) {
        page += 1;
        reload();
      }
    });

    await reload();
  }

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

  // ---------- 关于与诊断 ----------
