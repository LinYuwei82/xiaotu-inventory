/* app/masterdata_category.js：商品分类树页面（最多三层）。
 * 由 masterdata.js 拆出，供 pages.js 与入口共同加载。
 */
import { can, showNotice } from "./core_state.js";
import { escapeHtml } from "./core_format.js";
import { askConfirm } from "./core_dialog.js";
import { api } from "./core_api.js";

  // 商品分类树：数据库不限层，界面按产品约定只允许建到第三层。
  export async function itemCategoryPage(mount) {
    let rows = [];
    let editingId = 0;
    const writable = can("item.write");

    mount.innerHTML = `
      <div class="page-head">
        <div>
          <h2>商品分类</h2>
          <p class="page-sub">最多三层；停用分类不再出现在新建商品的下拉中，已归属的商品照常显示</p>
        </div>
        <div class="page-actions">
          <button class="btn" data-role="refresh">刷新</button>
          <button class="btn primary" data-role="new" ${writable ? "" : "hidden"}>新增一级分类</button>
        </div>
      </div>
      <div class="card" data-role="form-card" hidden>
        <form class="grid-form" data-role="form">
          <label class="field"><span>上级分类</span><select data-role="parent"></select></label>
          <label class="field"><span>分类名称 *</span><input type="text" data-role="name" required /></label>
          <label class="field"><span>排序</span><input type="number" step="1" data-role="sort" value="0" /></label>
          <label class="checkbox"><input type="checkbox" data-role="status" checked /> 启用</label>
        </form>
        <div class="form-actions">
          <button class="btn primary" type="button" data-role="save">保存</button>
          <button class="btn" type="button" data-role="cancel">取消</button>
        </div>
      </div>
      <div class="card"><table class="table" data-role="table"></table></div>`;

    const formCard = mount.querySelector('[data-role="form-card"]');
    const form = mount.querySelector('[data-role="form"]');
    const parentSelect = mount.querySelector('[data-role="parent"]');
    const nameInput = mount.querySelector('[data-role="name"]');
    const sortInput = mount.querySelector('[data-role="sort"]');
    const statusInput = mount.querySelector('[data-role="status"]');
    const table = mount.querySelector('[data-role="table"]');

    function renderTable() {
      if (!rows.length) {
        table.innerHTML = '<tbody><tr><td class="empty">还没有分类，先建一个一级分类</td></tr></tbody>';
        return;
      }
      table.innerHTML = `<thead><tr><th>分类名称</th><th>商品数</th><th>排序</th><th>状态</th><th class="actions">操作</th></tr></thead>
        <tbody>${rows
          .map((row) => {
            const indent = "　".repeat(Math.max(0, row.depth - 1));
            return `<tr data-id="${row.id}">
              <td>${indent}${escapeHtml(row.name)}</td>
              <td>${row.item_count}</td>
              <td>${row.sort_order}</td>
              <td>${row.status ? "启用" : "停用"}</td>
              <td class="actions">
                ${
                  writable && row.depth < 3
                    ? '<button class="link" data-role="add-child">新增子分类</button>'
                    : ""
                }
                ${writable ? '<button class="link" data-role="edit">编辑</button>' : ""}
                ${writable ? '<button class="link danger" data-role="delete">删除</button>' : ""}
              </td>
            </tr>`;
          })
          .join("")}</tbody>`;
    }

    function openForm(row, parentId) {
      editingId = row ? row.id : 0;
      // 第三层之下不再提供新建入口；编辑时保留当前上级，避免被下拉截断。
      const parents = rows.filter(
        (item) =>
          item.depth < 3 &&
          (!row || (item.id !== row.id && !item.path.startsWith(row.path)))
      );
      parentSelect.innerHTML =
        `<option value="0">一级分类</option>` +
        parents
          .map(
            (item) =>
              `<option value="${item.id}">${"　".repeat(item.depth - 1)}${escapeHtml(item.name)}</option>`
          )
          .join("");
      if (row) {
        parentSelect.value = String(row.parent_id);
        // 当前上级若因层级限制不在列表里，补一个选项，避免保存时被改到根。
        if (!parents.some((item) => item.id === row.parent_id) && row.parent_id !== 0) {
          parentSelect.insertAdjacentHTML(
            "beforeend",
            `<option value="${row.parent_id}">当前上级</option>`
          );
          parentSelect.value = String(row.parent_id);
        }
      } else {
        parentSelect.value = String(parentId || 0);
      }
      nameInput.value = row ? row.name : "";
      sortInput.value = row ? row.sort_order : 0;
      statusInput.checked = row ? row.status : true;
      formCard.hidden = false;
      nameInput.focus();
    }

    async function reload() {
      rows = await api("GET", "/api/item-categories");
      renderTable();
    }

    mount.querySelector('[data-role="new"]').addEventListener("click", () => openForm(null));
    mount.querySelector('[data-role="refresh"]').addEventListener("click", () => reload());
    mount.querySelector('[data-role="cancel"]').addEventListener("click", () => {
      formCard.hidden = true;
    });
    mount.querySelector('[data-role="save"]').addEventListener("click", async () => {
      const payload = {
        parent_id: Number(parentSelect.value || 0),
        name: nameInput.value.trim(),
        sort_order: Number(sortInput.value || 0),
        status: statusInput.checked,
      };
      if (!payload.name) {
        showNotice("分类名称不能为空", "error");
        return;
      }
      try {
        if (editingId) {
          await api("PUT", "/api/item-categories/" + editingId, payload);
        } else {
          await api("POST", "/api/item-categories", payload);
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
      if (!button) return;
      const tr = event.target.closest("tr");
      const id = Number(tr.dataset.id);
      const row = rows.find((item) => item.id === id);
      if (!row) return;
      if (button.dataset.role === "edit") {
        openForm(row);
      } else if (button.dataset.role === "add-child") {
        openForm(null, row.id);
      } else if (button.dataset.role === "delete") {
        if (!(await askConfirm("确认删除分类「" + row.name + "」？"))) return;
        try {
          await api("DELETE", "/api/item-categories/" + id);
          await reload();
        } catch (error) {
          showNotice(error.message, "error");
        }
      }
    });

    await reload();
  }
