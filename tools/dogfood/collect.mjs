// ドッグフードの結果の検証・証拠の収集・不具合の採番（W05-38）。依存なし。LLM は呼ばない。
//   node tools/dogfood/collect.mjs validate-checklist [--wave w05|w1 | --file <path>]
//   node tools/dogfood/collect.mjs collect --wave w05 --results <results.json> [--evidence-root <dir>] [--out <dir>] [--checklist <path>]
// 出力: <out>/{results.json, bugs.json, shots/, logs/, json/}。既定の <out> は %USERPROFILE%\dx12-scorecard\dogfood\<wave>\
// 不具合は DF-<WAVE>-NN。再実行では既存の bugs.json を保ち、同じチェックの open の不具合は増やさない。
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
export const SEVERITIES = ["blocker", "major", "minor"];
export const EVIDENCE_KINDS = ["screenshot", "json", "log"];
export const STATUSES = ["pass", "fail", "skip", "blocked"];
const STEP_KINDS = {
  mcp: { req: ["method"], opt: ["params"] },
  input: { req: ["op"], opt: ["params"], ops: ["virtual_input", "find", "pointer", "key", "screenshot"] },
  wait: { req: ["ms"], opt: [] },
  file: { req: ["path", "check"], opt: [], checks: ["exists", "absent", "record_hash", "hash_unchanged", "hash_changed", "contains"] },
  engine: { req: ["action"], opt: ["args"], actions: ["start", "stop", "restart", "kill", "idle_exit", "run"] },
  screenshot: { req: ["id"], opt: [] },
  note: { req: ["text"], opt: [] },
};
// 実マウス・フォーカス・前面化を要求する語（手順の文言にも入れない）
const FORBIDDEN = /SendInput|mouse_event|SetCursorPos|SetForegroundWindow|computer-use|PrintWindow|実マウス|実キーボード|前面化して|フォーカスを(?:移|奪|与)/;

export function validateStep(s, where, errs) {
  if (!s || typeof s !== "object" || Array.isArray(s)) return errs.push(`${where}: 手順がオブジェクトでない`);
  const def = STEP_KINDS[s.kind];
  if (!def) return errs.push(`${where}: 未知の kind ${JSON.stringify(s.kind)}`);
  const allowed = new Set(["kind", ...def.req, ...def.opt]);
  for (const k of def.req) if (s[k] === undefined) errs.push(`${where}: ${s.kind} に ${k} が無い`);
  for (const k of Object.keys(s)) if (!allowed.has(k)) errs.push(`${where}: ${s.kind} に不明なキー ${k}`);
  if (def.ops && !def.ops.includes(s.op)) errs.push(`${where}: input.op が不正 ${s.op}`);
  if (def.checks && !def.checks.includes(s.check)) errs.push(`${where}: file.check が不正 ${s.check}`);
  if (def.actions && !def.actions.includes(s.action)) errs.push(`${where}: engine.action が不正 ${s.action}`);
  if (s.kind === "wait" && !(Number.isInteger(s.ms) && s.ms >= 0)) errs.push(`${where}: wait.ms が非負整数でない`);
  if (s.kind === "engine" && s.args !== undefined && !(Array.isArray(s.args) && s.args.every((a) => typeof a === "string"))) errs.push(`${where}: engine.args が文字列の配列でない`);
  if (FORBIDDEN.test(JSON.stringify(s))) errs.push(`${where}: 実マウス・フォーカスを要求する語が含まれる`);
}

export function validateChecklist(doc) {
  const errs = [];
  if (!doc || typeof doc !== "object") return ["チェックリストがオブジェクトでない"];
  if (typeof doc.wave !== "string" || !doc.wave) errs.push("wave が無い");
  if (!Number.isInteger(doc.version)) errs.push("version が整数でない");
  if (!Array.isArray(doc.checks) || doc.checks.length === 0) return [...errs, "checks が空"];
  const seen = new Set();
  for (const c of doc.checks) {
    const w = `check ${c?.id ?? "?"}`;
    if (!c || typeof c.id !== "string" || !/^[A-Z][0-9]+[a-z]?$/.test(c.id)) { errs.push(`${w}: id の形が不正`); continue; }
    if (seen.has(c.id)) errs.push(`${w}: id が重複`);
    seen.add(c.id);
    for (const k of ["area", "expect"]) if (typeof c[k] !== "string" || !c[k]) errs.push(`${w}: ${k} が無い`);
    if (!SEVERITIES.includes(c.severityIfFail)) errs.push(`${w}: severityIfFail が不正`);
    if (!Array.isArray(c.evidence) || c.evidence.length === 0 || !c.evidence.every((e) => EVIDENCE_KINDS.includes(e))) errs.push(`${w}: evidence が不正`);
    if (!Array.isArray(c.steps) || c.steps.length === 0) errs.push(`${w}: steps が空`);
    else c.steps.forEach((s, i) => validateStep(s, `${w} steps[${i}]`, errs));
    if (FORBIDDEN.test(String(c.expect))) errs.push(`${w}: expect に実マウス・フォーカスを要求する語`);
    if (c.dpi !== undefined && !(Array.isArray(c.dpi) && c.dpi.every((d) => typeof d === "number" && d > 0))) errs.push(`${w}: dpi が不正`);
  }
  for (const c of doc.checks) for (const r of c.reuse ?? []) if (!seen.has(r) && !String(r).match(/^[A-Z][0-9]+[a-z]?$/)) errs.push(`check ${c.id}: reuse が不正 ${r}`);
  return errs;
}

