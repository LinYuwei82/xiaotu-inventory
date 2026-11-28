// 前端脚本语法检查：web/app.js 与 web/app/*.js 均为 ES module。
// jsc 的 new Function 不认识 import/export，这里先剥掉模块语法再解析；
// 只解析不执行，依赖能否解析、运行时行为由浏览器与实机验收覆盖。
// 文件列表由 check.sh 注入为 FILES；失败必须 throw（jsc 的 quit(0) 不透出退出码）。
const IMPORT = /^[ \t]*import[ \t][\s\S]*?;[ \t]*$/gm;
const EXPORT_DECL = /^[ \t]*export[ \t]+(?=(?:async[ \t]+)?(?:function|const|let|var|class)\b)/gm;
const EXPORT_LIST = /^[ \t]*export[ \t]*\{[\s\S]*?\}[ \t]*;[ \t]*$/gm;

let failed = 0;
for (const file of FILES) {
  const source = readFile(file);
  const script = source.replace(IMPORT, "").replace(EXPORT_DECL, "").replace(EXPORT_LIST, "");
  // 剩余的 import/export 只可能是模块语法（对象字面量的 "export:" 后面是冒号，不算）。
  const leftover = /^[ \t]*(?:import|export)(?=[ \t{*"'])/m.exec(script);
  if (leftover) {
    print("    " + file + " 存在未识别的模块语法：" + leftover[0].trim());
    failed++;
    continue;
  }
  try {
    new Function(script);
  } catch (error) {
    print("    " + file + " 语法错误: " + error);
    failed++;
  }
}
if (failed) throw new Error(failed + " 个前端脚本未通过语法检查");
print("    前端脚本语法通过（" + FILES.length + " 个文件）");
