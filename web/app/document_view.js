/* app/document_view.js：单据录入页的页面模板、明细行模板与列表列定义。
 * 由 document.js 拆出，只放不依赖页面闭包状态的视图代码，模板文本与拆分前一致。
 */
import { can } from "./core_state.js";
import { bizCategoryText, escapeHtml, money, qtyText } from "./core_format.js";

  // 单据页骨架：仓库/日期/往来单位/业务类型等表头字段 + 明细表 + 列表卡片。
  export function documentPageHtml(config, flags) {
    const { costAdjust, otherDoc, salesDoc } = flags;
    // 只有其他出入库有业务类型；入库可用期初，出库可用领用与报损，赠品两方向通用。
    const categoryOptions =
      config.docType === "OTH_IN"
        ? [
            ["GIFT", "赠品"],
            ["OPENING", "期初"],
            ["OTHER", "其他"],
          ]
        : [
            ["GIFT", "赠品"],
            ["USAGE", "领用"],
            ["LOSS", "报损"],
            ["OTHER", "其他"],
          ];

    return `
      <div class="page-head">
        <div>
          <h2>${escapeHtml(config.title)}</h2>
          <p class="page-sub">${escapeHtml(config.subtitle)}</p>
        </div>
        <button class="btn" data-role="refresh">刷新</button>
      </div>
      <div class="card">
        <form class="doc-form" data-role="form">
          <div class="doc-head">
            <label class="field" ${costAdjust ? "hidden" : ""}><span>${config.twoWarehouse ? "调出仓库" : "仓库"}</span>
              <select data-role="warehouse" required></select>
            </label>
            <label class="field" ${config.twoWarehouse ? "" : "hidden"}><span>调入仓库</span>
              <select data-role="target-warehouse" required></select>
            </label>
            <label class="field"><span>单据日期</span>
              <input type="date" data-role="date" required />
            </label>
            <label class="field" ${costAdjust ? "hidden" : ""}><span>${escapeHtml(config.partnerLabel)}</span>
              <input type="text" data-role="partner" placeholder="${escapeHtml(
                config.partnerPlaceholder
              )}" ${config.partnerApi ? "hidden" : ""} />
              <select data-role="partner-select" ${config.partnerApi ? "" : "hidden"}></select>
            </label>
            <label class="field" ${otherDoc ? "" : "hidden"}><span>业务类型</span>
              <select data-role="biz-category">
                ${categoryOptions
                  .map(([value, label]) => `<option value="${value}">${label}</option>`)
                  .join("")}
              </select>
            </label>
            <label class="field" ${salesDoc ? "" : "hidden"}><span>业务员</span>
              <select data-role="salesperson">
                <option value="0">未指定</option>
              </select>
            </label>
          </div>
          <div class="doc-remark">
            <label class="field"><span>备注</span>
              <textarea data-role="remark" rows="2" maxlength="200" placeholder="选填，最多 200 个字"></textarea>
            </label>
          </div>
          <table class="table lines" data-role="lines"></table>
          <p class="page-sub" data-role="doc-totals"></p>
          <div class="form-actions">
            <button class="btn" type="button" data-role="add-line">添加明细行</button>
            <button class="btn" type="button" data-role="save-draft">存为草稿</button>
            <button class="btn primary" type="button" data-role="submit">保存并审核</button>
            <button class="btn" type="button" data-role="cancel-edit" hidden>取消编辑</button>
          </div>
        </form>
      </div>
      <div class="card" data-role="doc-list"></div>`;
  }

  // 明细表头：成本调整只有调整金额与原因，其余单据带数量与税额列。
  export function documentLinesHead(costAdjust) {
    return costAdjust
      ? `<thead><tr><th>商品</th><th>仓库</th><th>调整金额（元）</th><th>调整原因</th><th class="actions">操作</th></tr></thead><tbody></tbody>`
      : `<thead><tr><th>商品</th><th>数量</th><th>单价（元）</th><th>金额（录入价）</th><th>税率（%）</th><th>含税</th><th>税额（元）</th><th>价税合计（元）</th><th class="actions">操作</th></tr></thead><tbody></tbody>`;
  }

  // 单条明细行：商品下拉与仓库下拉由页面提供选项渲染函数。
  export function documentLineRowHtml(line, costAdjust, itemOptions, warehouseOptions) {
    if (costAdjust) {
      return `
          <td><select data-role="item">${itemOptions(line ? line.item_id : undefined)}</select></td>
          <td><select data-role="line-warehouse">${warehouseOptions(
            line ? line.warehouse_id : undefined
          )}</select></td>
          <td><input type="text" inputmode="decimal" value="0.00" data-role="adjust" /></td>
          <td><input type="text" placeholder="调整原因" data-role="reason" /></td>
          <td class="actions"><button class="link danger" type="button" data-role="remove">移除</button></td>`;
    }
    return `
        <td><select data-role="item">${itemOptions(line ? line.item_id : undefined)}</select></td>
        <td><span class="qty-cell"><input type="text" inputmode="decimal" value="1" data-role="qty" /><span class="unit-hint" data-role="unit"></span></span></td>
        <td><input type="number" min="0" step="0.01" value="0.00" data-role="price" /></td>
        <td><input type="text" class="readonly" value="—" data-role="amount" disabled /></td>
        <td><input type="number" min="0" max="100" step="0.01" value="0" data-role="tax-rate" /></td>
        <td><label class="checkbox"><input type="checkbox" data-role="tax-included" /></label></td>
        <td><input type="text" class="readonly" value="—" data-role="tax" disabled /></td>
        <td><input type="text" class="readonly" value="—" data-role="with-tax" disabled /></td>
        <td class="actions"><button class="link danger" type="button" data-role="remove">移除</button></td>`;
  }

  export function statusLabel(status) {
    return { APPROVED: "已审核", DRAFT: "草稿", VOIDED: "已作废" }[status] || status;
  }

  export function actionsFor(doc, costAdjust) {
    const print =
      '<button class="link" data-role="print">打印</button>' +
      '<button class="link" data-role="pdf">导出 PDF</button>' +
      '<button class="link" data-role="export-csv">CSV</button>' +
      '<button class="link" data-role="export-xlsx">Excel</button>';
    if (costAdjust && !can("cost.adjust")) {
      return doc.status === "DRAFT"
        ? print + '<button class="link" data-role="edit">编辑</button>'
        : print;
    }
    if (doc.status === "DRAFT") {
      return (
        print +
        '<button class="link" data-role="approve">审核</button>' +
        '<button class="link" data-role="edit">编辑</button>' +
        '<button class="link danger" data-role="delete">删除</button>'
      );
    }
    if (doc.status === "APPROVED") {
      return print + '<button class="link danger" data-role="reverse">反审核</button>';
    }
    return print;
  }

  // 单据列表列定义：业务类型/业务员列按单据类型增删，金额列在成本调整下显示占位。
  export function documentListColumns(config, flags) {
    const { costAdjust, otherDoc, salesDoc } = flags;
    return [
      { title: "单据号", cell: (doc) => escapeHtml(doc.doc_no) },
      { title: "日期", cell: (doc) => escapeHtml(doc.doc_date) },
      { title: "仓库", cell: (doc) => escapeHtml(doc.warehouse_name) },
      { title: config.partnerLabel, cell: (doc) => escapeHtml(doc.customer || "—") },
      ...(otherDoc
        ? [
            {
              title: "业务类型",
              cell: (doc) => escapeHtml(bizCategoryText(doc.biz_category) || "—"),
            },
          ]
        : []),
      ...(salesDoc
        ? [
            {
              title: "业务员",
              cell: (doc) => escapeHtml(doc.salesperson_name || "未指定"),
            },
          ]
        : []),
      { title: "数量", cell: (doc) => (costAdjust ? "—" : qtyText(doc.total_qty)) },
      { title: "金额（元）", cell: (doc) => money(doc.total_amount_cents) },
      { title: "状态", cell: (doc) => statusLabel(doc.status) },
      { title: "操作", actions: true, cell: (doc) => actionsFor(doc, costAdjust) },
    ];
  }
