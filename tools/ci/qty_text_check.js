// 数量显示契约断言：直接抽取 web/app.js 里的 qtyText 源码执行，避免测一份副本。
// 服务端返回的 qty 已经是十进制字符串（"20"、"10.5"）；本地计算值才是 ×10000 定点整数。
const source = readFile("web/app.js");
const marker = "function qtyText(value)";
const start = source.indexOf(marker);
if (start < 0) throw new Error("web/app.js 中找不到 qtyText");

let depth = 0;
let end = -1;
for (let i = source.indexOf("{", start); i < source.length; i++) {
  const ch = source[i];
  if (ch === "{") depth++;
  else if (ch === "}") {
    depth--;
    if (depth === 0) {
      end = i;
      break;
    }
  }
}
if (end < 0) throw new Error("qtyText 函数体不完整");

const QTY_SCALE = 10000n;
const qtyText = eval("(" + source.slice(start, end + 1) + ")");

function eq(actual, expected, label) {
  if (actual !== expected) {
    throw new Error(label + "：期望 " + JSON.stringify(expected) + "，实际 " + JSON.stringify(actual));
  }
}

// 服务端已格式化的字符串必须原样返回，不能再除 10000。
eq(qtyText("20"), "20", 'qtyText("20")');
eq(qtyText("10.5"), "10.5", 'qtyText("10.5")');
eq(qtyText("0"), "0", 'qtyText("0")');
eq(qtyText("-3"), "-3", 'qtyText("-3")');
eq(qtyText(""), "", 'qtyText("")');
eq(qtyText(null), "", "qtyText(null)");
eq(qtyText(undefined), "", "qtyText(undefined)");
eq(qtyText("abc"), "abc", 'qtyText("abc")');

// 本地定点整数仍按 ×10000 换算。
eq(qtyText(200000), "20", "qtyText(200000)");
eq(qtyText(105000), "10.5", "qtyText(105000)");
eq(qtyText(-15000), "-1.5", "qtyText(-15000)");
eq(qtyText(0), "0", "qtyText(0)");

print("    qtyText 契约断言通过（12 项）");
