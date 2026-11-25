// ゴールデン比較の純関数の単体テスト（GPU もエンジンも pngjs も要らない）。
//   node --test tools/bench
import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { sha256, resampleArea, compareImages, judge } from "./lib/imageDiff.mjs";
import { resolveProject, REPO_ROOT } from "./lib/engine.mjs";
import os from "node:os";
import { loadGoldenEntries, loadBaseline, writeBaselineEntries } from "./lib/goldenStore.mjs";

const solid = (w, h, r, g, b) => {
  const data = Buffer.alloc(w * h * 4);
  for (let i = 0; i < w * h; i++) { data[i * 4] = r; data[i * 4 + 1] = g; data[i * 4 + 2] = b; data[i * 4 + 3] = 255; }
  return { width: w, height: h, data };
};
const TOL = { pixelThreshold: 12, maxDiffRatio: 0.001, maxMeanAbs: 0.25 };

test("同じ画像は差ゼロで合格", () => {
  const a = solid(64, 36, 10, 20, 30);
  const c = compareImages(a, solid(64, 36, 10, 20, 30), TOL.pixelThreshold);
  assert.equal(c.diffPixels, 0);
  assert.equal(c.meanAbs, 0);
  assert.equal(judge(c, TOL).pass, true);
});

test("閾値以下の 1 LSB 揺れは違う画素に数えない", () => {
  const b = solid(64, 36, 100, 100, 100);
  for (let p = 0; p < 200; p++) b.data[p * 4] = 101;          // 一部の画素が 1 LSB だけ揺れた（ドライバ差の想定）
  const c = compareImages(solid(64, 36, 100, 100, 100), b, TOL.pixelThreshold);
  assert.equal(c.diffPixels, 0);
  assert.equal(judge(c, TOL).pass, true);
});

test("一部の画素が大きく変わると不合格になり理由が出る", () => {
  const a = solid(100, 100, 0, 0, 0), b = solid(100, 100, 0, 0, 0);
  for (let p = 0; p < 50; p++) b.data[p * 4] = 255;           // 0.5% の画素を真っ赤に
  const c = compareImages(a, b, TOL.pixelThreshold);
  assert.equal(c.diffPixels, 50);
  const j = judge(c, TOL);
  assert.equal(j.pass, false);
  assert.ok(j.reasons.some((r) => r.includes("違う画素")));
});

test("全体の明るさが少し変わるだけでも平均差で落ちる", () => {
  const c = compareImages(solid(64, 36, 100, 100, 100), solid(64, 36, 110, 110, 110), 50);
  assert.equal(c.diffPixels, 0);
  assert.equal(judge(c, TOL).pass, false);
});

test("大きさが違う比較は例外", () => {
  assert.throws(() => compareImages(solid(4, 4, 0, 0, 0), solid(5, 4, 0, 0, 0), 12), /大きさが違う/);
});

test("面積平均の縮小: 単色は単色のまま、2x2 の市松は平均色になる", () => {
  const r = resampleArea(solid(8, 8, 50, 60, 70), 2, 2);
  assert.deepEqual([...r.data.slice(0, 4)], [50, 60, 70, 255]);
  const chk = { width: 2, height: 2, data: Buffer.from([0,0,0,255, 200,200,200,255, 200,200,200,255, 0,0,0,255]) };
  assert.deepEqual([...resampleArea(chk, 1, 1).data], [100, 100, 100, 255]);
});

test("sha256 は画素が 1 つ違えば変わる", () => {
  const a = solid(4, 4, 1, 2, 3), b = solid(4, 4, 1, 2, 3);
  assert.equal(sha256(a), sha256(b));
  b.data[0] ^= 1;
  assert.notEqual(sha256(a), sha256(b));
});

