/* app/masterdata.js：通用 CRUD 列表/表单的装配与加载。
 * 筛选、表单、保存、导入导出与档案合并分别见 masterdata_filters / masterdata_form /
 * masterdata_actions / masterdata_import / masterdata_merge；商品页见 masterdata_item.js。
 */
import { can } from "./core_state.js";
import { api } from "./core_api.js";
import { crudPageHtml, crudTableHtml } from "./masterdata_view.js";
import { bindFilters, filterQuery } from "./masterdata_filters.js";
import { bindFormActions, bindTableActions } from "./masterdata_actions.js";
import { bindImportExport, importExportButtons } from "./masterdata_import.js";
import { bindMergePanel } from "./masterdata_merge.js";

  export async function crudPage(mount, def) {
    const io = def.importable ? def.permissions || {} : null;
    const isTrader = def.key === "customer" || def.key === "supplier";

    mount.innerHTML = crudPageHtml(def, {
      ioButtons: importExportButtons(def),
      isTrader,
      mergeButton: can("customer.write"),
    });

    // ctx 汇集页面可变状态与 DOM 引用，供 masterdata_* 各模块共用。
    const ctx = {
      def,
      mount,
      formCard: mount.querySelector('[data-role="form-card"]'),
      form: mount.querySelector('[data-role="form"]'),
      table: mount.querySelector('[data-role="table"]'),
      filtersBar: mount.querySelector('[data-role="filters"]'),
      rows: [],
      editingId: 0,
      optionCache: {},
    };

    function renderTable() {
      ctx.table.innerHTML = crudTableHtml(ctx.rows, def.columns);
    }

    async function reload() {
      ctx.rows = await api("GET", def.api + filterQuery(ctx));
      renderTable();
    }
    ctx.reload = () => reload();

    await bindFilters(ctx);
    bindMergePanel(ctx);
    if (io) bindImportExport(ctx);
    bindFormActions(ctx);
    bindTableActions(ctx);

    await reload();
  }
