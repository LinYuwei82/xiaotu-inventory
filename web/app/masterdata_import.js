/* app/masterdata_import.js：通用 CRUD 的模板下载、Excel/CSV 导出与 Excel 导入。
 * 由 masterdata.js 拆出；页面状态通过 ctx 传入。
 */
import { API_BASE, can, showNotice, state } from "./core_state.js";
import { askConfirm } from "./core_dialog.js";
import { csvField, downloadCsv, downloadXlsx, isWinWebview, saveText } from "./core_export.js";

  export function importExportButtons(def) {
    const io = def.importable ? def.permissions || {} : null;
    if (!io) return "";
    return `${can(io.read) ? `<button class="btn" data-role="template">下载模板</button>
          <button class="btn" data-role="export-xlsx">导出 Excel</button>
          <button class="btn" data-role="export-csv">导出 CSV</button>` : ""}
        ${can(io.write) ? `<button class="btn" data-role="import">导入</button>` : ""}`;
  }

  export function bindImportExport(ctx) {
    const def = ctx.def;
    const mount = ctx.mount;

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
            await ctx.reload();
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