test("resolveProject: 絶対パス / リポジトリ内相対 / 親フォルダ指定 / 見つからない", () => {
  const exists = (set) => (p) => set.has(path.normalize(p));
  const abs = path.resolve("/abs/PerfTest");
  assert.equal(resolveProject(abs, {}, exists(new Set([path.normalize(abs)]))), abs);
  const rel = path.resolve(REPO_ROOT, "tests/golden/project");
  assert.equal(resolveProject("tests/golden/project", {}, exists(new Set([path.normalize(rel)]))), rel);
  const alt = path.join("/other", "Nocturne");
  assert.equal(
    resolveProject("C:/Users/x/Nocturne", { DX12_GOLDEN_PROJECTS_ROOT: "/other" }, exists(new Set([path.normalize(alt)]))), alt);
  assert.equal(resolveProject("C:/Users/x/Nocturne", {}, () => false), null);
});

// ─── 絞り込み・引数（W0-2: 「SKIP の見かけ合格にしない」を守る純ロジック）─────────────────
import { selectScenes, parseArgs } from "./lib/sceneSelect.mjs";
const SC = [
  { name: "old", requiresProjectsRoot: true },
  { name: "core_a" }, { name: "core_b" },
  { name: "rep", group: "repeat" },
  { name: "old_rep", group: "repeat", requiresProjectsRoot: true },
];
test("selectScenes: ROOT が無ければ requiresProjectsRoot を対象から外す（SKIP にしない）", () => {
  const r = selectScenes(SC, { hasRoot: false });
  assert.deepEqual(r.scenes.map((s) => s.name), ["core_a", "core_b"]);
  assert.deepEqual(r.gated, ["old", "old_rep"]);
});
test("selectScenes: ROOT があれば requiresProjectsRoot も回る", () => {
  assert.deepEqual(selectScenes(SC, { hasRoot: true }).scenes.map((s) => s.name), ["old", "core_a", "core_b"]);
});
test("selectScenes: group 指定はその group だけ・無指定は group 無しだけ", () => {
  assert.deepEqual(selectScenes(SC, { group: "repeat" }).scenes.map((s) => s.name), ["rep"]);
  assert.ok(!selectScenes(SC, {}).scenes.some((s) => s.group));
});
test("selectScenes: --only は対象外のシーンを復活させない", () => {
  assert.equal(selectScenes(SC, { only: new Set(["old"]) }).scenes.length, 0);
  assert.deepEqual(selectScenes(SC, { only: new Set(["core_b"]) }).scenes.map((s) => s.name), ["core_b"]);
});
test("parseArgs: 既定は strict・--no-strict で緩む・--group/--only/--repeat", () => {
  assert.equal(parseArgs([]).strict, true);
  assert.equal(parseArgs(["--no-strict"]).strict, false);
  assert.equal(parseArgs(["--no-strict", "--strict"]).strict, true);
  const a = parseArgs(["--group", "repeat", "--only", "a, b", "--repeat", "2"]);
  assert.equal(a.group, "repeat");
  assert.deepEqual([...a.only], ["a", "b"]);
  assert.equal(a.repeat, 2);
  assert.throws(() => parseArgs(["--nanika"]), /不明な引数/);
});

test("C3: layout:material の UI ゴールデン 4 件（DPI 1.0/1.5 x default/b）が定義され、基準 PNG がある", () => {
  const g = loadGoldenEntries(path.join(REPO_ROOT, "tests/golden"));
  const mats = g.scenes.filter((s) => s.kind === "ui" && s.layout === "material");
  assert.equal(mats.length, 4);
  assert.deepEqual(mats.map((s) => `${s.dpi}/${s.theme}`).sort(), ["1.5/b", "1.5/default", "1/b", "1/default"]);
  for (const s of mats) assert.ok(fs.existsSync(path.join(REPO_ROOT, "tests/golden/ui", `${s.name}.png`)), `${s.name}.png`);
});

