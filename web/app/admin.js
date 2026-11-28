/* app/admin.js：用户管理页。
 * 由原 web/app.js 按职责拆出；审计日志与系统参数已拆到 admin_audit.js / admin_settings.js。
 */
import { showNotice } from "./core_state.js";
import { escapeHtml } from "./core_format.js";
import { askConfirm } from "./core_dialog.js";
import { api } from "./core_api.js";
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
