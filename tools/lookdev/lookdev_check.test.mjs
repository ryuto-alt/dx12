// lookdev_check の純関数の単体テスト（GPU もエンジンも要らない）。 node --test tools/lookdev
import test from "node:test";
import assert from "node:assert/strict";
import { parseLookdevArgs, lumaStats, judgeV2, flipMean, judgePtFlip } from "./lookdev_check.mjs";

const solid = (w, h, r, g, b) => {
  const data = Buffer.alloc(w * h * 4);
  for (let i = 0; i < w * h; i++) { data[i * 4] = r; data[i * 4 + 1] = g; data[i * 4 + 2] = b; data[i * 4 + 3] = 255; }
  return { width: w, height: h, data };
};

test("引数: 既定は V1 と V2 の両方・2 回撮る", () => {
  const a = parseLookdevArgs([], { USERPROFILE: "X" });
  assert.deepEqual(a.revisions, [1, 2]);
  assert.equal(a.repeat, 2);
  assert.equal(a.v2RecordOnly, false);
});

test("引数: --only と --revisions と範囲外の拒否", () => {
  const a = parseLookdevArgs(["--only", "a, b", "--revisions", "2", "--v2-record-only"], {});
  assert.deepEqual([...a.only], ["a", "b"]);
  assert.deepEqual(a.revisions, [2]);
  assert.equal(a.v2RecordOnly, true);
  assert.throws(() => parseLookdevArgs(["--revisions", "1,3"], {}), /1 と 2/);
  assert.throws(() => parseLookdevArgs(["--nope"], {}), /不明な引数/);
  assert.throws(() => parseLookdevArgs(["--port"], {}), /値が要る/);
});

test("輝度の統計: 白は 1・飽和 100%、黒は 0・暗部 100%", () => {
  const w = lumaStats(solid(8, 8, 255, 255, 255));
  assert.ok(Math.abs(w.mean - 1) < 1e-9 && w.clippedRatio === 1 && w.darkRatio === 0);
  const k = lumaStats(solid(8, 8, 0, 0, 0));
  assert.ok(k.mean === 0 && k.darkRatio === 1 && k.clippedRatio === 0);
});

test("V2 の判定: しきい値内は合格、超えたら理由つきで不合格、record-only は常に合格", () => {
  const a = { v2MaxMeanAbs: 0.25, v2MaxDiffRatio: 0.001, v2RecordOnly: false };
  assert.equal(judgeV2({ diffRatio: 0, meanAbs: 0 }, a).pass, true);
  const bad = judgeV2({ diffRatio: 0.01, meanAbs: 1 }, a);
  assert.equal(bad.pass, false);
  assert.equal(bad.reasons.length, 2);
  assert.equal(judgeV2({ diffRatio: 0.5, meanAbs: 50 }, { ...a, v2RecordOnly: true }).pass, true);
});

test("PT 基準: 既定は切。FLIP の判定は計算できたときだけ効く", () => {
  const a = parseLookdevArgs(["--pt-reference", "--pt-spp", "64"], {});
  assert.equal(a.ptReference, true);
  assert.equal(a.ptSpp, 64);
  assert.equal(parseLookdevArgs([], {}).ptReference, false);
  assert.throws(() => parseLookdevArgs(["--pt-reference", "--no-save-images"], {}), /画像の保存/);
  assert.equal(flipMean({ metrics: { flip_ldr_mean: 0.05 } }), 0.05);
  assert.equal(flipMean({ skipped: "x" }), null);
  assert.equal(judgePtFlip({ metrics: { flip_ldr_mean: 0.05 } }, 0.08).pass, true);
  assert.equal(judgePtFlip({ metrics: { flip_ldr_mean: 0.2 } }, 0.08).pass, false);
  const sk = judgePtFlip({ skipped: "x" }, 0.08);
  assert.equal(sk.inconclusive, true);
  assert.equal(judgePtFlip({ error: "e" }, 0.08).pass, false);
  assert.equal(judgePtFlip({ metrics: {} }, 0.08).inconclusive, true);
  const tr = judgePtFlip({ metrics: { flip_ldr_mean: 0.01 } }, 0.08, { truncated: true });
  assert.equal(tr.inconclusive, true);
  assert.ok(tr.reasons.length);
});
