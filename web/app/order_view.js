/* app/order_view.js：开单页（销售订单 / 采购订单）的页面骨架、明细行模板与列表列定义。
 * 由 order.js 拆出，只放不依赖页面闭包状态的视图代码，模板文本与拆分前一致。
 */
import { escapeHtml, money, qtyText } from "./core_format.js";

  // 开单页骨架：表头字段 + 明细表 + 转出库卡片 + 列表卡片。
  export function orderPageHtml(config) {
    const docType = config.docType;
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
            <label class="field"><span>${escapeHtml(config.warehouseLabel)}</span>
              <select data-role="warehouse" required></select>
            </label>
            <label class="field"><span>订单日期</span>
              <input type="date" data-role="date" required />
            </label>
            <label class="field"><span>${escapeHtml(config.partnerLabel)}</span>
              <select data-role="customer"></select>
            </label>
            ${
              docType === "SAL_ORDER"
                ? `<label class="field"><span>业务员</span>
                    <select data-role="salesperson">
                      <option value="0">未指定</option>
                    </select>
                  </label>`
                : ""
            }
          </div>
          <div class="doc-remark">
            <label class="field"><span>备注</span>
              <textarea data-role="remark" rows="2" maxlength="200" placeholder="选填，最多 200 个字"></textarea>
            </label>
          </div>
          <table class="table lines" data-role="lines"></table>
          <div class="form-actions">
            <button class="btn" type="button" data-role="add-line">添加明细行</button>
            <button class="btn" type="button" data-role="save-draft">存为草稿</button>
            <button class="btn primary" type="button" data-role="submit">保存并确认</button>
            <button class="btn" type="button" data-role="cancel-edit" hidden>取消编辑</button>
          </div>
        </form>
      </div>
      <div class="card" data-role="convert-card" hidden>
        <div class="card-head">
          <h3>${escapeHtml(config.convertTitle)}</h3>
          <span class="page-sub" data-role="convert-title"></span>
        </div>
        <div class="doc-head">
          <label class="field"><span>${escapeHtml(config.convertWarehouseLabel)}</span>
            <select data-role="convert-warehouse"></select>
          </label>
          <label class="field"><span>${escapeHtml(config.convertDateLabel)}</span>
            <input type="date" data-role="convert-date" />
          </label>
        </div>
        <table class="table" data-role="convert-lines"></table>
        <div class="form-actions">
          <button class="btn primary" type="button" data-role="convert-submit">${escapeHtml(
            config.convertSubmit
          )}</button>
          <button class="btn" type="button" data-role="convert-cancel">取消</button>
        </div>
      </div>
      <div class="card" data-role="doc-list"></div>`;
  }

  // 明细表头：商品、数量、单价、金额与操作列。
  export function orderLinesHead() {
    return `<thead><tr><th>商品</th><th>数量</th><th>单价（元）</th><th>金额（元）</th><th class="actions">操作</th></tr></thead><tbody></tbody>`;
  }

  // 单条明细行：商品下拉由页面提供选项渲染函数。
  export function orderLineRowHtml(line, itemOptions) {
    return `
        <td><select data-role="item">${itemOptions(line ? line.item_id : undefined)}</select></td>
        <td><span class="qty-cell"><input type="text" inputmode="decimal" value="1" data-role="qty" /><span class="unit-hint" data-role="unit"></span></span></td>
        <td><input type="number" min="0" step="0.01" value="0.00" data-role="price" /></td>
        <td><input type="text" class="readonly" value="—" data-role="amount" disabled /></td>
        <td class="actions"><button class="link danger" type="button" data-role="remove">移除</button></td>`;
  }

  // 转出库明细：商品名称由页面提供按 id 取名的渲染函数。
  export function orderConvertLinesHtml(lines, itemLabelById) {
    return `
          <thead><tr><th>商品</th><th>订单数量</th><th>已下推</th><th>剩余</th><th>本次数量</th><th>单价（元）</th></tr></thead>
          <tbody>${lines
            .map((line) => {
              const remaining = Number(line.remaining_qty);
              return `<tr data-line-id="${line.id}" data-price-cents="${line.price_cents}">
                <td>${escapeHtml(itemLabelById(line.item_id))}</td>
                <td>${qtyText(line.qty)}</td>
                <td>${qtyText(line.converted_qty)}</td>
                <td>${qtyText(line.remaining_qty)}</td>
                <td><input type="text" inputmode="decimal"
                  value="${qtyText(line.remaining_qty)}" data-role="convert-qty" ${remaining <= 0 ? "disabled" : ""} /></td>
                <td><input type="number" min="0" step="0.01"
                  value="${(line.price_cents / 100).toFixed(2)}" data-role="convert-price" ${
                    remaining <= 0 ? "disabled" : ""
                  } /></td>
              </tr>`;
            })
            .join("")}</tbody>`;
  }

  function orderState(doc) {
    if (doc.status === "DRAFT") return "DRAFT";
    if (doc.order_status === "CLOSED") return "CLOSED";
    if (doc.order_status === "CONVERTED") return "CONVERTED";
    if (BigInt(doc.converted_qty || 0) > 0n) return "PARTIAL";
    return "CONFIRMED";
  }

  export function orderStatusLabel(doc, config) {
    const state = orderState(doc);
    if (state === "DRAFT") return "草稿";
    if (state === "CLOSED") return "已关闭";
    if (state === "CONVERTED") return config.convertedLabel;
    if (state === "PARTIAL") return config.partialLabel;
    return "已确认";
  }

  export function orderActionsFor(doc, config) {
    const print =
      '<button class="link" data-role="print">打印</button>' +
      '<button class="link" data-role="pdf">导出 PDF</button>';
    if (doc.status === "DRAFT") {
      return (
        print +
        '<button class="link" data-role="approve">确认</button>' +
        '<button class="link" data-role="edit">编辑</button>' +
        '<button class="link danger" data-role="delete">删除</button>'
      );
    }
    if (doc.order_status === "CONFIRMED") {
      return (
        print +
        `<button class="link" data-role="convert">${escapeHtml(config.convertAction)}</button>` +
        '<button class="link danger" data-role="close">关闭</button>'
      );
    }
    return print;
  }

  // 订单列表列定义：业务员列仅销售订单显示。
  export function orderListColumns(config, docType) {
    return [
      { title: "订单号", cell: (doc) => escapeHtml(doc.doc_no) },
      { title: "日期", cell: (doc) => escapeHtml(doc.doc_date) },
      { title: "仓库", cell: (doc) => escapeHtml(doc.warehouse_name) },
      { title: config.partnerLabel, cell: (doc) => escapeHtml(doc.customer || "—") },
      ...(docType === "SAL_ORDER"
        ? [
            {
              title: "业务员",
              cell: (doc) => escapeHtml(doc.salesperson_name || "未指定"),
            },
          ]
        : []),
      { title: "数量", cell: (doc) => qtyText(doc.total_qty) },
      { title: "金额（元）", cell: (doc) => money(doc.total_amount_cents) },
      { title: "状态", cell: (doc) => orderStatusLabel(doc, config) },
      { title: "操作", actions: true, cell: (doc) => orderActionsFor(doc, config) },
    ];
  }
