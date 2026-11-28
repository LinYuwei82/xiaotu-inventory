/* app/documents.js：单据列表的筛选、排序与行渲染工具。
 * 由原 web/app.js 按职责拆出，供本模块与入口共同加载。
 */
import { params, showNotice } from "./core_state.js";
import { escapeHtml, money } from "./core_format.js";
import { api } from "./core_api.js";

  export const DOC_FILTER_PREFIX = "xt.docFilter.";
  export const DOC_FILTER_STATUS = ["", "draft", "approved", "reversed"];
  export function isoDateOrEmpty(value) {
    return typeof value === "string" && /^\d{4}-\d{2}-\d{2}$/.test(value) ? value : "";
  }

  // 读取上次条件：字段类型不对就整条丢弃，按默认条件（本月）处理。
  export function readSavedDocFilter(docType) {
    let data = null;
    try {
      const raw = localStorage.getItem(DOC_FILTER_PREFIX + docType);
      if (!raw) return null;
      data = JSON.parse(raw);
    } catch (error) {
      return null;
    }
    if (!data || typeof data !== "object" || Array.isArray(data)) return null;
    return {
      keyword: typeof data.keyword === "string" ? data.keyword : "",
      date_from: isoDateOrEmpty(data.date_from),
      date_to: isoDateOrEmpty(data.date_to),
      status: DOC_FILTER_STATUS.includes(data.status) ? data.status : "",
      amount_min: typeof data.amount_min === "string" ? data.amount_min : "",
      amount_max: typeof data.amount_max === "string" ? data.amount_max : "",
      amount_open: data.amount_open === true,
    };
  }
  export function writeSavedDocFilter(docType, filter) {
    try {
      localStorage.setItem(DOC_FILTER_PREFIX + docType, JSON.stringify(filter));
    } catch (error) {
      // 本地存储不可用时不记条件，不影响查询。
    }
  }
  export function defaultDocFilter() {
    const now = new Date();
    const pad = (value) => String(value).padStart(2, "0");
    const month = now.getFullYear() + "-" + pad(now.getMonth() + 1);
    return {
      keyword: "",
      date_from: month + "-01",
      date_to:
        now.getFullYear() + "-" + pad(now.getMonth() + 1) + "-" + pad(now.getDate()),
      status: "",
      amount_min: "",
      amount_max: "",
      amount_open: false,
    };
  }
  export function documentAmountValid(value) {
    return value === "" || /^\d+(\.\d{1,2})?$/.test(value);
  }

  // columns: [{title, cell(doc), actions?}]；返回表格 DOM 与可按 id 取行的接口。
  export function createDocumentList(options) {
    const docType = options.docType;
    const mount = options.mount;
    const columns = options.columns;
    const emptyText = options.emptyText || "没有符合条件的单据";
    const pageSize = options.pageSize || 20;
    let filter = readSavedDocFilter(docType) || defaultDocFilter();
    let rows = [];
    let totalCount = 0;
    let totalAmountCents = 0;
    let page = 1;
    let loading = false;
    let requestSeq = 0;

    mount.innerHTML = `
      <form class="filters" data-role="doc-filters">
        <label class="field"><span>关键词</span>
          <input type="text" data-field="keyword" placeholder="单号 / 往来单位 / 业务员" />
        </label>
        <label class="field"><span>开始日期</span><input type="date" data-field="date-from" /></label>
        <label class="field"><span>结束日期</span><input type="date" data-field="date-to" /></label>
        <label class="field"><span>状态</span>
          <select data-field="status">
            <option value="">全部</option>
            <option value="draft">草稿</option>
            <option value="approved">已审核</option>
            <option value="reversed">已反审核</option>
          </select>
        </label>
        <div class="form-actions">
          <button class="btn" type="button" data-role="amount-toggle">金额</button>
          <button class="btn primary" type="submit">查询</button>
          <button class="btn" type="button" data-role="filter-reset">重置</button>
        </div>
      </form>
      <form class="filters" data-role="amount-fields" hidden>
        <label class="field"><span>金额下限（元）</span>
          <input type="text" inputmode="decimal" data-field="amount-min" placeholder="0.00" />
        </label>
        <label class="field"><span>金额上限（元）</span>
          <input type="text" inputmode="decimal" data-field="amount-max" placeholder="不限" />
        </label>
      </form>
      <p class="page-sub" data-role="doc-summary"></p>
      <table class="table" data-role="documents"></table>
      <div class="form-actions">
        <button class="btn" type="button" data-role="prev">上一页</button>
        <span class="page-sub" data-role="doc-page"></span>
        <button class="btn" type="button" data-role="next">下一页</button>
      </div>`;

    const filters = mount.querySelector('[data-role="doc-filters"]');
    const amountFields = mount.querySelector('[data-role="amount-fields"]');
    const amountToggle = mount.querySelector('[data-role="amount-toggle"]');
    const keywordInput = filters.querySelector('[data-field="keyword"]');
    const dateFromInput = filters.querySelector('[data-field="date-from"]');
    const dateToInput = filters.querySelector('[data-field="date-to"]');
    const statusField = filters.querySelector('[data-field="status"]');
    const amountMinInput = amountFields.querySelector('[data-field="amount-min"]');
    const amountMaxInput = amountFields.querySelector('[data-field="amount-max"]');
    const summary = mount.querySelector('[data-role="doc-summary"]');
    const table = mount.querySelector('[data-role="documents"]');
    const pageInfo = mount.querySelector('[data-role="doc-page"]');
    const prevButton = mount.querySelector('[data-role="prev"]');
    const nextButton = mount.querySelector('[data-role="next"]');

    function syncForm() {
      keywordInput.value = filter.keyword;
      dateFromInput.value = filter.date_from;
      dateToInput.value = filter.date_to;
      statusField.value = filter.status;
      amountMinInput.value = filter.amount_min;
      amountMaxInput.value = filter.amount_max;
      amountFields.hidden = !filter.amount_open;
      amountToggle.textContent = filter.amount_open ? "收起金额" : "金额";
    }

    // 从表单收集条件；金额只在展开时生效，收起时一律按清空处理。
    function collectFilter() {
      const next = {
        keyword: keywordInput.value.trim(),
        date_from: dateFromInput.value,
        date_to: dateToInput.value,
        status: DOC_FILTER_STATUS.includes(statusField.value) ? statusField.value : "",
        amount_min: "",
        amount_max: "",
        amount_open: !amountFields.hidden,
      };
      if (next.amount_open) {
        next.amount_min = amountMinInput.value.trim();
        next.amount_max = amountMaxInput.value.trim();
        if (!documentAmountValid(next.amount_min) || !documentAmountValid(next.amount_max)) {
          showNotice("金额最多两位小数，请检查金额区间", "error");
          return null;
        }
      }
      return next;
    }

    async function fetchPage() {
      const seq = ++requestSeq;
      loading = true;
      const params = new URLSearchParams();
      params.set("doc_type", docType);
      params.set("page", String(page));
      params.set("page_size", String(pageSize));
      params.set("keyword", filter.keyword);
      params.set("date_from", filter.date_from);
      params.set("date_to", filter.date_to);
      params.set("status", filter.status);
      params.set("amount_min", filter.amount_open ? filter.amount_min : "");
      params.set("amount_max", filter.amount_open ? filter.amount_max : "");
      try {
        const result = await api("GET", "/api/documents?" + params.toString());
        if (seq !== requestSeq) return;
        rows = result && Array.isArray(result.rows) ? result.rows : [];
        totalCount = Number(result && result.total_count) || 0;
        totalAmountCents = Number(result && result.total_amount_cents) || 0;
        page = Number(result && result.page) || page;
        render();
      } catch (error) {
        if (seq === requestSeq) showNotice(error.message, "error");
      } finally {
        if (seq === requestSeq) loading = false;
      }
    }

    async function reload() {
      const next = collectFilter();
      if (!next) return;
      filter = next;
      writeSavedDocFilter(docType, filter);
      await fetchPage();
    }

    function render() {
      const pages = Math.max(1, Math.ceil(totalCount / pageSize));
      page = Math.min(Math.max(1, page), pages);
      summary.textContent =
        "共 " + totalCount + " 张，合计金额 " + money(totalAmountCents) + " 元";
      pageInfo.textContent = "第 " + page + " / " + pages + " 页";
      prevButton.disabled = page <= 1;
      nextButton.disabled = page >= pages;
      if (!rows.length) {
        table.innerHTML = `<tbody><tr><td class="empty">${escapeHtml(emptyText)}</td></tr></tbody>`;
        return;
      }
      table.innerHTML = `
        <thead><tr>${columns
          .map((column) => `<th${column.actions ? ' class="actions"' : ""}>${escapeHtml(column.title)}</th>`)
          .join("")}</tr></thead>
        <tbody>${rows
          .map(
            (doc) => `<tr data-id="${doc.id}">${columns
              .map(
                (column) =>
                  `<td${column.actions ? ' class="actions"' : ""}>${column.cell(doc)}</td>`
              )
              .join("")}</tr>`
          )
          .join("")}</tbody>`;
    }

    syncForm();

    filters.addEventListener("submit", (event) => {
      event.preventDefault();
      page = 1;
      reload();
    });
    const onFilterChange = () => {
      page = 1;
      reload();
    };
    keywordInput.addEventListener("change", onFilterChange);
    dateFromInput.addEventListener("change", onFilterChange);
    dateToInput.addEventListener("change", onFilterChange);
    statusField.addEventListener("change", onFilterChange);
    amountMinInput.addEventListener("change", () => {
      if (!amountFields.hidden) onFilterChange();
    });
    amountMaxInput.addEventListener("change", () => {
      if (!amountFields.hidden) onFilterChange();
    });
    filters.querySelector('[data-role="filter-reset"]').addEventListener("click", () => {
      filter = defaultDocFilter();
      page = 1;
      syncForm();
      writeSavedDocFilter(docType, filter);
      fetchPage();
    });
    amountToggle.addEventListener("click", () => {
      if (amountFields.hidden) {
        amountFields.hidden = false;
        amountToggle.textContent = "收起金额";
        filter = { ...filter, amount_open: true };
        writeSavedDocFilter(docType, filter);
        return;
      }
      // 收起即清空金额条件，并立即按新条件回到第一页。
      amountFields.hidden = true;
      amountToggle.textContent = "金额";
      amountMinInput.value = "";
      amountMaxInput.value = "";
      page = 1;
      reload();
    });
    prevButton.addEventListener("click", () => {
      if (loading || page <= 1) return;
      page -= 1;
      fetchPage();
    });
    nextButton.addEventListener("click", () => {
      if (loading) return;
      page += 1;
      fetchPage();
    });

    return {
      table,
      reload,
      rowById(id) {
        return rows.find((doc) => doc.id === id);
      },
      get rows() {
        return rows;
      },
    };
  }

  // ---------- 单据页面（销售出库 / 采购入库共用） ----------
