/* app/shell.js：页面导航、路由与页面挂载。
 * 由原 web/app.js 按职责拆出，供本模块与入口共同加载。
 */
import { auditPage, settingPage, userPage } from "./admin.js";
import { api, can, confirmLeavePage, escapeHtml, main, money, nav, notice, setUnsavedGuard, showNotice, state } from "./core.js";
import { documentPage } from "./document.js";
import { financePage } from "./finance.js";
import { crudPage, itemCategoryPage, itemPage, openingPage } from "./masterdata.js";
import { orderPage } from "./order.js";
import { printTemplatePage } from "./print_template.js";
import { reportPage } from "./report.js";
import { dashboardPage, salesReportPage, statementPage } from "./salesreport.js";
import { stockPage } from "./stock.js";
import { NAV_GROUPS, NAV_LAYOUT, aboutPage, restorePage } from "./system.js";

  export function buildNav(all) {
    const groups = new Map(
      NAV_GROUPS.map((group) => [group.key, { ...group, pages: [] }])
    );
    const direct = new Map();
    for (const page of all) {
      const group = page.group ? groups.get(page.group) : null;
      if (group) {
        group.pages.push(page);
      } else {
        direct.set(page.key, page);
      }
    }

    const entries = [];
    const usedGroups = new Set();
    const usedPages = new Set();
    for (const item of NAV_LAYOUT) {
      if (item.type === "group") {
        const group = groups.get(item.key);
        if (group && group.pages.length) {
          entries.push({ type: "group", group });
          usedGroups.add(group.key);
        }
      } else if (direct.has(item.key)) {
        entries.push({ type: "page", page: direct.get(item.key) });
        usedPages.add(item.key);
      }
    }
    for (const group of groups.values()) {
      if (group.pages.length && !usedGroups.has(group.key)) {
        entries.push({ type: "group", group });
      }
    }
    for (const [key, page] of direct) {
      if (!usedPages.has(key)) entries.push({ type: "page", page });
    }
    return entries;
  }
  export function navHtml(entries) {
    return entries
      .map((entry) => {
        if (entry.type === "page") {
          return `<button type="button" class="nav-top" data-key="${entry.page.key}">${escapeHtml(
            entry.page.title
          )}</button>`;
        }
        const items = entry.group.pages
          .map(
            (page) =>
              `<button type="button" data-key="${page.key}" data-group="${entry.group.key}">${escapeHtml(
                page.title
              )}</button>`
          )
          .join("");
        return `<div class="nav-group" data-group="${entry.group.key}">
            <button type="button" class="nav-group-title" data-group-toggle="${entry.group.key}" aria-expanded="false">
              <span>${escapeHtml(entry.group.title)}</span><span class="nav-chevron">›</span>
            </button>
            <div class="nav-group-items">${items}</div>
          </div>`;
      })
      .join("");
  }
  export const FIXED_PAGES = [
    {
      key: "purchase",
      title: "采购入库",
      group: "purchase",
      render: (mount) =>
        documentPage(mount, {
          docType: "PUR_IN",
          title: "采购入库",
          subtitle: "保存即审核：同一事务内增加库存并写入库存流水",
          partnerLabel: "供应商",
          partnerPlaceholder: "可选",
          partnerApi: "/api/suppliers",
          docLabel: "入库",
        }),
    },
    {
      key: "purchase-return",
      title: "采购退货",
      group: "purchase",
      render: (mount) =>
        documentPage(mount, {
          docType: "PUR_RET",
          title: "采购退货",
          subtitle: "保存即审核：按退货单价冲减库存并写入库存流水",
          partnerLabel: "供应商",
          partnerPlaceholder: "可选",
          partnerApi: "/api/suppliers",
          docLabel: "采购退货",
        }),
    },
    {
      key: "purchase-orders",
      title: "采购订单",
      group: "purchase",
      render: (mount) =>
        orderPage(mount, {
          docType: "PUR_ORDER",
          title: "采购订单",
          subtitle: "只做需求确认与入库来源，不占库存、不写流水、不产生往来",
          partnerLabel: "供应商",
          partnerApi: "/api/suppliers",
          warehouseLabel: "预计入库仓",
          convertTitle: "入库",
          convertWarehouseLabel: "入库仓库",
          convertDateLabel: "入库日期",
          convertAction: "入库",
          convertSubmit: "确认入库",
          convertNotice: "已生成采购入库单：",
          convertedLabel: "已全部入库",
          partialLabel: "部分入库",
          emptyLabel: "还没有采购订单",
        }),
    },
    {
      key: "sales",
      title: "销售出库",
      group: "sales",
      render: (mount) =>
        documentPage(mount, {
          docType: "SAL_OUT",
          title: "销售出库",
          subtitle: "保存即审核：同一事务内扣减库存并写入库存流水",
          partnerLabel: "客户",
          partnerPlaceholder: "可选",
          partnerApi: "/api/customers",
          docLabel: "出库",
        }),
    },
    {
      key: "sales-return",
      title: "销售退货",
      group: "sales",
      render: (mount) =>
        documentPage(mount, {
          docType: "SAL_RET",
          title: "销售退货",
          subtitle: "保存即审核：按成本单价增加库存并写入库存流水",
          partnerLabel: "客户",
          partnerPlaceholder: "可选",
          partnerApi: "/api/customers",
          docLabel: "销售退货",
        }),
    },
    {
      key: "other-in",
      title: "其他入库",
      group: "stock",
      render: (mount) =>
        documentPage(mount, {
          docType: "OTH_IN",
          title: "其他入库",
          subtitle: "保存即审核：按录入单价增加库存，不产生应收应付",
          partnerLabel: "备注",
          partnerPlaceholder: "可选",
          docLabel: "入库",
        }),
    },
    {
      key: "other-out",
      title: "其他出库",
      group: "stock",
      render: (mount) =>
        documentPage(mount, {
          docType: "OTH_OUT",
          title: "其他出库",
          subtitle: "保存即审核：按当前移动加权平均结转成本，不产生应收应付",
          partnerLabel: "备注",
          partnerPlaceholder: "可选",
          docLabel: "出库",
        }),
    },
    {
      key: "sales-orders",
      title: "销售订单",
      group: "sales",
      render: (mount) => orderPage(mount),
    },
    {
      key: "transfer",
      title: "调拨",
      group: "stock",
      render: (mount) =>
        documentPage(mount, {
          docType: "STK_TRF",
          title: "库存调拨",
          subtitle: "保存即审核：源仓出库与目标仓入库在同一事务内完成",
          partnerLabel: "备注",
          partnerPlaceholder: "可选",
          docLabel: "调拨",
          twoWarehouse: true,
        }),
    },
    {
      key: "stocktake",
      title: "盘点",
      group: "stock",
      render: (mount) =>
        documentPage(mount, {
          docType: "STK_CNT",
          title: "库存盘点",
          subtitle: "保存即审核：明细数量填实盘数，按与账面的差额生成调整流水",
          partnerLabel: "备注",
          partnerPlaceholder: "可选",
          docLabel: "盘点",
        }),
    },
    {
      key: "cost-adjust",
      title: "成本调整",
      group: "finance",
      render: (mount) =>
        documentPage(mount, {
          docType: "CST_ADJ",
          title: "成本调整单",
          subtitle: "只调整结存金额、不改数量；仓库与原因逐行填写，审核权限仅管理员",
          partnerLabel: "备注",
          partnerPlaceholder: "（不适用）",
          docLabel: "成本调整",
          costAdjust: true,
        }),
    },
    {
      key: "ar-ap",
      title: "收付款",
      group: "finance",
      render: financePage,
    },
    { key: "statement", title: "对账单", group: "finance", render: statementPage },
    {
      key: "item-categories",
      title: "商品分类",
      group: "master",
      permission: "item.read",
      render: itemCategoryPage,
    },
    {
      key: "openings",
      title: "期初导入",
      group: "master",
      permission: "opening.write",
      render: openingPage,
    },
    { key: "stock", title: "库存", group: "stock", render: stockPage },
    { key: "print-templates", title: "打印模板", group: "system", render: printTemplatePage },
    { key: "reports", title: "报表", group: "", render: reportPage },
    {
      key: "dashboard",
      title: "经营看板",
      group: "",
      permission: "finance.read",
      capability: "report.dashboard",
      render: dashboardPage,
    },
    {
      key: "sales-report",
      title: "销售报表",
      group: "",
      permission: "finance.read",
      render: salesReportPage,
    },
    { key: "users", title: "用户", group: "system", permission: "user.read", render: userPage },
    {
      key: "audit",
      title: "审计日志",
      group: "system",
      permission: "audit.read",
      render: auditPage,
    },
    {
      key: "settings",
      title: "系统参数",
      group: "system",
      permission: "setting.read",
      render: settingPage,
    },
    {
      key: "restore",
      title: "备份与恢复",
      group: "system",
      permission: "setting.read",
      render: restorePage,
    },
    { key: "about", title: "关于与诊断", group: "system", render: aboutPage },
  ];

  // 导航状态：手风琴只记住一个展开组；页面 key 供下次打开回到原页。
  export function readNavState(key) {
    try {
      return localStorage.getItem(key) || "";
    } catch (error) {
      return "";
    }
  }
  export function writeNavState(key, value) {
    try {
      if (value) localStorage.setItem(key, value);
      else localStorage.removeItem(key);
    } catch (error) {
      // 隐私模式等禁用存储时静默降级，导航仍然可用。
    }
  }
  export function expandGroup(key) {
    for (const group of nav.querySelectorAll(".nav-group")) {
      const open = key !== "" && group.dataset.group === key;
      group.classList.toggle("open", open);
      const toggle = group.querySelector("[data-group-toggle]");
      if (toggle) toggle.setAttribute("aria-expanded", open ? "true" : "false");
    }
  }

  // 页面路由：#/<key>。key 与 nav 的 data-key 一致；只排除空白与斜杠这类
  // 结构性非法字符，点号、中文等交给 encodeURIComponent/decodeURIComponent。
  export function hashPageKey() {
    const hash = location.hash || "";
    if (!hash.startsWith("#/")) return "";
    const raw = hash.slice(2);
    if (!raw) return "";
    let key = raw;
    try {
      key = decodeURIComponent(raw);
    } catch (error) {
      return "";
    }
    if (/[\s/]/.test(key)) return "";
    return key;
  }
  export function replaceHash(key) {
    const target = key
      ? "#/" + encodeURIComponent(key)
      : location.pathname + location.search;
    history.replaceState(null, "", target);
  }
  export let routeToken = 0;
  // 页面挂载序号：切页只做一次原子替换，旧页在新页首屏就绪前保持不动。
  export let mountSeq = 0;
  export async function handleHashChange() {
    const pages = state.allPages;
    if (!pages.length) return;
    const token = ++routeToken;
    const page = pages.find((item) => item.key === hashPageKey());
    if (page && page.key === state.current) return;
    const leave = await confirmLeavePage();
    // 等待确认期间又有新的跳转，交给最新一次处理。
    if (token !== routeToken) return;
    if (!leave) {
      // 返回/前进已经改了 hash，取消时把地址还原到当前页。
      replaceHash(state.current);
      return;
    }
    if (!page) {
      const fallback = pages[0];
      replaceHash(fallback.key);
      if (fallback.key !== state.current) mountPage(fallback);
      return;
    }
    mountPage(page);
  }
  export async function goToPage(page) {
    if (page.key === state.current) return;
    if (!(await confirmLeavePage())) return;
    const target = "#/" + encodeURIComponent(page.key);
    if (location.hash === target) return;
    // 用 pushState 改地址再手动路由：直接赋值 location.hash 在 Safari 里会被当成
    // 一次文档导航，整页重新加载（服务端表现为 GET / + 全套静态资源），侧栏跟着
    // 一起重建，看起来就是「点菜单整页抖一下」。pushState 不触发任何浏览器导航。
    history.pushState(null, "", target);
    handleHashChange();
  }
  export function mountPage(page, options) {
    // 离开旧页后重新注册：列表页、报表页不设防，只有录入页会自己再注册。
    setUnsavedGuard(null);
    const persist = !options || options.persist !== false;
    state.current = page.key;
    if (page.group) expandGroup(page.group);
    for (const link of nav.querySelectorAll("button[data-key]")) {
      link.classList.toggle("active", link.dataset.key === page.key);
    }
    for (const group of nav.querySelectorAll(".nav-group")) {
      group.classList.toggle("active", group.dataset.group === page.group);
    }
    if (persist) {
      writeNavState("xiaotu.nav.page", page.key);
      writeNavState("xiaotu.nav.group", page.group || "");
    }
    // 切页过程中不动已经显示的内容：新页先渲染到脱离文档的容器里，等它的
    // render（各页都会 await 首次数据）落地后，再一次性换掉旧页。这样
    // main 的 scrollHeight 与滚动条不会经历「清空 → 空白 → 填回」的中间态，
    // main 也不会在切页中途被清空一次。期间旧页保持可交互，用户再点别的菜单
    // 会叠加新的序号，旧的那次替换自动作废。
    const seq = ++mountSeq;
    const container = document.createElement("section");
    container.className = "page";
    let swapped = false;
    let fallbackTimer = 0;
    const swap = () => {
      if (swapped) return;
      swapped = true;
      if (fallbackTimer) clearTimeout(fallbackTimer);
      // 期间已经切到别的页，这次结果直接丢弃。
      if (seq !== mountSeq) return;
      main.querySelectorAll(":scope > *").forEach((node) => {
        if (node !== notice && node.id !== "data-warning" && node.id !== "network-warning") {
          node.remove();
        }
      });
      showNotice("");
      main.appendChild(container);
      main.scrollTop = 0;
    };
    // 兜底：某页渲染卡住时不能让旧页一直留在屏幕上。
    fallbackTimer = setTimeout(swap, 1500);
    Promise.resolve(page.render(container, { crudPage, itemPage, money, escapeHtml, can }))
      .then(swap)
      .catch((error) => {
        showNotice(error.message, "error");
        swap();
      });
  }

  // 门禁页出现后，启动链接里的 #t= 一次性 token 已不再使用，清掉避免刷新时误判。