export function validateResults(doc, checklist) {
  const errs = [];
  if (!doc || typeof doc !== "object") return ["結果がオブジェクトでない"];
  if (typeof doc.wave !== "string") errs.push("wave が無い");
  if (!doc.runner || typeof doc.runner.model !== "string" || typeof doc.runner.promptVersion !== "string") errs.push("runner.model / runner.promptVersion が無い");
  if (!Array.isArray(doc.results)) return [...errs, "results が配列でない"];
  const ids = new Set(checklist.checks.map((c) => c.id));
  const seen = new Set();
  for (const r of doc.results) {
    const w = `result ${r?.id ?? "?"}`;
    if (!r || !ids.has(r.id)) { errs.push(`${w}: チェックリストに無い ID`); continue; }
    const key = `${r.id}@${r.dpi ?? ""}`;
    if (seen.has(key)) errs.push(`${w}: 同じ ID と DPI の結果が重複`);
    seen.add(key);
    if (!STATUSES.includes(r.status)) errs.push(`${w}: status が不正`);
    if (r.status === "fail" && !(typeof r.observed === "string" && r.observed)) errs.push(`${w}: fail には observed（実際に起きたこと）が要る`);
    for (const e of r.evidence ?? []) {
      if (!e || !EVIDENCE_KINDS.includes(e.kind) || typeof e.path !== "string" || !e.path) errs.push(`${w}: evidence の形が不正`);
    }
  }
  return errs;
}

export function nextBugNumber(bugs, wave) {
  const re = new RegExp(`^DF-${wave.toUpperCase()}-(\\d+)$`);
  let max = 0;
  for (const b of bugs) { const m = re.exec(b.id); if (m) max = Math.max(max, Number(m[1])); }
  return max + 1;
}

export function makeBugId(wave, n) { return `DF-${wave.toUpperCase()}-${String(n).padStart(2, "0")}`; }

// 失敗した結果から不具合を作る。既存の open な同じ checkId は増やさず、直っていれば verified にする。
export function deriveBugs(checklist, results, existing, nowIso) {
  const byId = new Map(checklist.checks.map((c) => [c.id, c]));
  const bugs = existing.map((b) => ({ ...b }));
  let n = nextBugNumber(bugs, checklist.wave);
  for (const r of results.results) {
    const c = byId.get(r.id);
    const open = bugs.find((b) => b.checkId === r.id && b.status === "open");
    if (r.status === "fail") {
      if (open) continue;
      bugs.push({
        id: makeBugId(checklist.wave, n++), checkId: r.id, severity: c.severityIfFail, area: c.area,
        title: `${r.id}: ${c.title ?? c.expect.slice(0, 60)}`, expected: c.expect, actual: r.observed,
        target: c.target ?? [], evidence: (r.evidence ?? []).map((e) => e.path), status: "open", firstSeen: nowIso,
      });
    } else if (r.status === "pass" && open) {
      open.status = "verified";
    }
  }
  return bugs;
}

function safeName(s) { return String(s).replace(/[^A-Za-z0-9._-]/g, "_"); }

// 証拠を out の下へコピー。root の外・存在しないものは拒む（欠落は missing に記録）。
export function collectEvidence(results, evidenceRoot, outDir) {
  const missing = [];
  const sub = { screenshot: "shots", json: "json", log: "logs" };
  const root = path.resolve(evidenceRoot);
  for (const r of results.results) {
    for (const e of r.evidence ?? []) {
      const abs = path.resolve(root, e.path);
      const rel = path.relative(root, abs);
      if (rel.startsWith("..") || path.isAbsolute(rel)) { missing.push(`${r.id}: 証拠の場所が root の外 ${e.path}`); continue; }
      if (!fs.existsSync(abs) || !fs.statSync(abs).isFile()) { missing.push(`${r.id}: 証拠が無い ${e.path}`); continue; }
      const dir = path.join(outDir, sub[e.kind]);
      fs.mkdirSync(dir, { recursive: true });
      const name = `${safeName(r.id)}${r.dpi ? `_dpi${r.dpi}` : ""}_${safeName(path.basename(abs))}`;
      fs.copyFileSync(abs, path.join(dir, name));
      e.collected = `${sub[e.kind]}/${name}`;
    }
    if (r.status !== "skip") {
      const c = (results._checklistById ?? new Map()).get(r.id);
      if (c && r.status === "pass") {
        for (const k of c.evidence) if (!(r.evidence ?? []).some((e) => e.kind === k)) missing.push(`${r.id}: 必要な証拠 ${k} が無い`);
      }
    }
  }
  return missing;
}