import { judgeDxr } from "./golden.mjs";
test("judgeDxr: Tier 1.0 / SM 不足は SKIP、対応ハードで supported:false は FAIL、例外は FAIL", () => {
  assert.equal(judgeDxr({ raytracingTier: "none", highestShaderModel: "6.7", supported: false }).status, "skip");
  assert.equal(judgeDxr({ raytracingTier: "1.0", highestShaderModel: "6.7", supported: false }).status, "skip");
  assert.equal(judgeDxr({ raytracingTier: "1.1", highestShaderModel: "6.4", supported: false }).status, "skip");
  assert.equal(judgeDxr({ raytracingTier: "1.1", highestShaderModel: "6.5", supported: false }).status, "fail");
  assert.equal(judgeDxr({ raytracingTier: "1.1", highestShaderModel: "6.7", supported: true }).status, "run");
  assert.equal(judgeDxr(new Error("x")).status, "fail");
});

// ─── W05-32: entries / baseline の分割 ───────────────────────────────────────
const mkGolden = (files) => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "golden-split-"));
  for (const [rel, obj] of Object.entries(files)) {
    fs.mkdirSync(path.dirname(path.join(dir, rel)), { recursive: true });
    fs.writeFileSync(path.join(dir, rel), JSON.stringify(obj));
  }
  return dir;
};
test("entries: order の ID が先（その順）、残りはファイル名の辞書順（作成順に依らない）", () => {
  const dir = mkGolden({
    "golden.json": { defaults: { width: 10 }, order: ["zeta", "alpha"], scenes: [] },
    "entries/new_b.json": { name: "new_b" }, "entries/alpha.json": { name: "alpha" },
    "entries/zeta.json": { name: "zeta" }, "entries/new_a.json": { name: "new_a" },
  });
  const r = loadGoldenEntries(dir);
  assert.deepEqual(r.scenes.map((s) => s.name), ["zeta", "alpha", "new_a", "new_b"]);
  assert.equal(r.defaults.width, 10);
});
test("entries: ID の重複（互換の scenes との重複を含む）・ファイル名との不一致・order の欠落はエラー", () => {
  assert.throws(() => loadGoldenEntries(mkGolden({ "golden.json": { scenes: [{ name: "a" }] }, "entries/a.json": { name: "a" } })), /重複/);
  assert.throws(() => loadGoldenEntries(mkGolden({ "entries/a.json": { name: "b" } })), /一致しない/);
  assert.throws(() => loadGoldenEntries(mkGolden({ "golden.json": { order: ["x"] }, "entries/a.json": { name: "a" } })), /order/);
});
test("writeBaselineEntries: 指定した ID のファイルだけを書き、他の ID には触れない", () => {
  const dir = mkGolden({ "baseline/a.json": { v: 1 }, "baseline/b.json": { v: 2 } });
  const bl = loadBaseline(dir);
  const fileA = path.join(dir, "baseline/a.json"), fileB = path.join(dir, "baseline/b.json");
  const old = new Date(2000, 0, 1);
  fs.utimesSync(fileB, old, old);
  bl.scenes.a = { v: 10 }; bl.scenes.b = { v: 20 }; bl.scenes.c = { v: 3 };
  assert.deepEqual(writeBaselineEntries(dir, bl, ["a", "c"]), ["a", "c"]);
  assert.equal(JSON.parse(fs.readFileSync(fileA, "utf8")).v, 10);
  assert.equal(JSON.parse(fs.readFileSync(path.join(dir, "baseline/c.json"), "utf8")).v, 3);
  assert.equal(JSON.parse(fs.readFileSync(fileB, "utf8")).v, 2);          // b は書き換わっていない
  assert.equal(fs.statSync(fileB).mtimeMs, old.getTime());
  assert.deepEqual(writeBaselineEntries(dir, bl, ["a"]), []);              // 同じ内容は書き直さない
});
test("リポジトリの entries と baseline: ID が重複せず、基準 PNG と baseline ファイルが対応する", () => {
  const dir = path.join(REPO_ROOT, "tests/golden");
  const cfg = loadGoldenEntries(dir).scenes;
  const bl = loadBaseline(dir).scenes;
  assert.equal(new Set(cfg.map((s) => s.name)).size, cfg.length);
  for (const s of cfg) assert.ok(bl[s.name], `baseline/${s.name}.json`);
  assert.deepEqual(Object.keys(bl).filter((k) => !cfg.some((s) => s.name === k)), []);
});
