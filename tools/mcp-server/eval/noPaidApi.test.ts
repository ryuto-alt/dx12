// W05-36: 有料の Anthropic API を使わない(EXEC_W05_W1 決定 3)の npm 側の検査。ctest の NoPaidApi
// (cmake/lint/NoPaidApi.cmake)と同じ規則を同じ許可リスト(cmake/lint/NoPaidApi.allow.txt)で確かめる。
//   - リポジトリ全体(追跡ファイルのコード・スクリプト・設定)と全 package.json / package-lock.json に
//     @anthropic-ai の依存・import が無い
//   - 鍵の環境変数名の参照は、許可リストの guard: に載るファイルの「拒むガード」の行だけ
// この検査の定義ファイル(本ファイル)は許可リストの全面除外に載っている。
// 実行: node eval/noPaidApi.test.ts
import fs from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(here, "..", "..", "..");

const KEY = ["ANTHROPIC", "API", "KEY"].join("_");
const SDK = ["@anthropic", "-ai"].join("");
const CODE_EXT = /\.(ts|tsx|js|mjs|cjs|json|ps1|psm1|py|cmake|txt|cpp|h|hpp|inc|yml|yaml|bat|cmd|iss)$/;
const GUARD_LINE = /(^|[^A-Za-z])(if|throw|exit|process[.]exit|Remove-Item|delete)([^A-Za-z]|$)|拒|refuse|reject/;

export type Allow = { full: string[]; guard: string[] };

export function parseAllow(text: string): Allow {
  const a: Allow = { full: [], guard: [] };
  for (const raw of text.split(/\r?\n/)) {
    const l = raw.trim();
    if (!l || l.startsWith("#")) continue;
    if (l.startsWith("guard:")) a.guard.push(l.slice(6)); else a.full.push(l);
  }
  return a;
}

/** 1 ファイルを検査して違反の説明(無ければ null)を返す。 */
export function checkFile(rel: string, text: string, allow: Allow): string | null {
  if (!CODE_EXT.test(rel) || /(^|\/)(node_modules|third_party)\//.test(rel)) return null;
  if (allow.full.some((p) => rel.startsWith(p))) return null;
  if (text.includes(SDK)) return `${rel}（SDK の参照）`;
  if (!text.includes(KEY)) return null;
  if (!allow.guard.some((p) => rel.startsWith(p))) return `${rel}（鍵の環境変数の参照）`;
  for (const ln of text.split(/\r?\n/)) if (ln.includes(KEY) && !GUARD_LINE.test(ln)) return `${rel}（ガードの形ではない行: ${ln.trim().slice(0, 120)}）`;
  return null;
}

function listFiles(): string[] {
  const r = spawnSync("git", ["-C", repoRoot, "ls-files"], { encoding: "utf8", maxBuffer: 64 * 1024 * 1024 });
  if (r.status === 0 && r.stdout) return r.stdout.split(/\r?\n/).filter(Boolean);
  const out: string[] = [];
  const walk = (d: string) => {
    for (const e of fs.readdirSync(path.join(repoRoot, d), { withFileTypes: true })) {
      const rel = d ? `${d}/${e.name}` : e.name;
      if (e.isDirectory()) { if (!/^(node_modules|build|third_party|\.git)$/.test(e.name)) walk(rel); } else out.push(rel);
    }
  };
  walk("");
  return out;
}

let failed = 0;
function check(label: string, cond: boolean, detail?: unknown) {
  if (cond) console.log(`  OK  ${label}`);
  else { failed++; console.log(`  NG  ${label}${detail !== undefined ? `\n      ${JSON.stringify(detail).slice(0, 800)}` : ""}`); }
}

if (process.argv[1] && path.resolve(process.argv[1]) === path.resolve(fileURLToPath(import.meta.url))) {
  console.log("[1] 検査の自己テスト");
  const allow = parseAllow("# c\ncmake/lint/\nguard:tools/g.ts\n");
  check("SDK の参照は検出", checkFile("tools/a.ts", `import x from "${SDK}/sdk"`, allow) !== null);
  check("鍵の参照は検出(許可外)", checkFile("tools/a.ts", `const k = process.env.${KEY};`, allow) !== null);
  check("ガードの行だけなら許可(guard:)", checkFile("tools/g.ts", `if (process.env.${KEY}) throw new Error("x");`, allow) === null);
  check("ガードの形ではない行は検出", checkFile("tools/g.ts", `const k = process.env.${KEY};`, allow) !== null);
  check("ガードのファイルでも SDK は不可", checkFile("tools/g.ts", `require("${SDK}/sdk")`, allow) !== null);
  check("全面除外は見ない", checkFile("cmake/lint/x.cmake", `${KEY} ${SDK}`, allow) === null);
  check("node_modules は見ない", checkFile("tools/mcp-server/node_modules/x/a.js", SDK, allow) === null);

  console.log("[2] リポジトリ全体");
  const allowFile = path.join(repoRoot, "cmake", "lint", "NoPaidApi.allow.txt");
  check("許可リストがある", fs.existsSync(allowFile));
  const real = parseAllow(fs.existsSync(allowFile) ? fs.readFileSync(allowFile, "utf8") : "");
  check("許可リストは検査の定義と守りだけ(全面除外は cmake/lint/ と noPaidApi.test.ts)", real.full.every((p) => p.startsWith("cmake/lint/") || p.endsWith("noPaidApi.test.ts")), real.full);
  const files = listFiles();
  const violations: string[] = [];
  for (const rel of files) {
    const abs = path.join(repoRoot, rel);
    let st: fs.Stats;
    try { st = fs.statSync(abs); } catch { continue; }
    if (!st.isFile() || st.size > 4_000_000 || !CODE_EXT.test(rel)) continue;
    const v = checkFile(rel, fs.readFileSync(abs, "utf8"), real);
    if (v) violations.push(v);
  }
  check(`有料 API の参照が無い(${files.length} ファイルを走査)`, violations.length === 0, violations);

  console.log("[3] 全 package.json");
  const pkgs = files.filter((f) => /(^|\/)package(-lock)?\.json$/.test(f) && !/node_modules\//.test(f));
  check("package.json が見つかる", pkgs.some((p) => p.endsWith("tools/mcp-server/package.json")), pkgs);
  const bad: string[] = [];
  for (const rel of pkgs) {
    const txt = fs.readFileSync(path.join(repoRoot, rel), "utf8");
    if (txt.includes(SDK)) bad.push(`${rel}（SDK の依存）`);
    if (txt.includes(KEY)) bad.push(`${rel}（鍵の参照）`);
  }
  check(`全 package.json / package-lock.json(${pkgs.length} 件)に SDK の依存も鍵の参照も無い`, bad.length === 0, bad);

  console.log(failed === 0 ? "\n全て OK" : `\n失敗 ${failed} 件`);
  process.exitCode = failed === 0 ? 0 : 1;
}
