/* app/masterdata_item.js：商品页在通用 CRUD 之上的分类/条码列与筛选配置。
 * 由 masterdata.js 拆出；税率按百分数录入，提交时换回整数万分比。
 */
import { escapeHtml } from "./core_format.js";
import { api } from "./core_api.js";
import { crudPage } from "./masterdata.js";

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
