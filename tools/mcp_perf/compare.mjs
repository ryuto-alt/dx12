// 2 つの結果 JSON(mcp_perf.mjs --out)を method+args で突き合わせ、markdown の表にする。
//   node compare.mjs before.json after.json [label]
import fs from "node:fs";
const [a, b] = process.argv.slice(2);
const A = JSON.parse(fs.readFileSync(a, "utf8")), B = JSON.parse(fs.readFileSync(b, "utf8"));
const key = (r) => r.method + " " + r.args;
const bm = new Map(B.map((r) => [key(r), r]));
console.log("| method | args | 前 ms | 後 ms | 前 KB | 後 KB | 備考 |\n|---|---|---:|---:|---:|---:|---|");
for (const r of A) {
  const n = bm.get(key(r));
  const note = (r.ok ? "" : "ERR(引数不足等) ") + (n && !n.ok && r.ok ? "後で ERR" : "");
  console.log(`| ${r.method} | ${r.args.replace(/\|/g, "/")} | ${r.ms} | ${n ? n.ms : "-"} | ${r.kb} | ${n ? n.kb : "-"} | ${note} |`);
}
