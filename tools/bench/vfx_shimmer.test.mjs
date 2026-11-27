// vfx_shimmer.mjs の単体テスト（エンジンも GPU も要らない。合成の連番だけ）。
//   node --test tools/bench/vfx_shimmer.test.mjs
import test from "node:test";
import assert from "node:assert/strict";
import { analyze, loadLimits, box3 } from "./vfx_shimmer.mjs";

const W = 96, H = 64;
function frame(fn) {
  const data = new Uint8Array(W * H * 4);
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    const v = Math.max(0, Math.min(255, Math.round(fn(x, y))));
    const p = (y * W + x) * 4;
    data[p] = data[p + 1] = data[p + 2] = v; data[p + 3] = 255;
  }
  return { width: W, height: H, data };
}
// 決定論の疑似乱数（画素と時刻から）
const hash = (x, y, t) => { let h = (x * 374761393 + y * 668265263 + t * 2147483647) >>> 0; h = ((h ^ (h >>> 13)) * 1274126177) >>> 0; return ((h ^ (h >>> 16)) >>> 0) / 4294967295; };

// 暗い背景の上を、炎のような広い光の塊がゆっくり動きながら 8Hz（60fps で 7.5 フレーム周期）で明滅する
const flameLike = (n) => Array.from({ length: n }, (_, t) =>
  frame((x, y) => 12 + (110 + 60 * Math.sin((2 * Math.PI * t) / 7.5)) * Math.exp(-(((x - 30 - 0.5 * t) ** 2) + (y - 32) ** 2) / 90)));

// 同じ塊に、画素ごとに独立に ±50 で明滅するきらめき（TAA のちらつきの想定）が乗る
const sparkle = (n) => Array.from({ length: n }, (_, t) =>
  frame((x, y) => 12 + 110 * Math.exp(-(((x - 30 - 0.5 * t) ** 2) + (y - 32) ** 2) / 90) + (hash(x, y, t) < 0.5 ? 50 : -50) * Math.exp(-(((x - 30 - 0.5 * t) ** 2) + (y - 32) ** 2) / 400)));

test("広い範囲がいっしょに明滅する炎は、ちらつきに数えない", () => {
  const r = analyze(flameLike(24));
  assert.ok(r.coverage.maxPct > loadLimits().minCoveragePct, `被覆 ${r.coverage.maxPct}`);
  assert.ok(r.shimmer.ratio < loadLimits().shimmerRatioMax, `ratio ${r.shimmer.ratio}`);
  assert.equal(r.ok, true);
});

test("画素ごとに独立に明滅するきらめきは TEMPORAL_SHIMMER になる", () => {
  const r = analyze(sparkle(24));
  assert.ok(r.shimmer.ratio > loadLimits().shimmerRatioMax * 3, `ratio ${r.shimmer.ratio}`);
  assert.ok(r.issues.some((i) => i.code === "TEMPORAL_SHIMMER"));
  assert.equal(r.ok, false);
});

test("何も出ていない連番は LOW_COVERAGE", () => {
  const r = analyze(Array.from({ length: 6 }, () => frame(() => 20)));
  assert.ok(r.issues.some((i) => i.code === "LOW_COVERAGE"));
});

test("背景を渡すと、中央値より正確に被覆が出る（最初から最後まで光っている塊も数える）", () => {
  const bg = frame(() => 12);
  const always = Array.from({ length: 6 }, () => frame((x, y) => 12 + (Math.hypot(x - 48, y - 32) < 10 ? 120 : 0)));
  const noBg = analyze(always);
  assert.ok(noBg.coverage.maxPct < 0.01, "背景を渡さないと塊が背景と見なされる");
  const withBg = analyze(always, { background: bg });
  assert.ok(withBg.coverage.maxPct > 4);
});

test("box3 は定数を保ち、縁も端の値で延長する", () => {
  const out = box3(new Float32Array(W * H).fill(7), W, H);
  assert.ok(out.every((v) => Math.abs(v - 7) < 1e-5));
});

test("フレームが足りない・大きさが違うと明確に失敗する", () => {
  assert.throws(() => analyze([frame(() => 0), frame(() => 0)]), /3 枚以上/);
  const small = { width: 2, height: 2, data: new Uint8Array(16) };
  assert.throws(() => analyze([frame(() => 0), frame(() => 0), small]), /揃っていない/);
});
