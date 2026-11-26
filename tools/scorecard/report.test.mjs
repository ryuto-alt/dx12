// スコアカードの純関数の単体テスト（GPU もエンジンも要らない）。 node --test tools/scorecard/report.test.mjs
import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { ema, parseArgs, sanitizeMsg } from "./capture_engine.mjs";
import { compareRuns, diffSummaries, summarizeRun, relOk, scrub, TOL } from "./report.mjs";

test("ema: 先頭から alpha で畳む", () => {
  assert.equal(ema([]), null);
  assert.equal(ema([2]), 2);
  assert.ok(Math.abs(ema([1, 2], 0.5) - 1.5) < 1e-12);
});

function mkRun(startup, vfx, sha = "aa") {
  const d = fs.mkdtempSync(path.join(os.tmpdir(), "sc-"));
  fs.writeFileSync(path.join(d, "stage_startup.json"), JSON.stringify({ ok: true, errors: [], notes: [], items: [{ id: "startup_warm_ema", kind: "metric", value: startup, runs: [] }] }));
  fs.writeFileSync(path.join(d, "stage_templates.json"), JSON.stringify({ ok: true, errors: [], notes: [], items: [{ id: "template_empty", kind: "image", file: "x.png", sha256: sha }] }));
  fs.writeFileSync(path.join(d, "stage_vfx.json"), JSON.stringify({ ok: true, errors: [], notes: [], items: [{ id: "vfx_a", kind: "image", file: "v.png", sha256: sha, metrics: vfx }] }));
  return d;
}

test("compare: 起動秒数は max(0.3s, 15%) 以内", () => {
  const m = { coverage: 0.1, lumaGain: 0.05, flicker: 0.01 };
  assert.ok(compareRuns(mkRun(1.0, m), mkRun(1.25, m)).ok);
  assert.ok(!compareRuns(mkRun(1.0, m), mkRun(1.6, m)).ok);
  assert.ok(compareRuns(mkRun(3.0, m), mkRun(3.4, m)).ok);   // 3.0s の 15% = 0.45s
});

test("compare: VFX 指標は絶対 2e-3 か相対 30%", () => {
  const a = { coverage: 0.1, lumaGain: 0.05, flicker: 0.01 };
  assert.ok(compareRuns(mkRun(1, a), mkRun(1, { ...a, coverage: 0.12 })).ok);
  assert.ok(!compareRuns(mkRun(1, a), mkRun(1, { ...a, coverage: 0.3 })).ok);
});

test("compare: 画素ハッシュが違い、画像ファイルも無ければ不合格", () => {
  const m = { coverage: 0.1, lumaGain: 0.05, flicker: 0.01 };
  const r = compareRuns(mkRun(1, m, "aa"), mkRun(1, m, "bb"));
  assert.ok(!r.ok);   // templates の画像が範囲外（VFX は指標のみで判定）
  assert.ok(r.mismatches.includes("templates/template_empty"));
});

test("summarize: 絶対パスを含まず pending 欄を持つ", () => {
  const d = mkRun(1, { coverage: 0, lumaGain: 0, flicker: 0 });
  const s = summarizeRun(d);
  assert.equal(s.wave, "w0");
  assert.ok(s.pending.mcp14);
  assert.ok(!JSON.stringify(s).includes(os.tmpdir().replaceAll("\\", "\\\\")));
});

test("diff: 変化なし / 変化あり / 数値の増減", () => {
  const m = { coverage: 0.1, lumaGain: 0.05, flicker: 0.01 };
  const a = summarizeRun(mkRun(2, m, "aa")), b = summarizeRun(mkRun(1, m, "bb"));
  const t = diffSummaries(a, b).text;
  assert.match(t, /template_empty\s+changed/);
  assert.match(t, /startup_warm_ema/);
  assert.ok(TOL.startupRel > 0);
});

test("relOk: 小さい指標でも絶対差の床が効く（0.01 以下なら何でも通るわけではない）", () => {
  assert.ok(!relOk(0.004, 0.009, TOL.vfxAbs, TOL.vfxRel));
  assert.ok(relOk(0.004, 0.0041, TOL.vfxAbs, TOL.vfxRel));
  assert.ok(relOk(null, null, 1, 1));
  assert.ok(!relOk(null, 1, 1, 1));
});

test("compare: 片方にしかない項目は不合格", () => {
  const m = { coverage: 0.1, lumaGain: 0.05, flicker: 0.01 };
  const a = mkRun(1, m), b = mkRun(1, m);
  const f = path.join(b, "stage_vfx.json");
  const j = JSON.parse(fs.readFileSync(f, "utf8")); j.items.push({ id: "vfx_extra", kind: "image", file: "e.png", sha256: "x", metrics: m });
  fs.writeFileSync(f, JSON.stringify(j));
  const r = compareRuns(a, b);
  assert.ok(!r.ok);
  assert.ok(r.mismatches.includes("vfx/vfx_extra"));
  const r2 = compareRuns(b, a);
  assert.ok(r2.mismatches.includes("vfx/vfx_extra"));
});

test("parseArgs: 既定ポートは lane-d の 8855、不正な引数は拒む", () => {
  const base = ["--stage", "vfx", "--engine-dir", ".", "--run-dir", ".", "--tool-dir", "."];
  assert.equal(parseArgs(base).port, 8855);
  assert.throws(() => parseArgs([...base, "--bogus"]));
  assert.throws(() => parseArgs(["--stage", "nope", "--engine-dir", ".", "--run-dir", ".", "--tool-dir", "."]));
});

test("要約の文言から絶対パスを消す", () => {
  const m = "failed C:\\Users\\x\\a.exe and /Users/x/b";
  assert.ok(!/Users/.test(scrub(m)));
  assert.ok(!/Users/.test(sanitizeMsg(m)));
});
