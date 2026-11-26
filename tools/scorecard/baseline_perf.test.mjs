// 性能ベースラインの純関数の単体テスト（GPU もエンジンも要らない）。 node --test tools/scorecard/baseline_perf.test.mjs
import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import {
  median, stddev, summarizeWindows, abThreshold, dropByClock, compareAB, ababSchedule, vramGate,
  emptyBaseline, mergeInto, validateBaseline, acquireGpuWindow, parseArgs, clockStats, EXPECTED_SCENES,
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

test("VRAM の門: Budget が 6.5GB 未満なら中止（nvidia-smi の使用量は引かない）、Budget が取れなければ中止", () => {
  assert.equal(vramGate(7127, 2122).ok, true);          // 実測（アイドル）: 通る
  assert.equal(vramGate(7600, null).ok, true);          // 他の使用量が取れなくても Budget で判定
  const g = vramGate(6000, 100);
  assert.equal(g.ok, false);
  assert.match(g.reason, /6500MB/);
  assert.equal(vramGate(NaN, 100).ok, false);
  assert.equal(vramGate(undefined, 100).ok, false);
});

test("dropByClock: 共通の基準（refMHz）を渡すと A/B と A/A で同じ基準になる", () => {
  const r = dropByClock([{ index: 0, clockA: 2500, clockB: 2500 }], 3, 2300);   // 自分の中央値なら残るが、基準 2300 からは 8%
  assert.equal(r.dropped.length, 1);
  assert.equal(r.refMHz, 2300);
});

test("parseArgs: --a-args の --no-dred は aNoDred に読み替え、--dred は拒否", () => {
  const base = ["ab", "--id", "dred", "--a-dir", ".", "--b-dir", ".", "--a-label", "a", "--b-label", "b"];
  const o = parseArgs([...base, "--a-args", "--no-dred --foo"]);
  assert.equal(o.aNoDred, true);
  assert.deepEqual(o.aArgs, ["--foo"]);
  assert.equal(o.bNoDred, false);
  assert.throws(() => parseArgs([...base, "--b-args", "--dred"]));
  assert.equal(parseArgs([...base, "--vram-min-mb", "5000"]).smoke, false);   // 門の値は試走の判定に入れない
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

const sc = (mk) => Object.fromEntries(EXPECTED_SCENES.map((id) => [id, mk]));
const mkSnap = (label) => ({ label, noDred: true, dredFlag: label === "pre-overhaul" ? "binary-has-no-dred" : "passed", smoke: false, engine: { exeSha256: "a".repeat(64) }, scenes: sc({ keptRounds: 3, summary: { frameMsTypical: 1.2 } }) });
const mkAb = (id) => ({ id, smoke: false, sameBinary: true, a: { noDred: true, dredFlag: "passed", extraArgs: [] }, b: { noDred: id !== "dred", dredFlag: "passed", extraArgs: [] }, aa: { pairs: 3 }, scenes: sc({ verdict: "within" }) });

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

const REQ = ["pre-overhaul", "w05-end", "dred", "f0", "hooks"];
const full = () => { let j = emptyBaseline(); for (const l of ["pre-overhaul", "w05-end"]) j = mergeInto(j, { snapshot: mkSnap(l) }); for (const id of ["dred", "f0", "hooks"]) j = mergeInto(j, { abTest: mkAb(id) }); return j; };

test("validate: 試走の記録は --require で NG（公式を後から回しても試走が混ざらない）", () => {
  const j = mergeInto(full(), { snapshot: { ...mkSnap("w05-end"), smoke: true } });
  assert.ok(validateBaseline(j, REQ).errors.some((e) => /試走/.test(e)));
  // 試走の後に別の記録を併合しても、試走の smoke は消えない（記録ごとに持つ）
  const j2 = mergeInto(j, { abTest: mkAb("f0") });
  assert.ok(validateBaseline(j2, REQ).errors.some((e) => /w05-end.*試走/.test(e)));
});

test("validate: DRED を揃えていない記録は NG", () => {
  const a = full(); a.snapshots["w05-end"].noDred = false;
  assert.ok(validateBaseline(a, REQ).errors.some((e) => /no-dred/.test(e)));
  const b = full(); b.abTests.dred.a.dredFlag = "binary-has-no-dred";
  assert.ok(validateBaseline(b, REQ).errors.some((e) => /旗/.test(e)));
});

test("validate: DRED の B 側が OFF の A/B、旗を受け取っていない w05-end と hooks は NG", () => {
  const a = full(); a.abTests.dred.b.extraArgs = ["--no-dred"];
  assert.ok(validateBaseline(a, REQ).errors.some((e) => /B 側が DRED OFF/.test(e)));
  const b = full(); b.abTests.dred.b.noDred = true;
  assert.ok(validateBaseline(b, REQ).errors.some((e) => /B 側が DRED OFF/.test(e)));
  const c = full(); c.snapshots["w05-end"].dredFlag = "binary-has-no-dred";
  assert.ok(validateBaseline(c, REQ).errors.some((e) => /w05-end.*受け取っていない/.test(e)));
  const d = full(); d.abTests.hooks.b.dredFlag = "binary-has-no-dred";
  assert.ok(validateBaseline(d, REQ).errors.some((e) => /ab hooks.*受け取っていない/.test(e)));
});

test("validate: シーンの欠落・集合の不一致は NG", () => {
  const a = full(); delete a.snapshots["w05-end"].scenes[EXPECTED_SCENES[0]];
  const e = validateBaseline(a, REQ).errors.join("; ");
  assert.match(e, /欠けている/);
  assert.match(e, /集合が違う/);
  const b = full(); b.abTests.f0.restrictedScenes = true;
  assert.ok(validateBaseline(b, REQ).errors.some((x) => /絞った/.test(x)));
});

test("dropByClock: クロックが二峰だと全回が捨てられうる（現状の仕様を固定）", () => {
  const r = dropByClock([{ index: 0, clockA: 2000 }, { index: 1, clockA: 2500 }]);   // 中央値 2250 から 11% ずつ
  assert.equal(r.kept.length, 0);
  assert.equal(r.dropped.length, 2);
});

test("試走の既定出力先の拒否に使う outExplicit", () => {
  assert.equal(parseArgs(["snapshot", "--label", "x", "--engine-dir", ".", "--frames", "60"]).outExplicit, undefined);
  assert.equal(parseArgs(["snapshot", "--label", "x", "--engine-dir", ".", "--frames", "60", "--out", "x.json"]).outExplicit, true);
  assert.throws(() => parseArgs(["probe"]));
});

const tmpDirs = [];
const mkLockFile = () => { const d = fs.mkdtempSync(path.join(os.tmpdir(), "lk-")); tmpDirs.push(d); return path.join(d, "w.lock"); };
test.after(() => { for (const d of tmpDirs) { try { fs.rmSync(d, { recursive: true, force: true }); } catch { /* 無視 */ } } });

test("計測窓のロック: 排他、古い持ち主は奪う、解放で消える", async () => {
  const f = mkLockFile();
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

test("古いロックの奪取: 持ち主の PID が居ないときだけ。生きている持ち主のロックは消さない", async () => {
  const f = mkLockFile();
  fs.writeFileSync(f, JSON.stringify({ pid: process.pid, owner: "alive" }));
  assert.equal(await acquireGpuWindow({ file: f, waitSec: 0 }), null);
  assert.equal(JSON.parse(fs.readFileSync(f, "utf8")).owner, "alive");
  // 古いロックを 2 つが同時に奪いに行っても、片方だけが持つ
  fs.writeFileSync(f, JSON.stringify({ pid: 2147483000, owner: "dead" }));
  const rs = await Promise.all([acquireGpuWindow({ file: f }), acquireGpuWindow({ file: f })]);
  assert.equal(rs.filter(Boolean).length, 1);
  rs.filter(Boolean)[0]();
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