export function defaultOut(wave) { return path.join(process.env.USERPROFILE || os.homedir(), "dx12-scorecard", "dogfood", wave); }

function readJson(p) { return JSON.parse(fs.readFileSync(p, "utf8").replace(/^﻿/, "")); }

export function collect({ checklistPath, resultsPath, evidenceRoot, outDir, now = new Date().toISOString() }) {
  const checklist = readJson(checklistPath);
  const ce = validateChecklist(checklist);
  if (ce.length) throw new Error("チェックリストが不正:\n" + ce.join("\n"));
  const results = readJson(resultsPath);
  const re = validateResults(results, checklist);
  if (re.length) throw new Error("結果が不正:\n" + re.join("\n"));
  if (results.wave !== checklist.wave) throw new Error(`wave が食い違う: ${results.wave} / ${checklist.wave}`);
  fs.mkdirSync(outDir, { recursive: true });
  results._checklistById = new Map(checklist.checks.map((c) => [c.id, c]));
  const missing = collectEvidence(results, evidenceRoot ?? path.dirname(resultsPath), outDir);
  delete results._checklistById;
  const bugsPath = path.join(outDir, "bugs.json");
  const existing = fs.existsSync(bugsPath) ? readJson(bugsPath) : [];
  const bugs = deriveBugs(checklist, results, existing, now);
  // 過去の結果を保ち、再実行したチェックだけ差し替える
  const resPath = path.join(outDir, "results.json");
  let merged = results;
  if (fs.existsSync(resPath)) {
    const old = readJson(resPath);
    const keep = (old.results ?? []).filter((o) => !results.results.some((n) => n.id === o.id && (n.dpi ?? "") === (o.dpi ?? "")));
    merged = { ...results, results: [...keep, ...results.results] };
  }
  fs.writeFileSync(resPath, JSON.stringify(merged, null, 2) + "\n");
  fs.writeFileSync(bugsPath, JSON.stringify(bugs, null, 2) + "\n");
  const summary = {
    wave: checklist.wave,
    pass: merged.results.filter((r) => r.status === "pass").length,
    fail: merged.results.filter((r) => r.status === "fail").length,
    skip: merged.results.filter((r) => r.status === "skip" || r.status === "blocked").length,
    openBugs: bugs.filter((b) => b.status === "open").length,
    openBlockerMajor: bugs.filter((b) => b.status === "open" && b.severity !== "minor").length,
    missingEvidence: missing,
  };
  return summary;
}

function arg(args, name) { const i = args.indexOf(name); return i >= 0 ? args[i + 1] : undefined; }

function main(args) {
  const cmd = args[0];
  const wave = arg(args, "--wave");
  const checklistPath = arg(args, "--checklist") ?? arg(args, "--file") ?? (wave ? path.join(here, "checklists", `${wave}.json`) : undefined);
  if (cmd === "validate-checklist") {
    const files = checklistPath ? [checklistPath] : ["w05", "w1"].map((w) => path.join(here, "checklists", `${w}.json`));
    let bad = 0;
    for (const f of files) {
      const errs = validateChecklist(readJson(f));
      console.log(`${path.basename(f)}: ${errs.length ? "NG" : "OK"}`);
      for (const e of errs) console.log("  " + e);
      bad += errs.length;
    }
    return bad ? 1 : 0;
  }
  if (cmd === "collect") {
    const resultsPath = arg(args, "--results");
    if (!wave || !resultsPath) { console.error("--wave と --results が要る"); return 2; }
    const s = collect({ checklistPath, resultsPath, evidenceRoot: arg(args, "--evidence-root"), outDir: arg(args, "--out") ?? defaultOut(wave) });
    console.log(JSON.stringify(s, null, 2));
    return s.openBlockerMajor > 0 || s.missingEvidence.length > 0 ? 1 : 0;
  }
  console.error("使い方: collect.mjs validate-checklist [--wave w05|w1] | collect --wave <w> --results <file> [--evidence-root d] [--out d]");
  return 2;
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try { process.exitCode = main(process.argv.slice(2)); } catch (e) { console.error(String(e?.message ?? e)); process.exitCode = 2; }
}
