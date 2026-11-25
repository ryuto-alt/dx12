// MCP サーバーのテスト実行器（W05-31）。
//   node scripts/run_tests.mjs            全テストを順に実行（最初の失敗で止まる）
//   node scripts/run_tests.mjs --offline  tests.offline-exclude.txt のテストを除く
//   node scripts/run_tests.mjs --self-test  一覧の検査（重複・欠落・連結順・offline-exclude）の自己テスト
//   node scripts/run_tests.mjs --list     実行せず、順番どおりの一覧を出す（--offline と併用可）
// 一覧は tests.base.txt → tests.lane-a/b/c/d.txt の順に連結する。
import { readFileSync, existsSync, mkdtempSync, mkdirSync, writeFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { spawnSync } from "node:child_process";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const root = dirname(here);

function readList(name, required, dir = here) {
  const p = join(dir, name);
  if (!existsSync(p)) {
    if (required) throw new Error(`テスト一覧がありません: ${name}`);
    return [];
  }
  return readFileSync(p, "utf8")
    .split(/\r?\n/)
    .map((l) => l.replace(/#.*$/, "").trim())
    .filter((l) => l.length > 0);
}

export function collectTests({ offline = false, listDir = here, rootDir = root } = {}) {
  const files = ["tests.base.txt", "tests.lane-a.txt", "tests.lane-b.txt", "tests.lane-c.txt", "tests.lane-d.txt"];
  const all = [];
  for (const f of files) all.push(...readList(f, true, listDir));
  const seen = new Set();
  for (const t of all) {
    if (seen.has(t)) throw new Error(`テストが重複しています: ${t}`);
    seen.add(t);
    if (!existsSync(join(rootDir, t))) throw new Error(`テストファイルが存在しません: ${t}`);
  }
  if (!offline) return all;
  const excl = new Set(readList("tests.offline-exclude.txt", true, listDir));
  for (const x of excl) if (!seen.has(x)) throw new Error(`tests.offline-exclude.txt に未知のテストがあります: ${x}`);
  return all.filter((t) => !excl.has(t));
}

function selfTest() {
  const t = (c, m) => {
    if (!c) throw new Error("self-test 失敗: " + m);
  };
  const throws = (f) => {
    try {
      f();
    } catch {
      return true;
    }
    return false;
  };
  const d = mkdtempSync(join(tmpdir(), "run_tests_"));
  try {
    for (const n of ["a.test.ts", "b.test.ts", "c.test.ts", "d.test.ts"]) writeFileSync(join(d, n), "");
    const lists = (o = {}) => {
      const v = { "tests.base.txt": "a.test.ts\n# c\nb.test.ts", "tests.lane-a.txt": "", "tests.lane-b.txt": "", "tests.lane-c.txt": "c.test.ts # x", "tests.lane-d.txt": "d.test.ts", "tests.offline-exclude.txt": "b.test.ts", ...o };
      for (const [n, c] of Object.entries(v)) writeFileSync(join(d, n), c);
    };
    const run = (opt = {}) => collectTests({ listDir: d, rootDir: d, ...opt });
    lists();
    t(run().join() === "a.test.ts,b.test.ts,c.test.ts,d.test.ts", "base → lane-a..d の連結順");
    t(run({ offline: true }).join() === "a.test.ts,c.test.ts,d.test.ts", "offline は除外を除く");
    lists({ "tests.lane-d.txt": "a.test.ts" });
    t(throws(() => run()), "重複は赤");
    lists({ "tests.lane-d.txt": "zzz.test.ts" });
    t(throws(() => run()), "存在しないファイルは赤");
    lists({ "tests.offline-exclude.txt": "nope.test.ts" });
    t(run().length === 4 && throws(() => run({ offline: true })), "offline-exclude の未知エントリは赤（offline のみ）");
    lists();
    rmSync(join(d, "tests.lane-c.txt"));
    t(throws(() => run()), "一覧ファイルの欠落は赤");
    mkdirSync(join(d, "x"), { recursive: true });
  } finally {
    rmSync(d, { recursive: true, force: true });
  }
  console.log("self-test OK");
  return 0;
}

function main() {
  const args = process.argv.slice(2);
  if (args.includes("--self-test")) return selfTest();
  const offline = args.includes("--offline");
  const list = args.includes("--list");
  const tests = collectTests({ offline });
  if (tests.length === 0) throw new Error("テストが 0 本です（一覧ファイルが空）");
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

// 起動方法（junction / subst / シンボリックリンク経由でも）に関わらず必ず走らせる。import 先は無い。
try {
  process.exitCode = main();
} catch (e) {
  console.error(String(e?.message ?? e));
  process.exitCode = 2;
}
