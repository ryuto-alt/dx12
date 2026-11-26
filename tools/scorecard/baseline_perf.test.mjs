// 性能ベースラインの純関数の単体テスト（GPU もエンジンも要らない）。 node --test tools/scorecard/baseline_perf.test.mjs
import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import {
  median, stddev, summarizeWindows, abThreshold, dropByClock, compareAB, ababSchedule, vramGate,
  emptyBaseline, mergeInto, validateBaseline, acquireGpuWindow, parseArgs, clockStats,
} from "./baseline_perf.mjs";

test("median / stddev", () => {
  assert.equal(median([3, 1, 2]), 2);
  assert.equal(median([1, 2, 3, 4]), 2.5);
  assert.equal(median([]), null);
  assert.equal(stddev([1]), null);
  assert.ok(Math.abs(stddev([1, 2, 3]) - 1) < 1e-12);
});

test("ABAB の順序は A,B を rounds 往復", () => {
  assert.deepEqual(ababSchedule(2).map((x) => x.side).join(""), "ABAB");
  assert.equal(ababSchedule(5).length, 10);
});

test("閾値 = max(0.05ms, 1%, A/A の 2σ)", () => {
  assert.equal(abThreshold(2, []).threshold, 0.05);            // 床
  assert.equal(abThreshold(10, []).threshold, 0.1);            // 1%
  const t = abThreshold(2, [0.1, -0.1, 0.1, -0.1, 0.1]);       // σ ≈ 0.1095 → 2σ ≈ 0.219
  assert.ok(t.threshold > 0.2 && t.threshold < 0.23, String(t.threshold));
});

test("クロックが 3% 以上違う回は捨てる（全体の中央値からも、ペアの相手からも）", () => {
  const r = dropByClock([
    { index: 0, clockA: 2500, clockB: 2510 },
    { index: 1, clockA: 2500, clockB: 2400 },   // ペアで 4% 違う
    { index: 2, clockA: 2490, clockB: 2495 },
    { index: 3, clockA: 2000, clockB: 2005 },   // 全体から 20% 低い
    { index: 4, clockA: 2505, clockB: NaN },    // 取得できなかった
  ]);
  assert.deepEqual(r.kept.map((x) => x.index), [0, 2]);
  assert.deepEqual(r.dropped.map((x) => x.index), [1, 3, 4]);
});

test("1 側だけ（snapshot）でも捨てる", () => {
  const r = dropByClock([{ index: 0, clockA: 2500 }, { index: 1, clockA: 2510 }, { index: 2, clockA: 2200 }]);
  assert.deepEqual(r.dropped.map((x) => x.index), [2]);
});

test("compareAB: 閾値内は within、超えたら遅い側を指す、データ不足は insufficient", () => {
  assert.equal(compareAB([2, 2.01, 2], [2.02, 2.01, 2.0], [0.01, -0.01]).verdict, "within");
  assert.equal(compareAB([2, 2, 2], [2.5, 2.5, 2.5], [0.01, -0.01]).verdict, "b-slower");
  assert.equal(compareAB([2, 2, 2], [1.5, 1.5, 1.5], [0.01, -0.01]).verdict, "b-faster");
  assert.equal(compareAB([], [1], []).verdict, "insufficient");
});

test("VRAM の門: Budget − 他の使用量 < 6.5GB は中止、取れなければ中止", () => {
  assert.equal(vramGate(7600, 1000).ok, true);          // 6600 >= 6500
  const g = vramGate(7600, 2000);                       // 5600
  assert.equal(g.ok, false);
  assert.equal(g.availableMB, 5600);
  assert.match(g.reason, /6500MB/);
  assert.equal(vramGate(NaN, 100).ok, false);
  assert.equal(vramGate(7600, null).ok, false);
});

