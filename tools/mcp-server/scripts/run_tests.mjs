// MCP サーバーのテスト実行器（W05-31）。
//   node scripts/run_tests.mjs            全テストを順に実行（最初の失敗で止まる）
//   node scripts/run_tests.mjs --offline  tests.offline-exclude.txt のテストを除く
//   node scripts/run_tests.mjs --list     実行せず、順番どおりの一覧を出す（--offline と併用可）
// 一覧は tests.base.txt → tests.lane-a/b/c/d.txt の順に連結する。
import { readFileSync, existsSync } from "node:fs";
import { spawnSync } from "node:child_process";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const root = dirname(here);

function readList(name, required) {
  const p = join(here, name);
  if (!existsSync(p)) {
    if (required) throw new Error(`テスト一覧がありません: scripts/${name}`);
    return [];
  }
  return readFileSync(p, "utf8")
    .split(/\r?\n/)
    .map((l) => l.replace(/#.*$/, "").trim())
    .filter((l) => l.length > 0);
}

export function collectTests({ offline = false } = {}) {
  const files = ["tests.base.txt", "tests.lane-a.txt", "tests.lane-b.txt", "tests.lane-c.txt", "tests.lane-d.txt"];
  const all = [];
  for (const f of files) all.push(...readList(f, true));
  const seen = new Set();
  for (const t of all) {
    if (seen.has(t)) throw new Error(`テストが重複しています: ${t}`);
    seen.add(t);
    if (!existsSync(join(root, t))) throw new Error(`テストファイルが存在しません: ${t}`);
  }
  if (!offline) return all;
  const excl = new Set(readList("tests.offline-exclude.txt", true));
  return all.filter((t) => !excl.has(t));
}

function main() {
  const args = process.argv.slice(2);
  const offline = args.includes("--offline");
  const list = args.includes("--list");
  const tests = collectTests({ offline });
  if (list) {
    for (const t of tests) console.log(t);
    return 0;
  }
  for (const t of tests) {
    console.log(`\n=== node ${t} ===`);
    const r = spawnSync(process.execPath, [t], { cwd: root, stdio: "inherit" });
    if (r.status !== 0) {
      console.error(`\nFAILED: ${t} (exit ${r.status ?? r.signal})`);
      return r.status || 1;
    }
  }
  console.log(`\nOK: ${tests.length} test files`);
  return 0;
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) {
  try {
    process.exitCode = main();
  } catch (e) {
    console.error(String(e?.message ?? e));
    process.exitCode = 2;
  }
}
