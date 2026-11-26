// スコアカードの純関数の単体テスト（GPU もエンジンも要らない）。 node --test tools/scorecard/report.test.mjs
import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { fileURLToPath } from "node:url";
import { ema, parseArgs, sanitizeMsg } from "./capture_engine.mjs";
import { compareRuns, deriveVfxTol, vfxFloor, diffSummaries, summarizeRun, validateMcp14, buildMcp14, personalPathReason, relOk, scrub, TOL } from "./report.mjs";

test("ema: 先頭から alpha で畳む", () => {
  assert.equal(ema([]), null);
  assert.equal(ema([2]), 2);
  assert.ok(Math.abs(ema([1, 2], 0.5) - 1.5) < 1e-12);
});

const tmpDirs = [];
test.after(() => { for (const d of tmpDirs) fs.rmSync(d, { recursive: true, force: true }); });

function mkRun(startup, vfx, sha = "aa") {
  const d = fs.mkdtempSync(path.join(os.tmpdir(), "sc-"));
  tmpDirs.push(d);
  fs.writeFileSync(path.join(d, "stage_startup.json"), JSON.stringify({ ok: true, errors: [], notes: [], items: [{ id: "startup_ready_ema", kind: "metric", value: startup, runs: [] }] }));
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

test("compare: VFX 指標は指標別の絶対差の床か相対 30%", () => {
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

const H = "a".repeat(64);
const IDS = ["T1", "T2", "T3", "T4", "T5", "T6"];
const mkRec = () => ({ subagentModelId: "claude-sonnet-5-5", procedureVersion: "e2e-0123456789ab", templateSha256: H, tasksJsonSha256: H, e2eToolsCommit: "abc1234",
  tsRef: "pre-overhaul", tsCommit: "def5678", buildRef: "pre-overhaul", buildCommit: "1234567", launchMode: "agent-tool", pointerRegistered: true,
  otherMcpIsolated: true, isolationMethod: "see https://example.com/docs and a --strict-mcp-config", port: 8856, noPaidApiKeyChecked: true, replayMatch: true,
  tasks: IDS.map((id) => ({ id, pass: true, calls: 5, retries: 0, errors: 0, traceSha256: H, scoreSha256: H })) });

test("mcp14: 記録の検査（形式・id・非有限値・URL は許す・個人のパスは拒む）", () => {
  const rec = mkRec();
  assert.deepEqual(validateMcp14(rec, { userName: "someuser" }), []);
  assert.ok(validateMcp14({ ...rec, replayMatch: false }).length);
  assert.ok(validateMcp14({ ...rec, tasks: rec.tasks.slice(1) }).length);
  assert.ok(validateMcp14({ ...rec, launchMode: "claude-p" }).length);
  assert.ok(validateMcp14({ ...rec, pointerRegistered: false }).length);
  assert.ok(validateMcp14({ ...rec, tasks: rec.tasks.map((t, i) => (i ? t : { ...t, calls: NaN })) }).length);
  assert.ok(validateMcp14({ ...rec, tasks: rec.tasks.map((t, i) => ({ ...t, id: "X" + i })) }).length);
  assert.ok(validateMcp14({ ...rec, tasks: rec.tasks.map((t, i) => (i ? t : { ...t, pass: 1 })) }).length);
  for (const p of ["C:\\Users\\x", "/c/Users/x/y", "\\\\host\\share","~/work", "/home/x/y", "as someuser did"])
    assert.ok(personalPathReason(p, "someuser"), p);
  assert.equal(personalPathReason("see https://docs.example.com/a", "someuser"), null);
});

test("mcp14: summarize への併合と、無ければ pending のまま", () => {
  const d = mkRun(1, { coverage: 0, lumaGain: 0, flicker: 0 });
  const s = summarizeRun(d, { mcp14: mkRec() });
  assert.equal(s.mcp14.tasks.length, 6);
  assert.equal(s.pending.mcp14, undefined);
  assert.ok(summarizeRun(d).pending.mcp14);
});

test("mcp14: buildMcp14 は欠けた出力を拒み、CLI は不正入力で終了コード 1", () => {
  const d = fs.mkdtempSync(path.join(os.tmpdir(), "m14-")); tmpDirs.push(d);
  const r = buildMcp14(d, { model: "m", isolation: "x" }, {});
  assert.ok(r.errors.length && !r.record);
  assert.ok(buildMcp14(d, {}, {}).errors.some((m) => m.includes("--model")));
  const prefix = "ANTHROPIC_";
  assert.ok(buildMcp14(d, { model: "m", isolation: "x" }, { [prefix + "API_KEY"]: "k" }).errors.some((m) => m.includes("有料")));
  const cli = path.join(path.dirname(fileURLToPath(import.meta.url)), "report.mjs");
  const w0 = path.join(d, "w0.json"); fs.writeFileSync(w0, JSON.stringify({ pending: { mcp14: "x" } }));
  const p = spawnSync(process.execPath, [cli, "mcp14-set", "--dir", d, "--model", "m", "--isolation", "x", "--w0", w0], { encoding: "utf8" });
  assert.equal(p.status, 1);
  assert.ok(JSON.parse(fs.readFileSync(w0, "utf8")).pending.mcp14);   // 失敗時は書かない
});

test("mcp14: 完全な出力から記録を組み立てて検査を通す", () => {
  const d = fs.mkdtempSync(path.join(os.tmpdir(), "m14-")); tmpDirs.push(d);
  fs.writeFileSync(path.join(d, "mcp.pointer.json"), "{}"); fs.writeFileSync(path.join(d, "current_trace.txt"), "");
  const prov = { tsRef: "pre-overhaul", tsCommit: "def5678abc", tsDirty: false, buildRef: "pre-overhaul", buildCommit: "1234567abc", assetsSource: "x" };
  for (const id of IDS) {
    const td = path.join(d, "task_" + id); fs.mkdirSync(td);
    const sc = { taskId: id, pass: true, checks: [{ id: "c", ok: true, detail: "" }], metrics: { calls: 4, retries: 0, errors: 0, maxCalls: 6, byTool: {} } };
    fs.writeFileSync(path.join(td, `score_${id}.json`), JSON.stringify(sc)); fs.writeFileSync(path.join(td, `trace_${id}.replay.score.json`), JSON.stringify(sc));
    fs.writeFileSync(path.join(td, `trace_${id}.jsonl`), "{}\n"); fs.writeFileSync(path.join(td, "provenance.json"), JSON.stringify(prov));
    fs.writeFileSync(path.join(td, `task_${id}.md`), `# ${id}\n作業フォルダ: ${td}\n`);
  }
  const r = buildMcp14(d, { model: "claude-sonnet-5-5", isolation: "strict config" }, {});
  assert.deepEqual(r.errors, undefined, JSON.stringify(r.errors));
  assert.equal(r.record.tasks[0].traceSha256, createHash("sha256").update("{}\n").digest("hex"));
  assert.match(r.record.procedureVersion, /^e2e-[0-9a-f]{12}$/);
  // replay が違えば拒む
  const f = path.join(d, "task_T3", "trace_T3.replay.score.json"); const j = JSON.parse(fs.readFileSync(f, "utf8")); j.metrics.calls = 9; fs.writeFileSync(f, JSON.stringify(j));
  assert.ok(buildMcp14(d, { model: "m", isolation: "x" }, {}).errors.some((m) => m.includes("T3") && m.includes("replay")));
});

test("diff: mcp14 の行（条件が同じなら増減、違えば比較不可）", () => {
  const base = { stages: {}, mcp14: mkRec() }, now = { stages: {}, mcp14: mkRec() };
  now.mcp14.tasks[0].calls = 7; now.mcp14.tasks[1].pass = false;
  const t = diffSummaries(base, now).text;
  assert.match(t, /T1\.calls\s+5\.000 -> 7\.000/);
  assert.match(t, /T2\.pass/);
  now.mcp14.subagentModelId = "other";
  assert.match(diffSummaries(base, now).text, /not comparable \(subagentModelId/);
});

test("diff: 変化なし / 変化あり / 数値の増減", () => {
  const m = { coverage: 0.1, lumaGain: 0.05, flicker: 0.01 };
  const a = summarizeRun(mkRun(2, m, "aa")), b = summarizeRun(mkRun(1, m, "bb"));
  const t = diffSummaries(a, b).text;
  assert.match(t, /template_empty\s+changed/);
  assert.match(t, /startup_ready_ema/);
  assert.ok(TOL.startupRel > 0);
});

test("relOk: 小さい指標でも絶対差の床が効く（0.01 以下なら何でも通るわけではない）", () => {
  assert.ok(!relOk(0.004, 0.009, TOL.vfxAbsByMetric.coverage, TOL.vfxRel));
  assert.ok(relOk(0.004, 0.0041, TOL.vfxAbsByMetric.coverage, TOL.vfxRel));
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

test("compare: 片方にしか無い段は不合格、両方に無い段は部分比較（受け入れは全段必須）", () => {
  const m = { coverage: 0.1, lumaGain: 0.05, flicker: 0.01 };
  const a = mkRun(1, m), b = mkRun(1, m);
  fs.rmSync(path.join(b, "stage_startup.json"));
  const r = compareRuns(a, b);
  assert.ok(!r.ok);
  assert.ok(r.stages.startup.asymmetric);
  const c = mkRun(1, m), d = mkRun(1, m);
  const ok = compareRuns(c, d);
  assert.ok(ok.ok);
  assert.ok(ok.partial.includes("splash"));          // 撮っていない段は partial に出る
  const strict = compareRuns(c, d, { expectStages: ["splash", "vfx"], vfxTol: {} });
  assert.ok(!strict.ok);
  assert.ok(strict.mismatches.some((x) => x.startsWith("splash:")));
  assert.ok(!compareRuns(c, d, { expectStages: ["vfx"] }).ok);   // --vfx-tol 無しは受け入れにならない
});

test("VFX: 指標別の床とレシピ別の上書き / 3 回の撮影から導出", () => {
  assert.ok(vfxFloor(null, "x", "flicker") < vfxFloor(null, "x", "coverage"));
  assert.equal(vfxFloor({ x: { flicker: 0.01 } }, "x", "flicker"), 0.01);
  const mk = (f) => { const d = mkRun(1, { coverage: 0.1, lumaGain: 0.05, flicker: f }); return d; };
  const t = deriveVfxTol([mk(0.01), mk(0.02), mk(0.012)]);
  assert.ok(Math.abs(t.vfx_a.flicker - 0.015) < 1e-9);   // 最大差 0.01 の 1.5 倍
  assert.equal(t.vfx_a.coverage, TOL.vfxAbsByMetric.coverage);
});

test("compare: image-seq のコマ数違い・画像許容差の上限（selfDiff があっても無制限にしない）", () => {
  const mk = (frames) => {
    const d = fs.mkdtempSync(path.join(os.tmpdir(), "sc-")); tmpDirs.push(d);
    fs.writeFileSync(path.join(d, "stage_splash.json"), JSON.stringify({ ok: true, errors: [], notes: [], items: [{ id: "splash_x", kind: "image-seq", count: frames.length, frames }] }));
    return d;
  };
  const f = (h) => ({ file: "n.png", sha256: h });
  assert.ok(compareRuns(mk([f("a"), f("b")]), mk([f("a"), f("b")])).ok);
  const r = compareRuns(mk([f("a"), f("b")]), mk([f("a")]));
  assert.ok(!r.ok);
  assert.ok(r.mismatches.includes("splash/splash_x"));
  assert.ok(TOL.maxDiffRatioCap <= 0.02 && TOL.maxMeanAbsCap <= 1.0);
});
