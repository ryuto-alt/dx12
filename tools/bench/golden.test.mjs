// ゴールデン比較の純関数の単体テスト（GPU もエンジンも pngjs も要らない）。
//   node --test tools/bench
import test from "node:test";
import assert from "node:assert/strict";
import path from "node:path";
import { sha256, resampleArea, compareImages, judge } from "./lib/imageDiff.mjs";
import { resolveProject, REPO_ROOT } from "./lib/engine.mjs";

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
