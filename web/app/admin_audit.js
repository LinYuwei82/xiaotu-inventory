/* app/admin_audit.js：审计日志页与审计摘要的展示层翻译。
 * 由 admin.js 拆出，供 pages.js 与入口共同加载。
 */
import { showNotice } from "./core_state.js";
import { bizTypeText, escapeHtml } from "./core_format.js";
import { api } from "./core_api.js";
import { downloadCsv, downloadXlsx } from "./core_export.js";

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
