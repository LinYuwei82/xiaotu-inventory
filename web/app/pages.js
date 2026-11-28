/* app/pages.js：固定业务页配置表（菜单 key、标题、权限与页面渲染函数）。
 * 导航分组与排版仍在 system.js / shell.js。
 */
import { userPage } from "./admin.js";
import { auditPage } from "./admin_audit.js";
import { settingPage } from "./admin_settings.js";
import { documentPage } from "./document.js";
import { financePage } from "./finance.js";
import { itemCategoryPage } from "./masterdata_category.js";
import { openingPage } from "./masterdata_opening.js";
import { orderPage } from "./order.js";
import { printTemplatePage } from "./print_template.js";
import { reportPage } from "./report.js";
import { dashboardPage, salesReportPage } from "./salesreport.js";
import { statementPage } from "./salesreport_statement.js";
import { stockPage } from "./stock.js";
import { aboutPage, restorePage } from "./system.js";

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