test("summarizeWindows: 窓の中央値", () => {
  const s = summarizeWindows([{ frameMsAvg: 2, frameMsP95: 3, gpuTotalMs: 1 }, { frameMsAvg: 4, frameMsP95: 5, gpuTotalMs: 3 }, { frameMsAvg: 3, frameMsP95: 4, gpuTotalMs: 2 }]);
  assert.equal(s.frameMsTypical, 3);
  assert.equal(s.gpuTotalMs, 2);
  assert.equal(s.windows, 3);
});

test("clockStats: アイドルの低いクロックを混ぜない", () => {
  const c = clockStats([{ smMHz: 2500, tempC: 60 }, { smMHz: 2510, tempC: 61 }, { smMHz: 210, tempC: 40 }]);
  assert.equal(c.smMHz, 2505);
  assert.equal(c.n, 2);
});

const mkSnap = (label) => ({ label, engine: { exeSha256: "a".repeat(64) }, scenes: { s1: { keptRounds: 3, summary: { frameMsTypical: 1.2 } } } });
const mkAb = (id) => ({ id, aa: { pairs: 3 }, scenes: { s1: { verdict: "within" } } });

test("mergeInto は同じキーだけ置き換える", () => {
  let j = mergeInto(emptyBaseline(), { snapshot: mkSnap("pre-overhaul") });
  j = mergeInto(j, { abTest: mkAb("dred") });
  j = mergeInto(j, { snapshot: { ...mkSnap("pre-overhaul"), noDred: true } });
  assert.deepEqual(Object.keys(j.snapshots), ["pre-overhaul"]);
  assert.equal(j.snapshots["pre-overhaul"].noDred, true);
  assert.ok(j.abTests.dred);
});

test("validate: 2 本・A/B がそろえば ok、欠ければ NG、絶対パスは NG", () => {
  let j = emptyBaseline();
  for (const l of ["pre-overhaul", "w05-end"]) j = mergeInto(j, { snapshot: mkSnap(l) });
  for (const id of ["dred", "f0", "hooks"]) j = mergeInto(j, { abTest: mkAb(id) });
  assert.ok(validateBaseline(j, ["pre-overhaul", "w05-end", "dred", "f0", "hooks"]).ok);
  assert.ok(!validateBaseline(mergeInto(emptyBaseline(), { snapshot: mkSnap("pre-overhaul") }), ["w05-end"]).ok);
  const bad = mergeInto(j, { snapshot: { ...mkSnap("x"), note: "C:\\Users\\someone\\x" } });
  assert.ok(validateBaseline(bad).errors.some((e) => /絶対パス/.test(e)));
  const noAa = mergeInto(emptyBaseline(), { abTest: { id: "f0", aa: { pairs: 0 }, scenes: { s1: { verdict: "within" } } } });
  assert.ok(!validateBaseline(noAa).ok);
});

test("計測窓のロック: 排他、古い持ち主は奪う、解放で消える", async () => {
  const f = path.join(fs.mkdtempSync(path.join(os.tmpdir(), "lk-")), "w.lock");
  const rel = await acquireGpuWindow({ file: f });
  assert.ok(rel);
  assert.equal(await acquireGpuWindow({ file: f, waitSec: 0 }), null);   // 自分が持っている
  rel();
  assert.ok(!fs.existsSync(f));
  fs.writeFileSync(f, JSON.stringify({ pid: 2147483000, owner: "dead" }));   // 存在しない PID
  const rel2 = await acquireGpuWindow({ file: f });
  assert.ok(rel2);
  rel2();
});

test("parseArgs: 必須引数と試走の判定", () => {
  assert.throws(() => parseArgs(["snapshot"]));
  assert.throws(() => parseArgs(["snapshot", "--label", "a b", "--engine-dir", "."]));
  const o = parseArgs(["snapshot", "--label", "pre-overhaul", "--engine-dir", "."]);
  assert.equal(o.smoke, false);
  assert.equal(o.warmup, 300);
  assert.equal(o.frames, 600);
  assert.equal(o.windows, 3);
  assert.equal(parseArgs(["snapshot", "--label", "x", "--engine-dir", ".", "--frames", "60"]).smoke, true);
  assert.throws(() => parseArgs(["ab", "--id", "dred"]));
});
