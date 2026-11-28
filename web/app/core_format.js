/* app/core_format.js：金额、数量（定点整数）与枚举文案等纯格式化函数。
 * 由 core.js 拆出；全部是无副作用的纯函数，不依赖其他 app 模块。
 */

  export function escapeHtml(value) {
    return String(value === null || value === undefined ? "" : value).replace(
      /[&<>"']/g,
      (ch) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[ch]
    );
  }

  // 各报表页共用的空行片段。放在模块作用域：经营看板、销售报表、
  // 资金/库存报表都引用它，留在单个页面函数里会导致其余页面 ReferenceError。
  export function emptyRow(text, colspan) {
    return `<tbody><tr><td class="empty" colspan="${colspan}">${text}</td></tr></tbody>`;
  }

  // 商品下拉的选项文案，全站唯一口径：编码 + 空格 + 名称 + 空格 + 规格。
  // 规格为空时自动省略，不留多余空格；同编码不同规格的商品据此区分。
  // 单据明细行、报表筛选、销售报表维度下拉都引用这里，避免各页面自行拼接。
  export function itemOptionText(item) {
    if (!item) return "";
    return [item.code, item.name, item.spec]
      .map((part) => (part === null || part === undefined ? "" : String(part).trim()))
      .filter(Boolean)
      .join(" ");
  }
  export function money(cents) {
    const value = Number(cents || 0);
    return (value / 100).toFixed(2);
  }
  export const QTY_SCALE = 10000n;

  // 定点整数（×10000）→ 十进制文本，去掉末尾 0；全程 BigInt，不做浮点换算。
  // 服务端返回的 qty 已是十进制字符串（如 "20"、"1.25"）；本地计算出的
  // 定点整数（×10000）才走下面的换算。两种来源不能混着除。
  export function qtyText(value) {
    if (typeof value === "string") return value;
    if (value === null || value === undefined || value === "") return "";
    let scaled;
    try {
      scaled = BigInt(value);
    } catch (error) {
      return String(value);
    }
    const negative = scaled < 0n;
    if (negative) scaled = -scaled;
    const whole = scaled / QTY_SCALE;
    const fraction = (scaled % QTY_SCALE)
      .toString()
      .padStart(4, "0")
      .replace(/0+$/, "");
    return (negative ? "-" : "") + whole.toString() + (fraction ? "." + fraction : "");
  }

  // 十进制文本 → 定点整数字符串；最多 4 位小数、不允许负数，非法返回 null。
  // allowZero 为 false 时 0 也视为非法（单据数量必须大于 0，盘点实盘可以为 0）。
  export function parseQty(text, allowZero) {
    const value = String(text === null || text === undefined ? "" : text).trim();
    const match = /^(-?)(\d+)(?:\.(\d+))?$/.exec(value);
    if (!match) return null;
    const fraction = match[3] || "";
    if (fraction.length > 4) return null;
    const whole = match[2].replace(/^0+(?=\d)/, "");
    if (whole.length > 9) return null;
    let scaled = BigInt(whole) * QTY_SCALE + BigInt((fraction + "0000").slice(0, 4));
    if (match[1]) scaled = -scaled;
    if (scaled < 0n) return null;
    if (!allowZero && scaled === 0n) return null;
    return scaled.toString();
  }

  // 金额预览用 BigInt 计算，避免数量 × 单价超过 2^53。
  export function qtyAmountText(qtyScaled, priceCents) {
    if (qtyScaled === null) return "—";
    const numerator = BigInt(qtyScaled) * BigInt(priceCents || 0);
    const scaled =
      numerator >= 0n ? (numerator + QTY_SCALE / 2n) / QTY_SCALE
                      : -((-numerator + QTY_SCALE / 2n) / QTY_SCALE);
    const negative = scaled < 0n;
    const absolute = negative ? -scaled : scaled;
    const fraction = (absolute % 100n).toString().padStart(2, "0");
    return (negative ? "-" : "") + (absolute / 100n).toString() + "." + fraction;
  }

  // 数量（定点整数）× 单价（分）→ 金额（分），与后端 mul_qty_price 同一舍入口径。
  export function qtyAmountCents(qtyScaled, priceCents) {
    if (qtyScaled === null) return 0;
    const numerator = BigInt(qtyScaled) * BigInt(priceCents || 0);
    const scaled =
      numerator >= 0n ? (numerator + QTY_SCALE / 2n) / QTY_SCALE
                      : -((-numerator + QTY_SCALE / 2n) / QTY_SCALE);
    return Number(scaled);
  }

  // 行税额：整数分四舍五入，逐行计算，保证「净额 + 税额 = 价税合计」不差一分。
  export function taxForAmount(amountCents, rateBp, includesTax) {
    if (!rateBp) return { net: amountCents, tax: 0 };
    const divRound = (numerator, denominator) =>
      numerator >= 0
        ? Math.floor((numerator + Math.floor(denominator / 2)) / denominator)
        : -Math.floor((-numerator + Math.floor(denominator / 2)) / denominator);
    if (includesTax) {
      const tax = divRound(amountCents * rateBp, 10000 + rateBp);
      return { net: amountCents - tax, tax };
    }
    return { net: amountCents, tax: divRound(amountCents * rateBp, 10000) };
  }
  export function qtyIsNegative(value) {
    return String(value === null || value === undefined ? "" : value).startsWith("-");
  }
  export function bizCategoryText(value) {
    return (
      {
        GIFT: "赠品",
        USAGE: "领用",
        LOSS: "报损",
        OPENING: "期初",
        OTHER: "其他",
      }[value] || ""
    );
  }
  export function bizTypeText(value) {
    const text = String(value);
    if (text === "REVERSE") return "反审核回冲";
    if (text.endsWith("_REVERSE")) {
      return bizTypeText(text.slice(0, -"_REVERSE".length)) + "回冲";
    }
    return (
      {
        PUR_IN: "采购入库",
        PUR_RET: "采购退货",
        SAL_OUT: "销售出库",
        SAL_RET: "销售退货",
        OTH_IN: "其他入库",
        OTH_OUT: "其他出库",
        STK_TRF: "调拨出库",
        STK_TRF_IN: "调拨入库",
        STK_CNT_IN: "盘盈",
        STK_CNT_OUT: "盘亏",
        STK_CNT: "盘点调整",
        CST_ADJ: "成本调整",
      }[text] || text
    );
  }
