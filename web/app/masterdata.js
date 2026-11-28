/* app/masterdata.js：基础资料页：通用 CRUD 列表/表单、商品、商品分类与期初库存。
 * 由原 web/app.js 按职责拆出，供本模块与入口共同加载。
 */
import { API_BASE, api, askAlert, askConfirm, can, csvField, downloadCsv, downloadXlsx, escapeHtml, invalidateMergedTraders, isWinWebview, params, saveText, showNotice, state } from "./core.js";
import { crudPageHtml, crudTableHtml, fieldInputHtml, mergeLogsHtml, mergePanelHtml, optionLabel } from "./masterdata_view.js";

  export async function crudPage(mount, def) {
    let rows = [];
    let editingId = 0;
    const optionCache = {};

    const io = def.importable ? def.permissions || {} : null;
    const isTrader = def.key === "customer" || def.key === "supplier";
    const ioButtons = io
      ? `${can(io.read) ? `<button class="btn" data-role="template">下载模板</button>
          <button class="btn" data-role="export-xlsx">导出 Excel</button>
          <button class="btn" data-role="export-csv">导出 CSV</button>` : ""}
        ${can(io.write) ? `<button class="btn" data-role="import">导入</button>` : ""}`
      : "";

    mount.innerHTML = crudPageHtml(def, {
      ioButtons,
      isTrader,
      mergeButton: can("customer.write"),
    });

    const formCard = mount.querySelector('[data-role="form-card"]');
    const form = mount.querySelector('[data-role="form"]');
    const table = mount.querySelector('[data-role="table"]');
    const filtersBar = mount.querySelector('[data-role="filters"]');

    async function renderFilters() {
      if (!def.filters) return;
      const parts = [];
      for (const filter of def.filters) {
        if (filter.optionsApi) {
          const options = await api("GET", filter.optionsApi);
          parts.push(`<label class="field"><span>${escapeHtml(filter.label)}</span>
            <select data-filter="${escapeHtml(filter.key)}">
              <option value="">${escapeHtml(filter.allLabel || "全部")}</option>
              ${filter.zeroLabel ? `<option value="0">${escapeHtml(filter.zeroLabel)}</option>` : ""}
              ${options
                .filter((option) => option.status !== false)
                .map(
                  (option) =>
                    `<option value="${option.id}">${escapeHtml(
                      "　".repeat(Math.max(0, (option.depth || 1) - 1)) + optionLabel(option)
                    )}</option>`
                )
                .join("")}
            </select>
          </label>`);
        } else if (filter.options) {
          parts.push(`<label class="field"><span>${escapeHtml(filter.label)}</span>
            <select data-filter="${escapeHtml(filter.key)}">
              <option value="">${escapeHtml(filter.allLabel || "全部")}</option>
              ${filter.options
                .map(
                  (option) =>
                    `<option value="${escapeHtml(option.value)}">${escapeHtml(option.label)}</option>`
                )
                .join("")}
            </select>
          </label>`);
        } else {
          parts.push(`<label class="field"><span>${escapeHtml(filter.label)}</span>
            <input type="text" data-filter="${escapeHtml(filter.key)}" placeholder="${escapeHtml(
            filter.placeholder || ""
          )}" />
          </label>`);
        }
      }
      parts.push('<button class="btn" type="button" data-role="filter-reset">重置</button>');
      filtersBar.innerHTML = parts.join("");
      filtersBar.addEventListener("change", () => reload());
      filtersBar.querySelector('[data-role="filter-reset"]').addEventListener("click", () => {
        filtersBar.querySelectorAll("[data-filter]").forEach((input) => {
          input.value = "";
        });
        reload();
      });
    }

    function filterQuery() {
      if (!def.filters) return "";
      const params = [];
      for (const filter of def.filters) {
        const input = filtersBar.querySelector('[data-filter="' + filter.key + '"]');
        const value = input ? input.value.trim() : "";
        if (value) params.push(encodeURIComponent(filter.key) + "=" + encodeURIComponent(value));
      }
      return params.length ? "?" + params.join("&") : "";
    }

    async function loadOptions() {
      for (const field of def.fields) {
        if (field.optionsApi && !optionCache[field.optionsApi]) {
          optionCache[field.optionsApi] = await api("GET", field.optionsApi);
        }
      }
    }

    function openForm(row) {
      editingId = row ? row.id : 0;
      form.innerHTML =
        def.fields
          .map((field) =>
            fieldInputHtml(field, row ? row[field.key] : field.default, optionCache)
          )
          .join("") + (def.formExtra ? def.formExtra(row) : "");
      formCard.hidden = false;
      if (def.bindFormExtra) {
        def.bindFormExtra(form, row);
      }
    }

    function readForm() {
      const payload = {};
      for (const field of def.fields) {
        const input = form.querySelector('[data-field="' + field.key + '"]');
        if (field.type === "bool") {
          payload[field.key] = input.checked;
        } else if (field.optionsApi) {
          payload[field.key] = Number(input.value || 0);
        } else if (field.type === "int") {
          payload[field.key] = Math.round(Number(input.value || 0) * (field.scale || 1));
        } else {
          payload[field.key] = input.value.trim();
        }
      }
      return payload;
    }

    function renderTable() {
      table.innerHTML = crudTableHtml(rows, def.columns);
    }

    async function reload() {
      rows = await api("GET", def.api + filterQuery());
      renderTable();
    }

    const mergeCard = mount.querySelector('[data-role="merge-card"]');
    const mergeBody = mount.querySelector('[data-role="merge-body"]');
    const mergeHint = mount.querySelector('[data-role="merge-hint"]');
    const mergeLog = mount.querySelector('[data-role="merge-log"]');
    let mergePreview = null;
    let mergePartners = [];
    let mergeLogs = [];

    function renderMergeLogs() {
      if (!mergeLog) return;
      mergeLog.innerHTML = mergeLogsHtml(mergeLogs);
    }

    async function openMergePanel() {
      if (!mergeCard) return;
      const role = def.key === "customer" ? "customer" : "supplier";
      mergeCard.hidden = false;
      mergeHint.textContent = "正在读取候选与合并记录…";
      try {
        const [partners, preview, logs] = await Promise.all([
          api("GET", "/api/partners?role=" + role),
          api("GET", "/api/partners/merge-preview"),
          api("GET", "/api/partners/merge-log?limit=10"),
        ]);
        mergePreview = preview;
        mergeLogs = logs;
        mergePartners = partners.filter((row) => !row.merged_into && row.status);
        mergeHint.textContent =
          `候选组 ${preview.groups} 组（高置信 ${preview.high}、中高 ${preview.medium}、` +
          `仅名称 ${preview.name_only}）。仅名称相同属低置信，不会自动合并，请逐组核对。`;
        mergeBody.innerHTML = mergePanelHtml(mergePartners, def.key);
        renderMergeLogs();
      } catch (error) {
        mergeHint.textContent = "读取失败：" + error.message;
      }
    }

    if (mergeCard) {
      mergeCard.addEventListener("click", async (event) => {
        const button = event.target.closest("button");
        if (!button) return;
        if (button.dataset.role === "merge-close") {
          mergeCard.hidden = true;
          return;
        }
        if (button.dataset.role === "merge-run") {
          const sources = Array.from(
            mergeBody.querySelectorAll("input[data-merge-source]:checked")
          )
            .map((input) => Number(input.value))
            .filter(Boolean);
          const targetInput = mergeBody.querySelector(
            `input[name="merge-target-${def.key}"]:checked`
          );
          const target = targetInput ? Number(targetInput.value) : 0;
          if (!target) {
            showNotice("请选择保留档案", "error");
            return;
          }
          const selected = sources.filter((id) => id !== target);
          if (!selected.length) {
            showNotice("请至少勾选一条要并入的档案，且不能只选保留档案", "error");
            return;
          }
          const byId = new Map(mergePartners.map((row) => [row.id, row]));
          const names = selected.map((id) => (byId.get(id) || {}).name || "#" + id);
          const targetName = (byId.get(target) || {}).name || "#" + target;
          const candidates = (mergePreview && mergePreview.candidates) || [];
          const matched = candidates.filter((group) =>
            (group.rows || []).some((row) => names.includes(row.name))
          );
          const matchText = matched.length
            ? matched.map((group) => group.match + "：" + group.match_value).join("；")
            : "未命中自动候选，按人工选择合并";
          const ok = await askConfirm(
            `将把以下 ${selected.length} 条档案合并到「${targetName}」：\n` +
              names.join("、") +
              `\n\n预览结果：${matchText}\n\n` +
              "合并后这些档案不再出现在列表与下拉，历史台账、账龄与对账单归到保留档案。确认继续？",
            { okText: "确认合并" }
          );
          if (!ok) return;
          try {
            const result = await api("POST", "/api/partners/merge", {
              target_id: target,
              source_ids: selected,
            });
            invalidateMergedTraders();
            mergeCard.hidden = true;
            showNotice(`已合并 ${selected.length} 条到「${result.target_name || targetName}」`);
            await reload();
          } catch (error) {
            showNotice(error.message, "error");
            await openMergePanel();
          }
          return;
        }
        if (button.dataset.role === "undo") {
          const logId = Number(button.dataset.log);
          const ok = await askConfirm(
            `撤销合并到「${button.dataset.name || ""}」？\n` +
              "仅在合并后没有新业务写入被合并档案时才允许撤销。",
            { okText: "确认撤销" }
          );
          if (!ok) return;
          try {
            await api("POST", "/api/partners/undo", { log_id: logId });
            invalidateMergedTraders();
            mergeCard.hidden = true;
            showNotice("已撤销合并");
            await reload();
          } catch (error) {
            showNotice(error.message, "error");
            await openMergePanel();
          }
        }
      });
      const openButton = mount.querySelector('[data-role="merge-open"]');
      if (openButton) openButton.addEventListener("click", openMergePanel);
    }

    await renderFilters();

    if (io) {
      async function showImportErrors(payload) {
        const errors = payload.errors || [];
        const lines = errors
          .slice(0, 100)
          .map((item) => `第 ${item.row} 行 · ${item.column}：${item.reason}`);
        const more = errors.length > 100 ? `\n…另有 ${errors.length - 100} 条未显示` : "";
        const download = await askConfirm(
          `${payload.message}\n\n${lines.join("\n")}${more}\n\n是否下载完整错误清单？`,
          { okText: "下载清单", cancelText: "关闭" }
        );
        if (!download) return;
        const csv = ["行号,列名,原因"]
          .concat(errors.map((item) => [item.row, item.column, item.reason].map(csvField).join(",")))
          .join("\r\n");
        saveText(def.key + "-import-errors.csv", csv);
      }

      const templateButton = mount.querySelector('[data-role="template"]');
      if (templateButton) {
        templateButton.addEventListener("click", async () => {
          try {
            await downloadXlsx(def.api + "/template");
          } catch (error) {
            showNotice(error.message, "error");
          }
        });
      }
      const exportXlsx = mount.querySelector('[data-role="export-xlsx"]');
      if (exportXlsx) {
        exportXlsx.addEventListener("click", async () => {
          try {
            await downloadXlsx(def.api + "/export?format=xlsx");
          } catch (error) {
            showNotice(error.message, "error");
          }
        });
      }
      const exportCsv = mount.querySelector('[data-role="export-csv"]');
      if (exportCsv) {
        exportCsv.addEventListener("click", async () => {
          try {
            await downloadCsv(def.api + "/export?format=csv");
          } catch (error) {
            showNotice(error.message, "error");
          }
        });
      }

      const importButton = mount.querySelector('[data-role="import"]');
      if (importButton) {
        const fileInput = document.createElement("input");
        fileInput.type = "file";
        fileInput.accept = ".xlsx";
        fileInput.hidden = true;
        mount.append(fileInput);
        // 处理导入主逻辑：接收一个 File 对象
        const runImport = async (file) => {
          const keyLabel = def.key === "item" ? "商品编码" : "编码";
          const confirmed = await askConfirm(
            `将从「${file.name}」导入${def.title}。\n\n` +
              "· 导入前会自动备份数据库，备份失败则中止导入；\n" +
              "· 空单元格一律保持原值，已有记录的「启用」留空不会被改成启用；\n" +
              `· 按${keyLabel}匹配，已存在则更新，不存在则新增；\n` +
              "· 单次最多 5000 行、5MB，任何一行有错误都会整批拒绝。\n\n" +
              "确认开始导入？",
            { okText: "开始导入", cancelText: "取消" }
          );
          if (!confirmed) return;
          importButton.disabled = true;
          importButton.textContent = "导入中…";
          try {
            const headers = {
              "Content-Type":
                "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
              "X-File-Name": encodeURIComponent(file.name),
            };
            if (state.csrf) headers["X-CSRF-Token"] = state.csrf;
            const response = await fetch(API_BASE + def.api + "/import", {
              method: "POST",
              credentials: "include",
              headers,
              body: file,
            });
            let payload = null;
            try {
              payload = await response.json();
            } catch (error) {
              payload = null;
            }
            if (response.ok) {
              showNotice(
                `导入完成：新增 ${payload.inserted} 条，更新 ${payload.updated} 条；` +
                  `导入前已备份为 ${payload.backup}。`
              );
              await reload();
              return;
            }
            if (payload && payload.errors && payload.errors.length) {
              await showImportErrors(payload);
              return;
            }
            throw new Error(
              payload && payload.message ? payload.message : "导入失败（HTTP " + response.status + "）"
            );
          } catch (error) {
            showNotice(error.message, "error");
          } finally {
            importButton.disabled = false;
            importButton.textContent = "导入";
          }
        };
        importButton.addEventListener("click", async () => {
          // Windows WebView2：走原生打开对话框
          if (isWinWebview()) {
            try {
              const nativeIo = await window.chrome.webview.hostObjects.native_io;
              const resultJson = await nativeIo.openFile("Excel 文件", "*.xlsx");
              const result = JSON.parse(resultJson);
              if (!result.ok) return;  // 用户取消
              // base64 → Blob → File（保留原文件名）
              const binary = atob(result.base64);
              const bytes = new Uint8Array(binary.length);
              for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
              const file = new File([bytes], result.name, {
                type: "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
              });
              await runImport(file);
            } catch (error) {
              showNotice(error.message, "error");
            }
            return;
          }
          fileInput.value = "";
          fileInput.click();
        });
        fileInput.addEventListener("change", async () => {
          const file = fileInput.files && fileInput.files[0];
          if (!file) return;
          await runImport(file);
        });
      }
    }

    mount.querySelector('[data-role="new"]').addEventListener("click", async () => {
      await loadOptions();
      openForm(null);
    });
    mount.querySelector('[data-role="cancel"]').addEventListener("click", () => {
      formCard.hidden = true;
    });
    mount.querySelector('[data-role="save"]').addEventListener("click", async () => {
      const payload = readForm();
      for (const field of def.fields) {
        if (field.required && !payload[field.key]) {
          showNotice(field.label + "不能为空", "error");
          return;
        }
        if (field.type === "int") {
          const value = payload[field.key];
          const tooSmall = field.min !== undefined && value < field.min;
          const tooLarge = field.max !== undefined && value > field.max;
          if (!Number.isInteger(value) || tooSmall || tooLarge) {
            const range =
              field.min !== undefined && field.max !== undefined
                ? "应在 " + field.min + "–" + field.max + " 之间"
                : "应为整数";
            showNotice(field.label + range, "error");
            return;
          }
        }
      }
      if (def.beforeSave) {
        const problem = def.beforeSave(payload);
        if (problem) {
          showNotice(problem, "error");
          return;
        }
      }
      try {
        let saved = null;
        if (editingId) {
          saved = await api("PUT", def.api + "/" + editingId, payload);
        } else {
          saved = await api("POST", def.api, payload);
        }
        if (def.afterSave) {
          await def.afterSave(saved, payload, editingId);
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
      const row = event.target.closest("tr");
      const id = Number(row.dataset.id);
      if (button.dataset.role === "edit") {
        await loadOptions();
        openForm(rows.find((item) => item.id === id));
      } else if (button.dataset.role === "delete") {
        if (!(await askConfirm("确认删除这条记录？"))) return;
        try {
          await api("DELETE", def.api + "/" + id);
          await reload();
        } catch (error) {
          showNotice(error.message, "error");
        }
      }
    });

    await reload();
  }

  // 商品页在通用 CRUD 之上补分类/条码列与筛选；税率按百分数录入，提交时换回整数万分比。
  // 条码支持一商品多码：表单里逐行维护，主码排第一并同步到 item.barcode。
  export function itemPage(mount, def) {
    const fields = def.fields
      .filter((field) => field.key !== "barcode")
      .map((field) =>
        field.key === "tax_rate_bp" ? { ...field, label: "税率（%）", scale: 100 } : field
      );
    let pendingBarcodes = [];

    function barcodeRowHtml(code, primary) {
      return `<div class="barcode-row" data-role="barcode-row">
          <input type="text" data-code value="${escapeHtml(code || "")}" placeholder="条码" />
          <label class="inline-check"><input type="radio" name="item-primary-barcode" data-primary ${primary ? "checked" : ""} /> 主码</label>
          <button class="link danger" type="button" data-role="remove-barcode">删除</button>
        </div>`;
    }

    function readBarcodes(form) {
      const rows = Array.from(form.querySelectorAll('[data-role="barcode-row"]'));
      const codes = [];
      let primary = -1;
      for (const row of rows) {
        const value = row.querySelector("[data-code]").value.trim();
        if (!value) continue;
        if (row.querySelector("[data-primary]").checked && primary < 0) {
          primary = codes.length;
        }
        codes.push(value);
      }
      if (primary > 0) {
        codes.unshift(codes.splice(primary, 1)[0]);
      }
      return codes;
    }

    return crudPage(mount, {
      ...def,
      columns: [
        { key: "code", label: "编码", type: "string" },
        { key: "name", label: "名称", type: "string" },
        { key: "spec", label: "规格", type: "string" },
        { key: "category_name", label: "分类", type: "string" },
        { key: "barcode", label: "主条码", type: "string" },
        { key: "unit", label: "单位", type: "string" },
        { key: "tax_rate_bp", label: "税率", type: "percent_bp" },
        { key: "status", label: "启用", type: "bool" },
      ],
      fields,
      formExtra() {
        return `<div class="field" data-role="barcode-editor">
            <span>条码（主码 + 备用码）</span>
            <div data-role="barcode-rows"></div>
            <button class="btn" type="button" data-role="add-barcode">添加条码</button>
            <span class="page-sub">选中「主码」的那一行会同步到商品条码字段；扫码可命中任意一个码。</span>
          </div>`;
      },
      bindFormExtra(form, row) {
        const rows = form.querySelector('[data-role="barcode-rows"]');
        const codes =
          row && Array.isArray(row.barcodes) && row.barcodes.length
            ? row.barcodes
            : [row && row.barcode ? row.barcode : ""];
        rows.innerHTML = codes
          .map((code, index) => barcodeRowHtml(code, index === 0))
          .join("");
        form.querySelector('[data-role="add-barcode"]').addEventListener("click", () => {
          rows.insertAdjacentHTML("beforeend", barcodeRowHtml("", false));
        });
        rows.addEventListener("click", (event) => {
          const button = event.target.closest('[data-role="remove-barcode"]');
          if (!button) return;
          const rowElement = button.closest('[data-role="barcode-row"]');
          if (rows.querySelectorAll('[data-role="barcode-row"]').length <= 1) {
            rowElement.querySelector("[data-code]").value = "";
            return;
          }
          rowElement.remove();
        });
      },
      beforeSave(payload) {
        const codes = readBarcodes(mount.querySelector('[data-role="form"]'));
        const seen = new Set();
        for (const code of codes) {
          if (seen.has(code)) {
            return "条码重复：" + code;
          }
          seen.add(code);
        }
        pendingBarcodes = codes;
        payload.barcode = codes[0] || "";
        return "";
      },
      async afterSave(saved) {
        if (!saved || !saved.id) return;
        await api("PUT", "/api/items/" + saved.id + "/barcodes", {
          barcodes: pendingBarcodes,
        });
      },
      filters: [
        {
          key: "category_id",
          label: "分类",
          optionsApi: "/api/item-categories",
          allLabel: "全部分类",
          zeroLabel: "未分类",
        },
        { key: "q", label: "搜索", placeholder: "编码 / 名称 / 条码" },
      ],
    });
  }

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

  // 期初导入：库存 / 应收 / 应付 / 资金账户四类，有业务流水后服务端会拒绝。
  export async function openingPage(mount) {
    let status = [];
    mount.innerHTML = `
      <div class="page-head">
        <div>
          <h2>期初导入</h2>
          <p class="page-sub">只做库存、应收、应付、资金账户四类期初；有业务流水后不再允许导入，需要重来须清空数据目录重建</p>
        </div>
        <button class="btn" data-role="refresh">刷新</button>
      </div>
      <div data-role="list"></div>`;
    const list = mount.querySelector('[data-role="list"]');

    function render() {
      list.innerHTML = status
        .map(
          (item) => `<div class="card">
            <div class="card-head">
              <h3>${escapeHtml(item.label)}</h3>
              <span class="page-sub">${item.allowed ? "可导入" : escapeHtml(item.reason)}</span>
            </div>
            <div class="form-actions">
              <button class="btn" data-role="template" data-kind="${escapeHtml(item.kind)}">下载模板</button>
              <button class="btn primary" data-role="import" data-kind="${escapeHtml(item.kind)}"
                ${item.allowed ? "" : "disabled"}>导入</button>
            </div>
          </div>`
        )
        .join("");
    }

    async function reload() {
      status = await api("GET", "/api/openings/status");
      render();
    }

    list.addEventListener("click", async (event) => {
      const button = event.target.closest("button");
      if (!button) return;
      const kind = button.dataset.kind;
      if (button.dataset.role === "template") {
        try {
          await downloadXlsx("/api/openings/" + kind + "/template");
        } catch (error) {
          showNotice(error.message, "error");
        }
        return;
      }
      if (button.dataset.role === "import") {
        // 共用导入处理函数：接收一个 File 对象
        const runOpeningImport = async (file) => {
          const confirmed = await askConfirm(
            `将从「${file.name}」导入期初。\n\n` +
              "· 导入前会自动备份数据库，备份失败则中止；\n" +
              "· 整批校验，任何一行有错误都不会写入；\n" +
              "· 库存期初会生成「其他入库 + 期初」并直接审核。\n\n确认开始导入？",
            { okText: "开始导入", cancelText: "取消" }
          );
          if (!confirmed) return;
          try {
            const headers = {
              "Content-Type":
                "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
              "X-File-Name": encodeURIComponent(file.name),
            };
            if (state.csrf) headers["X-CSRF-Token"] = state.csrf;
            const response = await fetch(API_BASE + "/api/openings/" + kind + "/import", {
              method: "POST",
              credentials: "include",
              headers,
              body: file,
            });
            let payload = null;
            try {
              payload = await response.json();
            } catch (error) {
              payload = null;
            }
            if (response.ok) {
              const docs = (payload.doc_nos || []).join("、");
              showNotice(
                `导入完成：写入 ${payload.inserted} 条；导入前已备份为 ${payload.backup}。` +
                  (docs ? "期初单据：" + docs : "")
              );
              await reload();
              return;
            }
            if (payload && payload.errors && payload.errors.length) {
              const lines = payload.errors
                .slice(0, 100)
                .map((item) => `第 ${item.row} 行 · ${item.column}：${item.reason}`);
              await askAlert(payload.message + "\n\n" + lines.join("\n"));
              return;
            }
            throw new Error(
              payload && payload.message ? payload.message : "导入失败（HTTP " + response.status + "）"
            );
          } catch (error) {
            showNotice(error.message, "error");
          }
        };
        // Windows WebView2：走原生打开对话框
        if (isWinWebview()) {
          try {
            const nativeIo = await window.chrome.webview.hostObjects.native_io;
            const resultJson = await nativeIo.openFile("Excel 文件", "*.xlsx");
            const result = JSON.parse(resultJson);
            if (!result.ok) return;
            const binary = atob(result.base64);
            const bytes = new Uint8Array(binary.length);
            for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
            const file = new File([bytes], result.name, {
              type: "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
            });
            await runOpeningImport(file);
          } catch (error) {
            showNotice(error.message, "error");
          }
          return;
        }
        const fileInput = document.createElement("input");
        fileInput.type = "file";
        fileInput.accept = ".xlsx";
        fileInput.hidden = true;
        mount.append(fileInput);
        fileInput.addEventListener("change", async () => {
          const file = fileInput.files && fileInput.files[0];
          fileInput.remove();
          if (!file) return;
          await runOpeningImport(file);
        });
        fileInput.click();
      }
    });

    mount.querySelector('[data-role="refresh"]').addEventListener("click", () => reload());
    await reload();
  }

  // ---------- 六类单据共用的列表：筛选条 + 服务端分页 + 服务端合计 ----------
